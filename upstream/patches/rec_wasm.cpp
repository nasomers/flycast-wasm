// rec_wasm.cpp — WASM JIT backend for Flycast SH4 dynarec
//
// Phase 2: Compiles SHIL basic blocks into WebAssembly functions at runtime.
// Each block becomes a WASM module with one exported function that:
//   - Operates on Sh4Context in shared Emscripten linear memory
//   - Calls imported functions for memory I/O and interpreter fallback
//   - Sets next PC and returns (mainloop handles dispatch)
//
// This file is compiled when FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_GENERIC
// (set in build.h for __EMSCRIPTEN__).

#include "build.h"

#if FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_GENERIC

#include "types.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/sh4/dyna/decoder.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"
#include "oslib/virtmem.h"

#include "wasm_module_builder.h"
#include "wasm_emit.h"

#include <unordered_map>
#include <cmath>
#include <cstring>

#include "hw/sh4/sh4_rom.h"  // sin_table for FSCA
#include "hw/pvr/pvr_regs.h"  // PVR register macros (FB_R_CTRL etc.)

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// Naomi serial EEPROM diagnostic counters (defined in naomi.cpp)
extern u32 g_naomi_board_write_count;
extern u32 g_naomi_board_read_count;

// Verify Sh4Context offsets used in wasm_emit.h
static_assert(offsetof(Sh4Context, pc) == 0x148, "PC offset mismatch");
static_assert(offsetof(Sh4Context, jdyn) == 0x14C, "jdyn offset mismatch");
static_assert(offsetof(Sh4Context, sr.T) == 0x154, "sr.T offset mismatch");
static_assert(offsetof(Sh4Context, cycle_counter) == 0x174, "cycle_counter offset mismatch");

// Forward declarations from driver.cpp
DynarecCodeEntryPtr DYNACALL rdv_FailedToFindBlock(u32 pc);

// Forward declarations for EM_JS functions (defined later, after extern "C" block)
#ifdef __EMSCRIPTEN__
extern "C" {
int wasm_compile_block(const u8* bytesPtr, u32 len, u32 block_pc);
int wasm_execute_block(u32 block_pc, u32 ctx_ptr);
int wasm_has_block(u32 block_pc);
void wasm_clear_cache();
void wasm_remove_block(u32 block_pc);
int wasm_cache_size();
}
#endif

// ============================================================
// Block info storage for SHIL fallback
// ============================================================
static std::unordered_map<u32, RuntimeBlockInfo*> blockByVaddr;
// SMC (self-modifying code) detection: store first word of block code
static std::unordered_map<u32, u32> blockCodeHash;

// ============================================================
// Deferred exception handling for shop_ifb
// ============================================================
// When an SH4 exception occurs inside a shop_ifb handler, we can't call
// Do_Exception immediately because the WASM block exit would overwrite
// the exception vector PC. Instead, we save the exception info and defer
// the Do_Exception call until after the WASM block finishes.
static bool g_ifb_exception_pending = false;
static u32 g_ifb_exception_epc = 0;
static Sh4ExceptionCode g_ifb_exception_expEvn = (Sh4ExceptionCode)0;

// ============================================================
// SHIL dry-run write trace (for memory write comparison)
// ============================================================
struct ShilWriteEntry {
	u32 addr;
	u32 size;
	u32 val_lo;  // low 32 bits (or full value for size<=4)
	u32 val_hi;  // high 32 bits for size==8
};
static std::vector<ShilWriteEntry> g_shil_writes;
static bool g_shil_dry_run = false;

// Write/read counters for diagnostic modes
static u32 g_shil_write_count = 0;
static u32 g_shil_read_count = 0;
static u32 g_shil_pvr_write_count = 0;
static u32 g_shil_sq_write_count = 0;

// ============================================================
// Memory access cycle penalties — approximates Sh4Cycles
// ============================================================
// The SH4 interpreter charges dynamic cycle penalties for memory accesses
// via Sh4Cycles::addReadAccessCycles/addWriteAccessCycles (called from
// sh4_cache.h during cache fills and uncached accesses). WASM compiled
// blocks bypass the cache, so we add approximate penalties here.
//
// SH4 address space regions:
//   P1 (0x80-0x9F): cached, physical = addr & 0x1FFFFFFF
//   P2 (0xA0-0xBF): uncached, physical = addr & 0x1FFFFFFF
//   P3 (0xC0-0xDF): cached via TLB
//   P4 (0xE0-0xFF): SH4 internal registers (0 penalty)
//
// Physical area mapping (bits 28:26):
//   Area 0 (0x00-0x03): ROM, Flash, Holly/PVR MMIO, AICA
//   Area 1 (0x04-0x07): VRAM
//   Area 3 (0x0C-0x0F): System RAM (most common)
//   Area 7 (0x1C-0x1F): SH4 on-chip registers
//
// Penalty values are internal (200 MHz) cycles, matching Sh4Cycles
// formula: readExternalAccessCycles(addr, size) * 2 * cpuRatio.
// For cached RAM, we use a low average to model the cache hit rate.
// Memory cycle penalties — DISABLED
// Shadow comparison proved: SHIL ops produce identical register state to ref.
// The only divergence is cycle_counter. The x64 JIT charges only guest_cycles
// (no extra memory penalties) and works correctly. We do the same.
// Penalties are no-ops to match the x64 JIT's cycle counting approach.
static inline void addMemReadPenalty(u32 addr, u32 size) {
	(void)addr; (void)size;
}

static inline void addMemWritePenalty(u32 addr, u32 size) {
	(void)addr; (void)size;
}

// ============================================================
// C-linkage wrapper functions for WASM imports
// ============================================================

extern "C" {

u32 EMSCRIPTEN_KEEPALIVE wasm_mem_read8(u32 addr) {
	addMemReadPenalty(addr, 1);
	return (u32)(s32)(s8)ReadMem8(addr);  // sign-extend (matches SHIL convention)
}

u32 EMSCRIPTEN_KEEPALIVE wasm_mem_read16(u32 addr) {
	addMemReadPenalty(addr, 2);
	return (u32)(s32)(s16)ReadMem16(addr);  // sign-extend (matches SHIL convention)
}

u32 EMSCRIPTEN_KEEPALIVE wasm_mem_read32(u32 addr) {
	addMemReadPenalty(addr, 4);
	return ReadMem32(addr);
}

void EMSCRIPTEN_KEEPALIVE wasm_mem_write8(u32 addr, u32 val) {
	addMemWritePenalty(addr, 1);
	WriteMem8(addr, (u8)val);
}

void EMSCRIPTEN_KEEPALIVE wasm_mem_write16(u32 addr, u32 val) {
	addMemWritePenalty(addr, 2);
	WriteMem16(addr, (u16)val);
}

void EMSCRIPTEN_KEEPALIVE wasm_mem_write32(u32 addr, u32 val) {
	addMemWritePenalty(addr, 4);
	WriteMem32(addr, val);
}

void EMSCRIPTEN_KEEPALIVE wasm_exec_ifb(u32 opcode, u32 pc) {
	(void)pc;
	OpPtr[opcode](&Sh4cntx, opcode);
}

// Forward declaration needed for per-op tracing diagnostic
extern u32 g_wasm_block_count;

// Runtime SHIL op interpreter — executes a single SHIL op by reading
// register values from Sh4Context, performing the operation, and writing
// results back. Used for ops that the WASM emitter doesn't handle natively.
static u32 g_shil_fb_call_count = 0;
static u32 g_shil_fb_miss_count = 0;

void EMSCRIPTEN_KEEPALIVE wasm_exec_shil_fb(u32 block_vaddr, u32 op_index) {
	g_shil_fb_call_count++;
	// Skip remaining ops after a deferred exception (block should abort)
	if (g_ifb_exception_pending) return;

	auto it = blockByVaddr.find(block_vaddr);
	if (it == blockByVaddr.end()) {
		g_shil_fb_miss_count++;
#ifdef __EMSCRIPTEN__
		if (g_shil_fb_miss_count <= 20) {
			EM_ASM({ console.log('[SHIL-FB-MISS] #' + $0 +
				' vaddr=0x' + ($1>>>0).toString(16) +
				' op_idx=' + $2); },
				g_shil_fb_miss_count, block_vaddr, op_index);
		}
#endif
		return;
	}
	RuntimeBlockInfo* block = it->second;
	if (op_index >= block->oplist.size()) return;

	shil_opcode& op = block->oplist[op_index];
	Sh4Context& ctx = Sh4cntx;

	// Helper lambdas to read/write params
	auto readI32 = [&](const shil_param& p) -> u32 {
		if (p.is_imm()) return p._imm;
		if (p.is_reg()) return *(u32*)((u8*)&ctx + p.reg_offset());
		return 0;
	};
	auto readF32 = [&](const shil_param& p) -> float {
		if (p.is_reg()) return *(float*)((u8*)&ctx + p.reg_offset());
		return 0.0f;
	};
	auto writeI32 = [&](const shil_param& p, u32 val) {
		if (p.is_reg()) *(u32*)((u8*)&ctx + p.reg_offset()) = val;
	};
	auto writeF32 = [&](const shil_param& p, float val) {
		if (p.is_reg()) *(float*)((u8*)&ctx + p.reg_offset()) = val;
	};

	// Per-op trace for diverging block #2360476 at pc=0x8c00b8e4
#ifdef __EMSCRIPTEN__
	// BLK-DETAIL #2360476 = execution count 2360475 (post-increment offset)
	// The diverging block starts at pc=0x8c00b996 (from BLK-DETAIL #2360475 next-pc)
	bool trace_this = (g_wasm_block_count == 2360475 && block_vaddr == 0x8c00b996);
	u32 r0_before = ctx.r[0];
	(void)r0_before;
	if (trace_this) {
		// On first op, dump block info
		if (op_index == 0) {
			EM_ASM({ console.log('[OP-TRACE] === Block #2360476 pc=0x8c00b8e4 nops=' + $0); },
				(u32)block->oplist.size());
		}
	}
#endif

	switch (op.op) {
	case shop_sync_sr:
		UpdateSR();
		break;
	case shop_sync_fpscr:
		Sh4Context::UpdateFPSCR(&ctx);
		break;
	case shop_pref: {
		u32 addr = readI32(op.rs1);
		if ((addr >> 26) == 0x38) {
			g_shil_sq_write_count++;
#ifdef __EMSCRIPTEN__
			if (g_shil_sq_write_count <= 20) {
				EM_ASM({ console.log('[SQ-WR] #' + $0 +
					' addr=0x' + ($1>>>0).toString(16)); },
					g_shil_sq_write_count, addr);
			}
#endif
			ctx.doSqWrite(addr, &ctx);
		}
		break;
	}
	// Integer ops with carry (64-bit result in rd, rd2)
	case shop_adc: {
		u32 a = readI32(op.rs1), b = readI32(op.rs2), c = readI32(op.rs3);
		u64 res = (u64)a + (u64)b + (u64)c;
		writeI32(op.rd, (u32)res);
		writeI32(op.rd2, (u32)(res >> 32));
		break;
	}
	case shop_sbc: {
		u32 a = readI32(op.rs1), b = readI32(op.rs2), c = readI32(op.rs3);
		u64 res = (u64)a - (u64)b - (u64)c;
		writeI32(op.rd, (u32)res);
		writeI32(op.rd2, res >> 63);
		break;
	}
	case shop_negc: {
		u32 a = readI32(op.rs1), c = readI32(op.rs2);
		u64 res = 0ULL - (u64)a - (u64)c;
		writeI32(op.rd, (u32)res);
		writeI32(op.rd2, res >> 63);
		break;
	}
	case shop_rocl: {
		u32 val = readI32(op.rs1), carry = readI32(op.rs2);
		u32 newCarry = val >> 31;
		writeI32(op.rd, (val << 1) | (carry & 1));
		writeI32(op.rd2, newCarry);
		break;
	}
	case shop_rocr: {
		u32 val = readI32(op.rs1), carry = readI32(op.rs2);
		u32 newCarry = val & 1;
		writeI32(op.rd, (val >> 1) | ((carry & 1) << 31));
		writeI32(op.rd2, newCarry);
		break;
	}
	case shop_shld: {
		u32 val = readI32(op.rs1);
		s32 shift = (s32)readI32(op.rs2);
		if (shift >= 0)
			writeI32(op.rd, val << (shift & 0x1F));
		else if ((shift & 0x1F) == 0)
			writeI32(op.rd, 0);
		else
			writeI32(op.rd, val >> ((-shift) & 0x1F));
		break;
	}
	case shop_shad: {
		s32 val = (s32)readI32(op.rs1);
		s32 shift = (s32)readI32(op.rs2);
		if (shift >= 0)
			writeI32(op.rd, (u32)(val << (shift & 0x1F)));
		else if ((shift & 0x1F) == 0)
			writeI32(op.rd, (u32)(val >> 31));
		else
			writeI32(op.rd, (u32)(val >> ((-shift) & 0x1F)));
		break;
	}
	case shop_mul_u64: {
		u64 res = (u64)readI32(op.rs1) * (u64)readI32(op.rs2);
		writeI32(op.rd, (u32)res);
		writeI32(op.rd2, (u32)(res >> 32));
		break;
	}
	case shop_mul_s64: {
		s64 res = (s64)(s32)readI32(op.rs1) * (s64)(s32)readI32(op.rs2);
		writeI32(op.rd, (u32)res);
		writeI32(op.rd2, (u32)((u64)res >> 32));
		break;
	}
	case shop_setpeq: {
		u32 a = readI32(op.rs1), b = readI32(op.rs2);
		u32 xor_val = a ^ b;
		u32 result = ((xor_val & 0xFF000000) == 0) || ((xor_val & 0x00FF0000) == 0) ||
		             ((xor_val & 0x0000FF00) == 0) || ((xor_val & 0x000000FF) == 0);
		writeI32(op.rd, result);
		break;
	}
	// FPU ops
	case shop_fmac: {
		float fn = readF32(op.rs1), f0 = readF32(op.rs2), fm = readF32(op.rs3);
		writeF32(op.rd, std::fma(f0, fm, fn));
		break;
	}
	case shop_fsrra: {
		float val = readF32(op.rs1);
		writeF32(op.rd, 1.0f / sqrtf(val));
		break;
	}
	case shop_fipr: {
		// 4-element dot product with double accumulation (matches canonical)
		u32 off1 = op.rs1.reg_offset(), off2 = op.rs2.reg_offset();
		double sum = 0;
		for (int i = 0; i < 4; i++) {
			float a = *(float*)((u8*)&ctx + off1 + i * 4);
			float b = *(float*)((u8*)&ctx + off2 + i * 4);
			sum += (double)a * (double)b;
		}
		writeF32(op.rd, (float)sum);
		break;
	}
	case shop_ftrv: {
		// 4x4 matrix * 4-element vector with double accumulation (matches canonical)
		u32 voff = op.rs1.reg_offset(), moff = op.rs2.reg_offset();
		u32 doff = op.rd.reg_offset();
		for (int i = 0; i < 4; i++) {
			double sum = 0;
			for (int j = 0; j < 4; j++) {
				float m = *(float*)((u8*)&ctx + moff + (j * 4 + i) * 4);
				float v = *(float*)((u8*)&ctx + voff + j * 4);
				sum += (double)m * (double)v;
			}
			*(float*)((u8*)&ctx + doff + i * 4) = (float)sum;
		}
		break;
	}
	case shop_frswap: {
		u32 off1 = op.rs1.reg_offset(), off2 = op.rd.reg_offset();
		for (int i = 0; i < 16; i++) {
			u32* a = (u32*)((u8*)&ctx + off1 + i * 4);
			u32* b = (u32*)((u8*)&ctx + off2 + i * 4);
			u32 tmp = *a; *a = *b; *b = tmp;
		}
		break;
	}
	case shop_fsca: {
		u32 angle = readI32(op.rs1);
		u32 pi_index = angle & 0xFFFF;
		u32 doff = op.rd.reg_offset();
		*(float*)((u8*)&ctx + doff) = sin_table[pi_index].u[0];
		*(float*)((u8*)&ctx + doff + 4) = sin_table[pi_index].u[1];
		break;
	}
	// ---- Tier 1/2 basic ops (needed when WASM emitters are disabled for debugging) ----
	case shop_mov32:
		writeI32(op.rd, readI32(op.rs1));
		break;
	case shop_mov64: {
		u32 soff = op.rs1.reg_offset(), doff = op.rd.reg_offset();
		*(u32*)((u8*)&ctx + doff) = *(u32*)((u8*)&ctx + soff);
		*(u32*)((u8*)&ctx + doff + 4) = *(u32*)((u8*)&ctx + soff + 4);
		break;
	}
	case shop_add:
		writeI32(op.rd, readI32(op.rs1) + readI32(op.rs2));
		break;
	case shop_sub:
		writeI32(op.rd, readI32(op.rs1) - readI32(op.rs2));
		break;
	case shop_and:
		writeI32(op.rd, readI32(op.rs1) & readI32(op.rs2));
		break;
	case shop_or:
		writeI32(op.rd, readI32(op.rs1) | readI32(op.rs2));
		break;
	case shop_xor:
		writeI32(op.rd, readI32(op.rs1) ^ readI32(op.rs2));
		break;
	case shop_not:
		writeI32(op.rd, ~readI32(op.rs1));
		break;
	case shop_neg:
		writeI32(op.rd, (u32)(-(s32)readI32(op.rs1)));
		break;
	case shop_shl:
		writeI32(op.rd, readI32(op.rs1) << (readI32(op.rs2) & 0x1F));
		break;
	case shop_shr:
		writeI32(op.rd, readI32(op.rs1) >> (readI32(op.rs2) & 0x1F));
		break;
	case shop_sar:
		writeI32(op.rd, (u32)((s32)readI32(op.rs1) >> (readI32(op.rs2) & 0x1F)));
		break;
	case shop_ror: {
		u32 v = readI32(op.rs1), s = readI32(op.rs2) & 0x1F;
		writeI32(op.rd, (v >> s) | (v << (32 - s)));
		break;
	}
	case shop_ext_s8:
		writeI32(op.rd, (u32)(s32)(s8)(readI32(op.rs1) & 0xFF));
		break;
	case shop_ext_s16:
		writeI32(op.rd, (u32)(s32)(s16)(readI32(op.rs1) & 0xFFFF));
		break;
	case shop_mul_u16:
		writeI32(op.rd, (readI32(op.rs1) & 0xFFFF) * (readI32(op.rs2) & 0xFFFF));
		break;
	case shop_mul_s16:
		writeI32(op.rd, (u32)((s32)(s16)(readI32(op.rs1) & 0xFFFF) * (s32)(s16)(readI32(op.rs2) & 0xFFFF)));
		break;
	case shop_mul_i32:
		writeI32(op.rd, readI32(op.rs1) * readI32(op.rs2));
		break;
	case shop_test:
		writeI32(op.rd, (readI32(op.rs1) & readI32(op.rs2)) == 0 ? 1 : 0);
		break;
	case shop_seteq:
		writeI32(op.rd, (readI32(op.rs1) == readI32(op.rs2)) ? 1 : 0);
		break;
	case shop_setge:
		writeI32(op.rd, (s32)readI32(op.rs1) >= (s32)readI32(op.rs2) ? 1 : 0);
		break;
	case shop_setgt:
		writeI32(op.rd, (s32)readI32(op.rs1) > (s32)readI32(op.rs2) ? 1 : 0);
		break;
	case shop_setae:
		writeI32(op.rd, readI32(op.rs1) >= readI32(op.rs2) ? 1 : 0);
		break;
	case shop_setab:
		writeI32(op.rd, readI32(op.rs1) > readI32(op.rs2) ? 1 : 0);
		break;
	case shop_jdyn: {
		u32 val = readI32(op.rs1);
		if (!op.rs2.is_null()) val += readI32(op.rs2);
		ctx.jdyn = val;
		break;
	}
	case shop_jcond:
		ctx.sr.T = readI32(op.rs1);
		break;
	case shop_readm: {
		u32 addr = readI32(op.rs1);
		if (!op.rs3.is_null()) addr += readI32(op.rs3);
		addMemReadPenalty(addr, op.size);
		g_shil_read_count++;
		if (op.size == 8) {
			u32 doff = op.rd.reg_offset();
			*(u32*)((u8*)&ctx + doff) = ReadMem32(addr);
			*(u32*)((u8*)&ctx + doff + 4) = ReadMem32(addr + 4);
		} else if (op.size == 1) {
			writeI32(op.rd, (u32)(s32)(s8)ReadMem8(addr));
		} else if (op.size == 2) {
			writeI32(op.rd, (u32)(s32)(s16)ReadMem16(addr));
		} else {
			writeI32(op.rd, ReadMem32(addr));
		}
		break;
	}
	case shop_writem: {
		u32 addr = readI32(op.rs1);
		if (!op.rs3.is_null()) addr += readI32(op.rs3);
		addMemWritePenalty(addr, op.size);
		g_shil_write_count++;
		// Track PVR MMIO writes (physical 0x005F8000-0x005F9FFF)
		{
			u32 phys = addr & 0x1FFFFFFF;
			if (phys >= 0x005F8000 && phys <= 0x005F9FFF) {
				g_shil_pvr_write_count++;
				u32 val = 0;
				if (op.size == 8) {
					val = *(u32*)((u8*)&ctx + op.rs2.reg_offset());
				} else {
					val = readI32(op.rs2);
				}
#ifdef __EMSCRIPTEN__
				// Always log writes to critical PVR registers
				bool is_critical = (phys == 0x005F8044) || // FB_R_CTRL
				                   (phys == 0x005F8048) || // FB_W_CTRL
				                   (phys == 0x005F8014) || // STARTRENDER
				                   (phys == 0x005F8060) || // FB_W_SOF1
				                   (phys == 0x005F8064) || // FB_W_SOF2
				                   (phys == 0x005F8050) || // FB_R_SOF1
				                   (phys == 0x005F8054);   // FB_R_SOF2
				if (g_shil_pvr_write_count <= 50 || is_critical) {
					EM_ASM({ console.log('[PVR-WR] #' + $0 +
						' blk=' + $4 +
						' addr=0x' + ($1>>>0).toString(16) +
						' val=0x' + ($2>>>0).toString(16) +
						' size=' + $3); },
						g_shil_pvr_write_count, addr, val, op.size, g_wasm_block_count);
				}
#endif
			}
		}
		if (g_shil_dry_run) {
			// Dry run: capture intended writes, don't apply
			ShilWriteEntry e;
			e.addr = addr;
			e.size = op.size;
			if (op.size == 8) {
				u32 soff = op.rs2.reg_offset();
				e.val_lo = *(u32*)((u8*)&ctx + soff);
				e.val_hi = *(u32*)((u8*)&ctx + soff + 4);
			} else {
				e.val_lo = readI32(op.rs2);
				e.val_hi = 0;
			}
			g_shil_writes.push_back(e);
		} else {
			if (op.size == 8) {
				u32 soff = op.rs2.reg_offset();
				WriteMem32(addr, *(u32*)((u8*)&ctx + soff));
				WriteMem32(addr + 4, *(u32*)((u8*)&ctx + soff + 4));
			} else if (op.size == 1) {
				WriteMem8(addr, (u8)readI32(op.rs2));
			} else if (op.size == 2) {
				WriteMem16(addr, (u16)readI32(op.rs2));
			} else {
				WriteMem32(addr, readI32(op.rs2));
			}
		}
		break;
	}
	case shop_ifb: {
		if (op.rs1._imm)
			ctx.pc = op.rs2._imm;
		// Let exceptions propagate to mainloop (matching ref_execute_block behavior).
		// In ref, exceptions from OpPtr propagate to the mainloop catch, which calls
		// Do_Exception and adds +5 to cycle_counter. Deferred exception handling
		// (g_ifb_exception_pending) caused behavioral differences because remaining
		// SHIL ops continued executing after the exception.
		if (ctx.sr.FD == 1 && OpDesc[op.rs3._imm]->IsFloatingPoint())
			throw SH4ThrownException(ctx.pc - 2, Sh4Ex_FpuDisabled);
		OpPtr[op.rs3._imm](&ctx, op.rs3._imm);
		break;
	}
	case shop_swaplb: {
		u32 v = readI32(op.rs1);
		writeI32(op.rd, ((v >> 8) & 0xFF) | ((v & 0xFF) << 8) | (v & 0xFFFF0000));
		break;
	}
	case shop_xtrct:
		writeI32(op.rd, (readI32(op.rs1) >> 16) | (readI32(op.rs2) << 16));
		break;
	// FPU basic ops
	case shop_fadd:
		writeF32(op.rd, readF32(op.rs1) + readF32(op.rs2));
		break;
	case shop_fsub:
		writeF32(op.rd, readF32(op.rs1) - readF32(op.rs2));
		break;
	case shop_fmul:
		writeF32(op.rd, readF32(op.rs1) * readF32(op.rs2));
		break;
	case shop_fdiv:
		writeF32(op.rd, readF32(op.rs1) / readF32(op.rs2));
		break;
	case shop_fabs:
		writeF32(op.rd, fabsf(readF32(op.rs1)));
		break;
	case shop_fneg:
		writeF32(op.rd, -readF32(op.rs1));
		break;
	case shop_fsqrt:
		writeF32(op.rd, sqrtf(readF32(op.rs1)));
		break;
	case shop_fseteq:
		writeI32(op.rd, readF32(op.rs1) == readF32(op.rs2) ? 1 : 0);
		break;
	case shop_fsetgt:
		writeI32(op.rd, readF32(op.rs1) > readF32(op.rs2) ? 1 : 0);
		break;
	case shop_cvt_f2i_t: {
		float fval = readF32(op.rs1);
		s32 res;
		if (fval > 2147483520.0f) {
			res = 0x7fffffff;
		} else {
			res = (s32)fval;
			if (std::isnan(fval))
				res = (s32)0x80000000;
		}
		writeI32(op.rd, (u32)res);
		break;
	}
	case shop_cvt_i2f_n:
	case shop_cvt_i2f_z:
		writeF32(op.rd, (float)(s32)readI32(op.rs1));
		break;
	case shop_div1: {
		// SH4 DIV1 — single-step division (matches shil_canonical.h exactly)
		u32 a = readI32(op.rs1);
		s32 b = (s32)readI32(op.rs2);
		u32 T = readI32(op.rs3);
		bool qxm = ctx.sr.Q ^ ctx.sr.M;
		ctx.sr.Q = (int)a < 0;
		a = (a << 1) | T;
		u32 oldA = a;
		a += (qxm ? 1 : -1) * b;
		ctx.sr.Q ^= ctx.sr.M ^ (qxm ? a < oldA : a > oldA);
		T = !(ctx.sr.Q ^ ctx.sr.M);
		writeI32(op.rd, a);
		writeI32(op.rd2, T);
		break;
	}
	case shop_div32u: {
		// Unsigned 64/32 division
		u32 r1 = readI32(op.rs1), r2 = readI32(op.rs2), r3 = readI32(op.rs3);
		u64 dividend = ((u64)r3 << 32) | r1;
		u32 quo = r2 ? (u32)(dividend / r2) : 0;
		u32 rem = r2 ? (u32)(dividend % r2) : (u32)dividend;
		writeI32(op.rd, quo);
		writeI32(op.rd2, rem);
		break;
	}
	case shop_div32s: {
		// Signed 64/32 division
		u32 r1 = readI32(op.rs1);
		s32 r2 = (s32)readI32(op.rs2);
		s32 r3 = (s32)readI32(op.rs3);
		s64 dividend = ((s64)r3 << 32) | r1;
		if (dividend < 0) dividend++;  // 1's complement → 2's complement
		s32 quo = r2 ? (s32)(dividend / r2) : 0;
		s32 rem = (s32)(dividend - (s64)quo * r2);
		u32 negative = ((u32)r3 ^ (u32)r2) & 0x80000000;
		if (negative) quo--;
		else if (r3 < 0) rem--;
		writeI32(op.rd, (u32)quo);
		writeI32(op.rd2, (u32)rem);
		break;
	}
	case shop_div32p2: {
		// Division fixup step
		s32 a = (s32)readI32(op.rs1);
		s32 b = (s32)readI32(op.rs2);
		u32 T = readI32(op.rs3);
		if (!(T & 0x80000000)) {
			if (!(T & 1)) a -= b;
		} else {
			if (b > 0) a--;
			if (T & 1) a += b;
		}
		writeI32(op.rd, (u32)a);
		break;
	}
	default:
		// Unknown op — log and skip
#ifdef __EMSCRIPTEN__
		static int unhandledCount = 0;
		unhandledCount++;
		if (unhandledCount <= 20) {
			EM_ASM({ console.warn('[rec_wasm] unhandled SHIL fallback op=' + $0 + ' at block 0x' + ($1>>>0).toString(16)); },
				(int)op.op, block_vaddr);
		}
#endif
		break;
	}

#ifdef __EMSCRIPTEN__
	if (trace_this) {
		u32 r0_after = ctx.r[0];
		// Log: op index, op type, r0 before/after
		// Also log rd target offset and rs1 details for reads
		u32 rd_off = op.rd.is_reg() ? op.rd.reg_offset() : 0xFFFF;
		u32 rs1_val = op.rs1.is_reg() ? *(u32*)((u8*)&ctx + op.rs1.reg_offset()) : (op.rs1.is_imm() ? op.rs1._imm : 0);
		bool r0_changed = (r0_before != r0_after);
		EM_ASM({ console.log('[OP-TRACE] i=' + $0 +
			' op=' + $1 +
			' sz=' + $2 +
			' r0=' + ($3 ? 'CHANGED' : 'same') +
			' r0_b=0x' + ($4>>>0).toString(16) +
			' r0_a=0x' + ($5>>>0).toString(16) +
			' rd_off=0x' + ($6>>>0).toString(16) +
			' rs1_val=0x' + ($7>>>0).toString(16)); },
			op_index, (int)op.op, (int)op.size,
			r0_changed ? 1 : 0,
			r0_before, r0_after,
			rd_off, rs1_val);
	}
#endif
}

} // extern "C"

// ============================================================
// Per-instruction block executor — executes raw SH4 instructions
// Uses OpPtr directly (same as interpreter), but in block batches.
// Follows PC after each instruction to handle branches properly
// (branch handlers execute delay slot internally via executeDelaySlot).
// ============================================================
// Forward declaration — defined later but needed by ref_execute_block
extern u32 g_wasm_block_count;

// EXECUTOR_MODE must be defined BEFORE ref_execute_block so that
// #if EXECUTOR_MODE == 0 inside the function evaluates correctly.
// Previously it was defined AFTER, causing undefined-macro = 0 = TRUE,
// which made per-instruction cycle charging always active in ref_execute_block.
#define EXECUTOR_MODE 1
#define SHIL_START_BLOCK 2360000

// Reference executor: per-instruction via OpPtr
// Per-instruction cycle counting (1 per instruction executed)
// Does NOT follow branches within blocks — exits at first branch
// to match JIT dispatch model
static u32 ref_call_count = 0;
static void ref_execute_block(RuntimeBlockInfo* block) {
	Sh4Context& ctx = Sh4cntx;
	int cc_at_entry = ctx.cycle_counter;
	ctx.pc = block->vaddr;
	u32 block_end = block->vaddr + block->sh4_code_size;
	u32 maxInstrs = block->guest_opcodes + 1;
	u32 actual_iters = 0;
	for (u32 n = 0; n < maxInstrs; n++) {
		u32 pc = ctx.pc;
		if (pc < block->vaddr || pc >= block_end) break;
		ctx.pc = pc + 2;
		u16 op = IReadMem16(pc);
		if (ctx.sr.FD == 1 && OpDesc[op]->IsFloatingPoint()) {
			Do_Exception(pc, Sh4Ex_FpuDisabled);
			return;
		}
		actual_iters++;
		OpPtr[op](&ctx, op);
		// Per-instruction cycle charging — only active in mode 0 (pure ref).
		// EXECUTOR_MODE is defined above this function.
#if EXECUTOR_MODE == 0
		ctx.cycle_counter -= 1;
#endif
		if (ctx.pc != (pc + 2) && ctx.pc != (pc + 4)) {
			break;
		}
	}
#ifdef __EMSCRIPTEN__
	if (ref_call_count < 5) {
		int cc_at_exit = ctx.cycle_counter;
		EM_ASM({ console.log('[REF-BLOCK] #' + $0 +
			' cc_entry=' + $1 +
			' cc_exit=' + $2 +
			' delta=' + ($1 - $2) +
			' iters=' + $3 +
			' go=' + $4); },
			ref_call_count, cc_at_entry, cc_at_exit,
			actual_iters, block->guest_opcodes);
	}
#endif
	ref_call_count++;
}

// C++ block exit logic (matches emitBlockExit / driver.cpp)
static void applyBlockExitCpp(RuntimeBlockInfo* block) {
	Sh4Context& ctx = Sh4cntx;
	u32 bcls = BET_GET_CLS(block->BlockType);
	switch (bcls) {
	case BET_CLS_Static:
		if (block->BlockType == BET_StaticIntr)
			ctx.pc = block->NextBlock;
		else
			ctx.pc = block->BranchBlock;
		break;
	case BET_CLS_Dynamic:
		ctx.pc = ctx.jdyn;
		break;
	case BET_CLS_COND:
		if (block->BlockType == BET_Cond_1)
			ctx.pc = ctx.sr.T ? block->BranchBlock : block->NextBlock;
		else // BET_Cond_0
			ctx.pc = ctx.sr.T ? block->NextBlock : block->BranchBlock;
		break;
	}
}

// === MODE SWITCH (defined above ref_execute_block) ===
// 0 = ref (per-instruction charging)
// 1 = SHIL with guest_offs-based per-instruction charging
// 4 = ref execution + SHIL-style charging
// 5 = shadow comparison
// 6 = pure WASM execution

static u32 pc_hash = 0;
u32 g_wasm_block_count = 0;  // global so pvr_regs.cpp can reference it
static u32 state_hash = 0;
static u32 state_hash2 = 0;
static u32 state_hash3 = 0;
static int cc_leak_total = 0;      // cumulative unexpected cc delta
static u32 cc_leak_count = 0;      // number of blocks with leaks
static u32 cc_leak_logged = 0;     // number of leak events logged (cap at 50)
#if EXECUTOR_MODE == 5
static u32 shadow_mismatch_count = 0;
static u32 shadow_match_count = 0;
#endif

static void cpp_execute_block(RuntimeBlockInfo* block) {
	Sh4Context& ctx = Sh4cntx;

#ifdef __EMSCRIPTEN__
	// Per-block trace for first 500 blocks (compare between modes)
	if (g_wasm_block_count < 500) {
		EM_ASM({ console.log('[BLK] #' + $0 +
			' pc=0x' + ($1>>>0).toString(16) +
			' cc=' + ($2|0) +
			' go=' + $3 + ' gc=' + $7 +
			' r0=0x' + ($4>>>0).toString(16) +
			' r4=0x' + ($5>>>0).toString(16) +
			' T=' + $6); },
			g_wasm_block_count, block->vaddr, ctx.cycle_counter,
			block->guest_opcodes, ctx.r[0], ctx.r[4], ctx.sr.T,
			block->guest_cycles);
	}
#endif

#if EXECUTOR_MODE == 0
	// REF executor (per-instruction charging)
	ref_execute_block(block);
#elif EXECUTOR_MODE == 3
	// HYBRID: ref (mode 4 path) for early blocks, SHIL (mode 1 path) after threshold.
	// Both paths use guest_cycles charging with forced reset.
	// This isolates whether SHIL works correctly when starting from known-good state.
	if (g_wasm_block_count < SHIL_START_BLOCK) {
		// Mode 4 ref path (known PASS): guest_cycles + ref_execute_block + forced reset
		int cc_pre = ctx.cycle_counter;
		ctx.cycle_counter -= block->guest_cycles;
		ref_execute_block(block);
		ctx.cycle_counter = cc_pre - (int)block->guest_cycles;
		// Use ref's own PC (not applyBlockExitCpp)
	} else {
		// Mode 3 SHIL path with FULL binary comparison for first N blocks.
		// Compares ALL 512 bytes of Sh4Context (not just known registers).
		// This catches: old_sr, old_fpscr, sq_buffer, temp_reg, interrupt_pend.
		u32 shil_idx = g_wasm_block_count - SHIL_START_BLOCK;
		static u32 diag_diff_count = 0;
		bool diag = (shil_idx < 2000 && diag_diff_count < 50);

		if (diag) {
			// Save pre-block state
			alignas(16) static u8 diag_backup[sizeof(Sh4Context)];
			memcpy(diag_backup, &ctx, sizeof(Sh4Context));

			// Run ref
			int cc_pre = ctx.cycle_counter;
			ctx.cycle_counter -= block->guest_cycles;
			ref_execute_block(block);
			ctx.cycle_counter = cc_pre - (int)block->guest_cycles;

			// Save ref result
			alignas(16) static u8 diag_ref[sizeof(Sh4Context)];
			memcpy(diag_ref, &ctx, sizeof(Sh4Context));

			// Restore pre-block state for SHIL
			memcpy(&ctx, diag_backup, sizeof(Sh4Context));

			// Run SHIL
			cc_pre = ctx.cycle_counter;
			ctx.cycle_counter -= block->guest_cycles;
			g_ifb_exception_pending = false;
			for (u32 i = 0; i < block->oplist.size(); i++)
				wasm_exec_shil_fb(block->vaddr, i);
			ctx.cycle_counter = cc_pre - (int)block->guest_cycles;
			applyBlockExitCpp(block);
			if (g_ifb_exception_pending) {
				Do_Exception(g_ifb_exception_epc, g_ifb_exception_expEvn);
				g_ifb_exception_pending = false;
			}

			// Full binary comparison of ALL 512 bytes
			const u8* ref_bytes = (const u8*)diag_ref;
			const u8* shil_bytes = (const u8*)&ctx;
			// Skip offsets: cycle_counter (0x174, 4 bytes) and doSqWrite pointer (0x178, 4 bytes)
			bool any_diff = false;
			int first_diff_off = -1;
			u32 first_diff_ref = 0, first_diff_shil = 0;
			int diff_count = 0;
			for (int off = 0; off < (int)sizeof(Sh4Context); off += 4) {
				// Skip cycle_counter and doSqWrite
				if (off == 0x174 || off == 0x178) continue;
				u32 rv = *(u32*)(ref_bytes + off);
				u32 sv = *(u32*)(shil_bytes + off);
				if (rv != sv) {
					diff_count++;
					if (!any_diff) {
						any_diff = true;
						first_diff_off = off;
						first_diff_ref = rv;
						first_diff_shil = sv;
					}
				}
			}
#ifdef __EMSCRIPTEN__
			if (any_diff) {
				diag_diff_count++;
				// Map offset to field name
				const char* field_name = "unknown";
				if (first_diff_off < 0x20) field_name = "sq_buffer[0]";
				else if (first_diff_off < 0x40) field_name = "sq_buffer[1]";
				else if (first_diff_off < 0x80) field_name = "xf";
				else if (first_diff_off < 0xC0) field_name = "fr";
				else if (first_diff_off < 0x100) field_name = "r";
				else if (first_diff_off == 0x100) field_name = "mac.l";
				else if (first_diff_off == 0x104) field_name = "mac.h";
				else if (first_diff_off < 0x128) field_name = "r_bank";
				else if (first_diff_off == 0x128) field_name = "gbr";
				else if (first_diff_off == 0x12C) field_name = "ssr";
				else if (first_diff_off == 0x130) field_name = "spc";
				else if (first_diff_off == 0x134) field_name = "sgr";
				else if (first_diff_off == 0x138) field_name = "dbr";
				else if (first_diff_off == 0x13C) field_name = "vbr";
				else if (first_diff_off == 0x140) field_name = "pr";
				else if (first_diff_off == 0x144) field_name = "fpul";
				else if (first_diff_off == 0x148) field_name = "pc";
				else if (first_diff_off == 0x14C) field_name = "jdyn";
				else if (first_diff_off == 0x150) field_name = "sr.status";
				else if (first_diff_off == 0x154) field_name = "sr.T";
				else if (first_diff_off == 0x158) field_name = "fpscr";
				else if (first_diff_off == 0x15C) field_name = "old_sr";
				else if (first_diff_off == 0x160) field_name = "old_fpscr";
				else if (first_diff_off == 0x164) field_name = "CpuRunning";
				else if (first_diff_off == 0x168) field_name = "sh4_sched_next";
				else if (first_diff_off == 0x16C) field_name = "interrupt_pend";
				else if (first_diff_off == 0x170) field_name = "temp_reg";

				EM_ASM({ console.log('[DIAG-FULL] #' + $0 +
					' blk_pc=0x' + ($1>>>0).toString(16) +
					' bt=' + $2 +
					' diffs=' + $3 +
					' first_off=0x' + ($4>>>0).toString(16) +
					' field=' + UTF8ToString($5) +
					' ref=0x' + ($6>>>0).toString(16) +
					' shil=0x' + ($7>>>0).toString(16) +
					' nops=' + $8); },
					shil_idx, block->vaddr, (int)block->BlockType,
					diff_count, first_diff_off, field_name,
					first_diff_ref, first_diff_shil,
					(u32)block->oplist.size());

				// For first 10 diffs, also dump all differing offsets
				if (diag_diff_count <= 10) {
					for (int off = 0; off < (int)sizeof(Sh4Context); off += 4) {
						if (off == 0x174 || off == 0x178) continue;
						u32 rv = *(u32*)(ref_bytes + off);
						u32 sv = *(u32*)(shil_bytes + off);
						if (rv != sv) {
							EM_ASM({ console.log('[DIAG-DIFF] off=0x' + ($0>>>0).toString(16) +
								' ref=0x' + ($1>>>0).toString(16) +
								' shil=0x' + ($2>>>0).toString(16)); },
								off, rv, sv);
						}
					}
					// Dump oplist
					for (u32 i = 0; i < block->oplist.size() && i < 30; i++) {
						auto& sop = block->oplist[i];
						EM_ASM({ console.log('[DIAG-OP] [' + $0 + '] shop=' + $1 +
							' rd_type=' + $2 + ' rd_imm=0x' + ($3>>>0).toString(16) +
							' rs1_type=' + $4 + ' rs1_imm=0x' + ($5>>>0).toString(16) +
							' rs2_type=' + $6 + ' rs2_imm=0x' + ($7>>>0).toString(16) +
							' size=' + $8); },
							i, (int)sop.op,
							(int)sop.rd.type, sop.rd._imm,
							(int)sop.rs1.type, sop.rs1._imm,
							(int)sop.rs2.type, sop.rs2._imm,
							sop.size);
					}
				}
			}
#endif
			// Restore ref's result
			memcpy(&ctx, diag_ref, sizeof(Sh4Context));
		} else {
			// Pure SHIL
			int cc_pre = ctx.cycle_counter;
			ctx.cycle_counter -= block->guest_cycles;
			g_ifb_exception_pending = false;
			for (u32 i = 0; i < block->oplist.size(); i++)
				wasm_exec_shil_fb(block->vaddr, i);
			ctx.cycle_counter = cc_pre - (int)block->guest_cycles;
			applyBlockExitCpp(block);
			if (g_ifb_exception_pending) {
				Do_Exception(g_ifb_exception_epc, g_ifb_exception_expEvn);
				g_ifb_exception_pending = false;
			}
		}
	}
#elif EXECUTOR_MODE == 2
	// REF executor with guest_opcodes upfront charging (no per-instruction)
	{
		int cc_pre = ctx.cycle_counter;
		ctx.cycle_counter -= block->guest_opcodes;
		ref_execute_block(block);
		int cc_post = ctx.cycle_counter;
		int total_charge = cc_pre - cc_post;
		int leak = total_charge - block->guest_opcodes;
#ifdef __EMSCRIPTEN__
		if (g_wasm_block_count < 100) {
			EM_ASM({ console.log('[CHARGE] #' + $0 +
				' go=' + $1 + ' gc=' + $2 +
				' total=' + $3 + ' leak=' + $4 +
				' bt=' + $5); },
				g_wasm_block_count, block->guest_opcodes, block->guest_cycles,
				total_charge, leak, (int)block->BlockType);
		}
#endif
	}
#elif EXECUTOR_MODE == 4
	// REF execution with SHIL-style cycle charging + block exit comparison.
	// Compares ref's natural PC (from OpPtr) with applyBlockExitCpp's PC
	// to verify block exit logic is correct.
	{
		int cc_pre = ctx.cycle_counter;
		ctx.cycle_counter -= block->guest_cycles;
		ref_execute_block(block);
		ctx.cycle_counter = cc_pre - (int)block->guest_cycles;

		// Compare ref's PC with applyBlockExitCpp's result
		u32 ref_pc = ctx.pc;
		applyBlockExitCpp(block);
		u32 exit_pc = ctx.pc;
		static u32 exit_mismatch_count = 0;
		if (ref_pc != exit_pc && exit_mismatch_count < 200) {
			exit_mismatch_count++;
#ifdef __EMSCRIPTEN__
			EM_ASM({ console.log('[EXIT-DIFF] #' + $0 +
				' blk=' + $1 +
				' block_pc=0x' + ($2>>>0).toString(16) +
				' ref_pc=0x' + ($3>>>0).toString(16) +
				' exit_pc=0x' + ($4>>>0).toString(16) +
				' bt=' + $5 +
				' T=' + $6 +
				' jdyn=0x' + ($7>>>0).toString(16)); },
				exit_mismatch_count, g_wasm_block_count, block->vaddr,
				ref_pc, exit_pc, (int)block->BlockType,
				ctx.sr.T, ctx.jdyn);
#endif
		}
		// Restore ref's PC (the correct one) for continued execution
		ctx.pc = ref_pc;
	}
#elif EXECUTOR_MODE == 5
	// SHADOW COMPARISON: run ref first (correct), then SHIL on same input.
	// Ref writes to memory (correct). SHIL reads the same correct memory.
	// Compare register output to find the first diverging block/op.
	// Execution continues with ref's result (correct) so all subsequent
	// blocks start from a known-good state.
	{
		// Save pre-block register state
		alignas(16) static u8 ctx_backup_buf[sizeof(Sh4Context)];
		Sh4Context& ctx_backup = *(Sh4Context*)ctx_backup_buf;
		memcpy(&ctx_backup, &ctx, sizeof(Sh4Context));

		// --- Run ref (known correct) ---
		{
			int cc_pre = ctx.cycle_counter;
			ctx.cycle_counter -= block->guest_cycles;
			ref_execute_block(block);
			ctx.cycle_counter = cc_pre - (int)block->guest_cycles;
		}

		// Save ref's result
		alignas(16) static u8 ref_result_buf[sizeof(Sh4Context)];
		Sh4Context& ref_result = *(Sh4Context*)ref_result_buf;
		memcpy(&ref_result, &ctx, sizeof(Sh4Context));

		// Restore pre-block registers for SHIL (memory keeps ref's writes)
		memcpy(&ctx, &ctx_backup, sizeof(Sh4Context));

		// --- Run SHIL (no dry-run: real writes, which are same values ref wrote) ---
		// Memory already has ref's correct writes. SHIL reads correct values.
		// SHIL writes same values again (redundant, but harmless for RAM/MMIO).
		// No write comparison (previous attempts had MMIO read side effects).
		ctx.cycle_counter -= block->guest_cycles;
		g_ifb_exception_pending = false;
		for (u32 i = 0; i < block->oplist.size(); i++)
			wasm_exec_shil_fb(block->vaddr, i);
		applyBlockExitCpp(block);
		if (g_ifb_exception_pending) {
			Do_Exception(g_ifb_exception_epc, g_ifb_exception_expEvn);
			g_ifb_exception_pending = false;
		}

		// Compare SHIL vs ref registers (skip cycle_counter, jdyn)
		if (shadow_mismatch_count < 500) {
			bool match = true;
			int diff_reg = -1;
			const char* diff_name = "";
			u32 shil_v = 0, ref_v = 0;

#define CMP_REG(field, name) \
	if (match && ctx.field != ref_result.field) { \
		match = false; diff_name = name; \
		shil_v = (u32)ctx.field; ref_v = (u32)ref_result.field; \
	}
#define CMP_REG_ARR(arr, count, name) \
	if (match) for (int _i = 0; _i < (count); _i++) { \
		if (*(u32*)&ctx.arr[_i] != *(u32*)&ref_result.arr[_i]) { \
			match = false; diff_name = name; diff_reg = _i; \
			shil_v = *(u32*)&ctx.arr[_i]; ref_v = *(u32*)&ref_result.arr[_i]; \
			break; \
		} \
	}

			CMP_REG(pc, "pc")
			CMP_REG_ARR(r, 16, "r")
			CMP_REG(sr.T, "sr.T")
			CMP_REG(sr.status, "sr.status")
			CMP_REG_ARR(fr, 16, "fr")
			CMP_REG_ARR(xf, 16, "xf")
			CMP_REG(mac.l, "mac.l")
			CMP_REG(mac.h, "mac.h")
			CMP_REG(pr, "pr")
			CMP_REG(fpscr.full, "fpscr")
			CMP_REG(gbr, "gbr")
			CMP_REG(fpul, "fpul")
			// jdyn excluded: ref doesn't set it (uses OpPtr dispatch), SHIL does (shop_jdyn).
			// Both produce correct PC, so jdyn mismatch is a false positive.
			// CMP_REG(jdyn, "jdyn")
			CMP_REG_ARR(r_bank, 8, "r_bank")
			CMP_REG(vbr, "vbr")
			CMP_REG(ssr, "ssr")
			CMP_REG(spc, "spc")
			CMP_REG(sgr, "sgr")
			CMP_REG(dbr, "dbr")
			CMP_REG(sr.S, "sr.S")
			CMP_REG(sr.IMASK, "sr.IMASK")
			CMP_REG(sr.Q, "sr.Q")
			CMP_REG(sr.M, "sr.M")
			CMP_REG(sr.FD, "sr.FD")
			CMP_REG(sr.BL, "sr.BL")
			CMP_REG(sr.RB, "sr.RB")
			CMP_REG(sr.MD, "sr.MD")

#undef CMP_REG
#undef CMP_REG_ARR

			if (!match) {
				shadow_mismatch_count++;
#ifdef __EMSCRIPTEN__
				EM_ASM({ console.log('[SHADOW] MISMATCH #' + $0 +
					' blk=' + $1 +
					' pc=0x' + ($2>>>0).toString(16) +
					' diff=' + UTF8ToString($3) +
					(($4 >= 0) ? ('[' + $4 + ']') : '') +
					' shil=0x' + ($5>>>0).toString(16) +
					' ref=0x' + ($6>>>0).toString(16) +
					' nops=' + $7 +
					' go=' + $8); },
					shadow_mismatch_count, g_wasm_block_count, block->vaddr,
					diff_name, diff_reg, shil_v, ref_v,
					(u32)block->oplist.size(), block->guest_opcodes);

				// For the first few mismatches, dump full register state
				if (shadow_mismatch_count <= 5) {
					EM_ASM({ console.log('[SHADOW-SHIL] r0=0x' + ($0>>>0).toString(16) +
						' r1=0x' + ($1>>>0).toString(16) +
						' r2=0x' + ($2>>>0).toString(16) +
						' r3=0x' + ($3>>>0).toString(16) +
						' r4=0x' + ($4>>>0).toString(16) +
						' r5=0x' + ($5>>>0).toString(16) +
						' r15=0x' + ($6>>>0).toString(16) +
						' pc=0x' + ($7>>>0).toString(16) +
						' T=' + $8 +
						' pr=0x' + ($9>>>0).toString(16)); },
						ctx.r[0], ctx.r[1], ctx.r[2],
						ctx.r[3], ctx.r[4], ctx.r[5],
						ctx.r[15], ctx.pc, ctx.sr.T, ctx.pr);
					EM_ASM({ console.log('[SHADOW-REF]  r0=0x' + ($0>>>0).toString(16) +
						' r1=0x' + ($1>>>0).toString(16) +
						' r2=0x' + ($2>>>0).toString(16) +
						' r3=0x' + ($3>>>0).toString(16) +
						' r4=0x' + ($4>>>0).toString(16) +
						' r5=0x' + ($5>>>0).toString(16) +
						' r15=0x' + ($6>>>0).toString(16) +
						' pc=0x' + ($7>>>0).toString(16) +
						' T=' + $8 +
						' pr=0x' + ($9>>>0).toString(16)); },
						ref_result.r[0], ref_result.r[1], ref_result.r[2],
						ref_result.r[3], ref_result.r[4], ref_result.r[5],
						ref_result.r[15], ref_result.pc, ref_result.sr.T, ref_result.pr);

					// Dump the SHIL ops for this block
					// NOTE: reg_offset() calls verify(is_reg()) which aborts on null/imm operands.
					// Use _imm (union member) for raw value regardless of type.
					for (u32 i = 0; i < block->oplist.size() && i < 30; i++) {
						auto& sop = block->oplist[i];
						EM_ASM({ console.log('[SHADOW-OP] [' + $0 + '] shop=' + $1 +
							' rd=' + $2 + ':0x' + ($3>>>0).toString(16) +
							' rs1=' + $4 + ':0x' + ($5>>>0).toString(16) +
							' rs2=' + $6 + ':0x' + ($7>>>0).toString(16) +
							' rs3=' + $8 + ':0x' + ($9>>>0).toString(16) +
							' size=' + $10); },
							i, (int)sop.op,
							(int)sop.rd.type, sop.rd._imm,
							(int)sop.rs1.type, sop.rs1._imm,
							(int)sop.rs2.type, sop.rs2._imm,
							(int)sop.rs3.type, sop.rs3._imm,
							sop.size);
					}
				}
#endif
			} else {
				shadow_match_count++;
			}
		}

		// Always restore ref's result so execution continues correctly
		memcpy(&ctx, &ref_result, sizeof(Sh4Context));
	}
#elif EXECUTOR_MODE == 6
	// Phase 2: PURE WASM execution.
	// Shadow comparison confirmed 2.36M blocks match (mismatch was methodology artifact).
	// Now run WASM directly without shadow overhead.
	{
		g_ifb_exception_pending = false;
		u32 ctx_ptr = (u32)(uintptr_t)&ctx;
		int trap = wasm_execute_block(block->vaddr, ctx_ptr);
		if (trap) {
			// WASM trapped — fallback to C++ for this block
			ctx.cycle_counter -= block->guest_cycles;
			for (u32 i = 0; i < block->oplist.size(); i++)
				wasm_exec_shil_fb(block->vaddr, i);
			applyBlockExitCpp(block);
		}
		if (g_ifb_exception_pending) {
			Do_Exception(g_ifb_exception_epc, g_ifb_exception_expEvn);
			g_ifb_exception_pending = false;
		}
	}
#else
	// SHIL executor — charge guest_cycles upfront, forced reset after.
	// See FINDINGS comment in compile() for timing investigation results.
	{
		int cc_pre = ctx.cycle_counter;
		ctx.cycle_counter -= block->guest_cycles;
		g_ifb_exception_pending = false;
		for (u32 i = 0; i < block->oplist.size(); i++)
			wasm_exec_shil_fb(block->vaddr, i);
		ctx.cycle_counter = cc_pre - (int)block->guest_cycles;
#ifdef __EMSCRIPTEN__
		// Log r0 right after SHIL ops, before applyBlockExitCpp
		if (g_wasm_block_count == 2360475) {
			EM_ASM({ console.log('[SHIL-EXIT] blk=2360475' +
				' vaddr=0x' + ($0>>>0).toString(16) +
				' r0=0x' + ($1>>>0).toString(16) +
				' pc=0x' + ($2>>>0).toString(16) +
				' nops=' + $3); },
				block->vaddr, ctx.r[0], ctx.pc, (u32)block->oplist.size());
		}
#endif
		applyBlockExitCpp(block);
		if (g_ifb_exception_pending) {
			Do_Exception(g_ifb_exception_epc, g_ifb_exception_expEvn);
			g_ifb_exception_pending = false;
		}
	}
#endif

#ifdef __EMSCRIPTEN__
	// Track visits to the critical PCs where FB_R_CTRL writes happen
	{
		static u32 visits_da5a8 = 0;
		static u32 visits_db58a = 0;
		static u32 visits_da9e4 = 0;  // STARTRENDER PC
		if (block->vaddr == 0x8c0da5a8) {
			visits_da5a8++;
			if (visits_da5a8 <= 10)
				EM_ASM({ console.log('[KEY-PC] 0x8c0da5a8 visit #' + $0 +
					' blk=' + $1 + ' r0=0x' + ($2>>>0).toString(16) +
					' cc=' + ($3|0)); },
					visits_da5a8, g_wasm_block_count, ctx.r[0], ctx.cycle_counter);
		}
		if (block->vaddr == 0x8c0db58a) {
			visits_db58a++;
			if (visits_db58a <= 20)
				EM_ASM({ console.log('[KEY-PC] 0x8c0db58a visit #' + $0 +
					' blk=' + $1 + ' r0=0x' + ($2>>>0).toString(16)); },
					visits_db58a, g_wasm_block_count, ctx.r[0]);
		}
		if (block->vaddr == 0x8c0da9e4) {
			visits_da9e4++;
			if (visits_da9e4 <= 10)
				EM_ASM({ console.log('[KEY-PC] 0x8c0da9e4 (STARTRENDER) visit #' + $0 +
					' blk=' + $1); },
					visits_da9e4, g_wasm_block_count);
		}
		// Summary at checkpoints
		if (g_wasm_block_count % 5000000 == 0 && g_wasm_block_count > 0) {
			EM_ASM({ console.log('[KEY-PC-SUM] blk=' + $0 +
				' da5a8=' + $1 + ' db58a=' + $2 + ' da9e4=' + $3); },
				g_wasm_block_count, visits_da5a8, visits_db58a, visits_da9e4);
		}
	}

	// CC-SUMMARY for ALL modes
	if (g_wasm_block_count == 100000 || g_wasm_block_count == 500000 || g_wasm_block_count == 1000000 ||
	    g_wasm_block_count == 2000000 || g_wasm_block_count == 2360000 || g_wasm_block_count == 2360114) {
		EM_ASM({ console.log('[CC-SUMMARY] blk=' + $0 +
			' cc=' + ($1|0) +
			' mode=' + $2); },
			g_wasm_block_count, ctx.cycle_counter, EXECUTOR_MODE);
	}
#if EXECUTOR_MODE == 5
	if (g_wasm_block_count == 1000 || g_wasm_block_count == 10000 || g_wasm_block_count == 100000 ||
	    g_wasm_block_count == 500000 || g_wasm_block_count == 1000000 || g_wasm_block_count == 2000000) {
		EM_ASM({ console.log('[SHADOW-SUM] at blk=' + $0 +
			' match=' + $1 + ' mismatch=' + $2 +
			' rate=' + (($1 > 0) ? (100.0 * $1 / ($1 + $2)).toFixed(2) : '0') + '%'); },
			g_wasm_block_count, shadow_match_count, shadow_mismatch_count);
	}
#endif
#if EXECUTOR_MODE == 6 || EXECUTOR_MODE == 4 || EXECUTOR_MODE == 5 || EXECUTOR_MODE == 1 || EXECUTOR_MODE == 3
	if (g_wasm_block_count % 500000 == 0 && g_wasm_block_count > 0 && g_wasm_block_count <= 50000000) {
		EM_ASM({ console.log('[SHIL-IO] blk=' + $0 +
			' reads=' + $1 + ' writes=' + $2 +
			' pvr_wr=' + $3 + ' sq_wr=' + $4 +
			' fb_calls=' + $5 + ' fb_miss=' + $6); },
			g_wasm_block_count, g_shil_read_count, g_shil_write_count,
			g_shil_pvr_write_count, g_shil_sq_write_count,
			g_shil_fb_call_count, g_shil_fb_miss_count);

		// Read PVR registers via BOTH paths: ReadMem and direct union
		u32 fb_r_ctrl_mem = ReadMem32(0xA05F8044);
		u32 fb_r_ctrl_union = FB_R_CTRL.full;
		u32 fb_w_ctrl = FB_W_CTRL.full;
		u32 spg_ctrl = SPG_CONTROL.full;
		u32 fb_r_sof1 = FB_R_SOF1;
		u32 fb_r_sof2 = FB_R_SOF2;
		EM_ASM({ console.log('[PVR-REG] blk=' + $0 +
			' FB_R_CTRL_mem=0x' + ($1>>>0).toString(16) +
			' FB_R_CTRL_union=0x' + ($2>>>0).toString(16) +
			' fb_enable=' + ($2 & 1) +
			' FB_W_CTRL=0x' + ($3>>>0).toString(16) +
			' SPG=0x' + ($4>>>0).toString(16) +
			' SOF1=0x' + ($5>>>0).toString(16) +
			' SOF2=0x' + ($6>>>0).toString(16)); },
			g_wasm_block_count, fb_r_ctrl_mem, fb_r_ctrl_union,
			fb_w_ctrl, spg_ctrl, fb_r_sof1, fb_r_sof2);
		// Naomi serial EEPROM access counters
		EM_ASM({ console.log('[NAOMI-ID] blk=' + $0 +
			' boardWrites=' + $1 + ' boardReads=' + $2); },
			g_wasm_block_count, g_naomi_board_write_count, g_naomi_board_read_count);
	}
	// Extra Naomi counter checkpoints around the switchover
	if (g_wasm_block_count == 2360000 || g_wasm_block_count == 2361000 ||
	    g_wasm_block_count == 2362000 || g_wasm_block_count == 2365000 ||
	    g_wasm_block_count == 2370000 || g_wasm_block_count == 2380000 ||
	    g_wasm_block_count == 2400000 || g_wasm_block_count == 3000000) {
		EM_ASM({ console.log('[NAOMI-ID] blk=' + $0 +
			' boardWrites=' + $1 + ' boardReads=' + $2); },
			g_wasm_block_count, g_naomi_board_write_count, g_naomi_board_read_count);
	}
#endif
#endif

	g_wasm_block_count++;

	// (post-execution diagnostic removed — no longer needed)

	pc_hash = pc_hash * 1000003u + block->vaddr;

	// State hash A: r[0..15] + pc + sr.T — MULTIPLICATIVE accumulation
	u32 sh = 0;
	for (int i = 0; i < 16; i++) sh = sh * 31u + ctx.r[i];
	sh = sh * 31u + ctx.pc;
	sh = sh * 31u + ctx.sr.T;
	state_hash = state_hash * 1000003u + sh;

	// State hash B: fr[0..15] + mac + pr + sr.status + fpscr + gbr
	u32 sh2 = 0;
	for (int i = 0; i < 16; i++) sh2 = sh2 * 31u + *(u32*)&ctx.fr[i];
	sh2 = sh2 * 31u + ctx.mac.l;
	sh2 = sh2 * 31u + ctx.mac.h;
	sh2 = sh2 * 31u + ctx.pr;
	sh2 = sh2 * 31u + ctx.sr.status;
	sh2 = sh2 * 31u + ctx.fpscr.full;
	sh2 = sh2 * 31u + ctx.gbr;
	state_hash2 = state_hash2 * 1000003u + sh2;

	// State hash C: xf[0..15] + r_bank[0..7] + fpul + vbr + ssr + spc + sgr + dbr
	u32 sh3 = 0;
	for (int i = 0; i < 16; i++) sh3 = sh3 * 31u + *(u32*)&ctx.xf[i];
	for (int i = 0; i < 8; i++) sh3 = sh3 * 31u + ctx.r_bank[i];
	sh3 = sh3 * 31u + ctx.fpul;
	sh3 = sh3 * 31u + ctx.vbr;
	sh3 = sh3 * 31u + ctx.ssr;
	sh3 = sh3 * 31u + ctx.spc;
	sh3 = sh3 * 31u + ctx.sgr;
	sh3 = sh3 * 31u + ctx.dbr;
	state_hash3 = state_hash3 * 1000003u + sh3;

#ifdef __EMSCRIPTEN__
	if (g_wasm_block_count % 100000 == 0 ||
	    (g_wasm_block_count >= 2300000 && g_wasm_block_count <= 2400000 && g_wasm_block_count % 1000 == 0)) {
		EM_ASM({ console.log('[TRACE] blk=' + $0 +
			' pc=0x' + ($1>>>0).toString(16) +
			' hPC=0x' + ($2>>>0).toString(16) +
			' hA=0x' + ($3>>>0).toString(16) +
			' hB=0x' + ($4>>>0).toString(16) +
			' hC=0x' + ($5>>>0).toString(16) +
			' r0=0x' + ($6>>>0).toString(16) +
			' T=' + $7); },
			g_wasm_block_count, ctx.pc, pc_hash,
			state_hash, state_hash2, state_hash3,
			ctx.r[0], ctx.sr.T);
		pc_hash = 0;
		state_hash = 0;
		state_hash2 = 0;
		state_hash3 = 0;
	}
	// Per-block trace in the divergence range to find exact diverging block
	if (g_wasm_block_count >= 2360000 && g_wasm_block_count < 2361000) {
		// Hash ALL r[0..15] to find ANY register divergence
		u32 rh = 0;
		for (int i = 0; i < 16; i++) rh = rh * 31u + ctx.r[i];
		// Read the value at the ACTUAL r0 pop address for the diverging block
		// BLK-DETAIL #2360476: r15_end=0x8cffffc0, r0 popped from r15_end - 8
		u32 stack_val = ReadMem32(0x8cffffb8);
		EM_ASM({ console.log('[BLK-DETAIL] #' + $0 +
			' pc=0x' + ($1>>>0).toString(16) +
			' cc=' + ($2|0) +
			' rH=0x' + ($3>>>0).toString(16) +
			' stk=0x' + ($4>>>0).toString(16) +
			' r0=0x' + ($5>>>0).toString(16) +
			' r4=0x' + ($6>>>0).toString(16) +
			' r15=0x' + ($7>>>0).toString(16) +
			' T=' + $8 +
			' pr=0x' + ($9>>>0).toString(16)); },
			g_wasm_block_count, ctx.pc, ctx.cycle_counter,
			rh, stack_val,
			ctx.r[0], ctx.r[4], ctx.r[15],
			ctx.sr.T, ctx.pr);
	}
#endif
}

// ============================================================
// EM_JS bridge: compile + execute WASM blocks from JavaScript
// ============================================================

#ifdef __EMSCRIPTEN__

EM_JS(int, wasm_compile_block, (const u8* bytesPtr, u32 len, u32 block_pc), {
	try {
		var wasmBytes = Module.HEAPU8.slice(bytesPtr, bytesPtr + len);
		var mod = new WebAssembly.Module(wasmBytes);
		var instance = new WebAssembly.Instance(mod, {
			env: {
				memory: wasmExports.memory,
				read8:   function(addr) { return Module._wasm_mem_read8(addr); },
				read16:  function(addr) { return Module._wasm_mem_read16(addr); },
				read32:  function(addr) { return Module._wasm_mem_read32(addr); },
				write8:  function(addr, val) { Module._wasm_mem_write8(addr, val); },
				write16: function(addr, val) { Module._wasm_mem_write16(addr, val); },
				write32: function(addr, val) { Module._wasm_mem_write32(addr, val); },
				ifb:     function(opcode, pc) { Module._wasm_exec_ifb(opcode, pc); },
				shil_fb: function(block_vaddr, op_idx) { Module._wasm_exec_shil_fb(block_vaddr, op_idx); }
			}
		});
		if (!Module._wasmBlockCache) Module._wasmBlockCache = {};
		Module._wasmBlockCache[block_pc] = instance.exports.b;
		return 1;
	} catch (e) {
		console.error('[rec_wasm] compile fail PC=0x' + (block_pc >>> 0).toString(16) + ': ' + e.message);
		return 0;
	}
});

EM_JS(int, wasm_execute_block, (u32 block_pc, u32 ctx_ptr), {
	try {
		Module._wasmBlockCache[block_pc](ctx_ptr);
		return 0;
	} catch (e) {
		if (!Module._wasmTrapCount) Module._wasmTrapCount = 0;
		Module._wasmTrapCount++;
		if (Module._wasmTrapCount <= 50) {
			console.error('[wasm-trap] PC=0x' + (block_pc >>> 0).toString(16) + ': ' + e.message);
		}
		return 1;
	}
});

EM_JS(int, wasm_has_block, (u32 block_pc), {
	return (Module._wasmBlockCache && Module._wasmBlockCache[block_pc]) ? 1 : 0;
});

EM_JS(void, wasm_clear_cache, (), {
	Module._wasmBlockCache = {};
});

EM_JS(void, wasm_remove_block, (u32 block_pc), {
	if (Module._wasmBlockCache) delete Module._wasmBlockCache[block_pc];
});

EM_JS(int, wasm_cache_size, (), {
	return Module._wasmBlockCache ? Object.keys(Module._wasmBlockCache).length : 0;
});

#else
static int wasm_compile_block(const u8*, u32, u32) { return 0; }
static void wasm_execute_block(u32, u32) {}
static int wasm_has_block(u32) { return 0; }
static void wasm_remove_block(u32) {}
static void wasm_clear_cache() {}
static int wasm_cache_size() { return 0; }
#endif

// ============================================================
// Build a complete WASM module for one compiled block
// ============================================================

static bool buildBlockModule(WasmModuleBuilder& b, RuntimeBlockInfo* block) {
	b.emitHeader();

	// Type section: 3 function signatures
	// Type 0: (i32) -> void      — block function
	// Type 1: (i32) -> i32       — read8/16/32
	// Type 2: (i32, i32) -> void — write8/16/32, ifb, shil_fb
	b.emitTypeSection(3);
	{
		u8 p0[] = { WASM_TYPE_I32 };
		b.emitFuncType(p0, 1, nullptr, 0);

		u8 r1[] = { WASM_TYPE_I32 };
		b.emitFuncType(p0, 1, r1, 1);

		u8 p2[] = { WASM_TYPE_I32, WASM_TYPE_I32 };
		b.emitFuncType(p2, 2, nullptr, 0);
	}
	b.endSection();

	// Import section: 1 memory + 8 functions
	b.emitImportSection(9);
	b.emitImportMemory("env", "memory", 0);
	b.emitImportFunc("env", "read8",   1);
	b.emitImportFunc("env", "read16",  1);
	b.emitImportFunc("env", "read32",  1);
	b.emitImportFunc("env", "write8",  2);
	b.emitImportFunc("env", "write16", 2);
	b.emitImportFunc("env", "write32", 2);
	b.emitImportFunc("env", "ifb",     2);
	b.emitImportFunc("env", "shil_fb", 2);
	b.endSection();

	// Function section: 1 defined function (type 0)
	u32 typeIdx = 0;
	b.emitFunctionSection(1, &typeIdx);

	// Export section: export block function as "b" (func idx 8)
	b.emitExportSection("b", WIMPORT_COUNT);

	// Code section
	b.beginCodeSection(1);
	b.beginFuncBody();

	// Locals: 1 temp i32
	u32 lc = 1;
	u8 lt = WASM_TYPE_I32;
	b.emitLocals(1, &lc, &lt);

	// Prologue: cycle_counter -= guest_cycles (matches C++ SHIL mode 6)
	b.op_local_get(LOCAL_CTX);
	b.op_local_get(LOCAL_CTX);
	b.op_i32_load(ctx_off::CYCLE_COUNTER);
	b.op_i32_const((s32)block->guest_cycles);
	b.op_i32_sub();
	b.op_i32_store(ctx_off::CYCLE_COUNTER);

	// Emit each SHIL op
	for (u32 i = 0; i < block->oplist.size(); i++) {
		shil_opcode& op = block->oplist[i];
		if (!emitShilOp(b, op, block, i)) {
			// Unhandled op — call SHIL canonical fallback
			b.op_i32_const((s32)block->vaddr);
			b.op_i32_const((s32)i);
			b.op_call(WIMPORT_SHIL_FB);
		}
	}

	// Epilogue: set next PC
	emitBlockExit(b, block);

	b.endFuncBody();
	b.endSection();

	return true;
}

// ============================================================
// WasmDynarec class
// ============================================================

class WasmDynarec : public Sh4Dynarec
{
public:
	WasmDynarec()
	{
		sh4Dynarec = this;
	}

	void init(Sh4Context& ctx, Sh4CodeBuffer& buf) override
	{
#ifdef __EMSCRIPTEN__
		EM_ASM({ console.log('[rec_wasm] WasmDynarec::init() — Phase 2 WASM JIT'); });
		EM_ASM({ console.log('[FINDINGS] SHIL executor timing investigation results:'); });
		EM_ASM({ console.log('[FINDINGS] - Mode 0 (ref_execute_block + flat -1/instr) = PASS'); });
		EM_ASM({ console.log('[FINDINGS] - Mode 1 (SHIL + guest_cycles) = FAIL_BLACK, diverges from mode 0 at ~3.2M blocks'); });
		EM_ASM({ console.log('[FINDINGS] - Root cause: accumulated cycle_counter drift from different charging models'); });
		EM_ASM({ console.log('[FINDINGS] - Mode 0 charges: flat -1/iteration + IReadMem16 cache penalties (sh4_cache.h)'); });
		EM_ASM({ console.log('[FINDINGS] - Mode 1 charges: guest_cycles upfront (SH4 timing tables via countCycles)'); });
		EM_ASM({ console.log('[FINDINGS] - guest_cycles != sum(flat -1) because different timing models'); });
		EM_ASM({ console.log('[FINDINGS] - Upstream interpreter uses executeCycles(op) (per-instruction, varies by type)'); });
		EM_ASM({ console.log('[FINDINGS] - Sh4Cycles class (sh4_cycles.h): executeCycles, addReadAccessCycles, addWriteAccessCycles'); });
		EM_ASM({ console.log('[FINDINGS] - Timer reads (TMU TCNT0 at 0xFFD8000C) depend on cycle_counter via now()'); });
		EM_ASM({ console.log('[FINDINGS] - Drift causes timer read divergence -> branch divergence -> fb_enable never set'); });
		EM_ASM({ console.log('[FINDINGS] - ATTEMPTED FIXES (all worse than baseline):'); });
		EM_ASM({ console.log('[FINDINGS]   1. guest_offs per-instruction charging: diverges at 2.5M (earlier!)'); });
		EM_ASM({ console.log('[FINDINGS]   2. delay_slot-aware charging: same 2.5M divergence'); });
		EM_ASM({ console.log('[FINDINGS]   3. guest_opcodes instead of guest_cycles: 2.5M divergence'); });
		EM_ASM({ console.log('[FINDINGS]   4. No forced reset (natural penalties): 2.5M divergence'); });
		EM_ASM({ console.log('[FINDINGS] - CONCLUSION: per-instruction charging adds error faster than it corrects.'); });
		EM_ASM({ console.log('[FINDINGS]   The baseline guest_cycles model (3.2M divergence) is closest to working.'); });
		EM_ASM({ console.log('[FINDINGS] - NEXT STEPS: need to either match ref_execute_block exactly (call'); });
		EM_ASM({ console.log('[FINDINGS]   executeCycles per SH4 instruction) or find a way to make the BIOS'); });
		EM_ASM({ console.log('[FINDINGS]   tolerate the timing drift (e.g., longer test, different BIOS version).'); });
#endif
		sh4ctx = &ctx;
		codeBuffer = &buf;
		compiledCount = 0;
		failCount = 0;
	}

	void compile(RuntimeBlockInfo* block, bool smc_checks, bool optimise) override
	{
		blockByVaddr[block->vaddr] = block;

		// Store hash for SMC detection (first 2 bytes of block code)
		blockCodeHash[block->vaddr] = (u32)IReadMem16(block->vaddr);

#if EXECUTOR_MODE == 6
		// Only build WASM modules when using WASM execution
		WasmModuleBuilder builder;
		buildBlockModule(builder, block);

		const auto& bytes = builder.getBytes();
		int result = wasm_compile_block(bytes.data(), (u32)bytes.size(), block->vaddr);

		if (result)
			compiledCount++;
		else
			failCount++;
#else
		compiledCount++;
#endif

		// Dummy code pointer for block manager
		block->code = (DynarecCodeEntryPtr)codeBuffer->get();
		if (codeBuffer->getFreeSpace() >= 4)
			codeBuffer->advance(4);

#ifdef __EMSCRIPTEN__
		if (compiledCount <= 10 || (compiledCount % 200 == 0)) {
			EM_ASM({ console.log('[rec_wasm] compiled=' + $0 + ' fail=' + $1 +
				' pc=0x' + ($2>>>0).toString(16) + ' ops=' + $3); },
				compiledCount, failCount, block->vaddr,
				(int)block->oplist.size());
		}
		// Dump full oplist for the ACTUAL diverging block at PC 0x8c00b996
		// (and 0x8c00b8e4 which is the epilogue that reads the different value)
		if (block->vaddr == 0x8c00b996 || block->vaddr == 0x8c00b8e4) {
			EM_ASM({ console.log('[OPLIST-DUMP] pc=0x8c00b8e4 nops=' + $0 +
				' guest_cycles=' + $1 + ' guest_opcodes=' + $2 +
				' bt=' + $3 + ' sh4_size=' + $4); },
				(u32)block->oplist.size(), block->guest_cycles,
				block->guest_opcodes, (int)block->BlockType,
				block->sh4_code_size);
			for (u32 i = 0; i < block->oplist.size(); i++) {
				shil_opcode& sop = block->oplist[i];
				u32 rd_off = sop.rd.is_reg() ? sop.rd.reg_offset() : 0xFFFF;
				u32 rs1_info = sop.rs1.is_reg() ? sop.rs1.reg_offset() :
				               (sop.rs1.is_imm() ? sop.rs1._imm : 0xDEAD);
				u32 rs2_info = sop.rs2.is_reg() ? sop.rs2.reg_offset() :
				               (sop.rs2.is_imm() ? sop.rs2._imm : 0xDEAD);
				u32 rs3_info = sop.rs3.is_reg() ? sop.rs3.reg_offset() :
				               (sop.rs3.is_imm() ? sop.rs3._imm : 0xDEAD);
				EM_ASM({ console.log('[OPLIST] i=' + $0 +
					' op=' + $1 + ' sz=' + $2 +
					' rd=0x' + ($3>>>0).toString(16) +
					' rs1=0x' + ($4>>>0).toString(16) +
					' rs2=0x' + ($5>>>0).toString(16) +
					' rs3=0x' + ($6>>>0).toString(16) +
					' rs1_reg=' + $7 + ' rs1_imm=' + $8); },
					i, (int)sop.op, (int)sop.size,
					rd_off, rs1_info, rs2_info, rs3_info,
					sop.rs1.is_reg() ? 1 : 0, sop.rs1.is_imm() ? 1 : 0);
			}
		}
#endif
	}

	void mainloop(void* cntx) override
	{
		// CRITICAL: Branch instructions with delay slots call executeDelaySlot()
		// which dereferences Sh4Interpreter::Instance.
		Sh4Interpreter::Instance = Sh4Recompiler::Instance;

#ifdef __EMSCRIPTEN__
		static int mainloop_count = 0;
		mainloop_count++;
		if (mainloop_count <= 5) {
			EM_ASM({ console.log('[rec_wasm] mainloop #' + $0 + ' cache=' + $1); },
				mainloop_count, wasm_cache_size());
		}
#endif
		u32 blockExecs = 0;
		u32 interpExecs = 0;
		u32 timeslices = 0;

		try {
			do {
				try {
					do {
						u32 pc = sh4ctx->pc;
						auto it = blockByVaddr.find(pc);

						if (it == blockByVaddr.end()) {
							rdv_FailedToFindBlock(pc);
							it = blockByVaddr.find(pc);
						}

						if (it != blockByVaddr.end()) {
							// SMC check: verify first opcode hasn't changed
							auto hit = blockCodeHash.find(pc);
							if (hit != blockCodeHash.end()) {
								u32 curOp = (u32)IReadMem16(pc);
								if (curOp != hit->second) {
									// Invalidate stale block and fall through to interpreter
									wasm_remove_block(pc);
									blockCodeHash.erase(pc);
									blockByVaddr.erase(pc);
									it = blockByVaddr.end();
								}
							}
						}

						if (it != blockByVaddr.end()) {
							cpp_execute_block(it->second);
							blockExecs++;

							if (sh4ctx->interrupt_pend)
								UpdateINTC();
						} else {
							// Interpreter fallback — one instruction
							sh4ctx->pc = pc + 2;
							u16 op = IReadMem16(pc);
							if (sh4ctx->sr.FD == 1 && OpDesc[op]->IsFloatingPoint())
								throw SH4ThrownException(pc, Sh4Ex_FpuDisabled);
							OpPtr[op](sh4ctx, op);
							sh4ctx->cycle_counter -= 1;
							interpExecs++;
						}

					} while (sh4ctx->cycle_counter > 0);

#ifdef __EMSCRIPTEN__
					if (timeslices < 10) {
						EM_ASM({ console.log('[TS] #' + $0 +
							' cc_end=' + ($1|0) +
							' blks=' + $2 +
							' pc=0x' + ($3>>>0).toString(16)); },
							timeslices, sh4ctx->cycle_counter,
							blockExecs, sh4ctx->pc);
					}
#endif
					sh4ctx->cycle_counter += SH4_TIMESLICE;
					timeslices++;
					UpdateSystem_INTC();

				} catch (const SH4ThrownException& ex) {
					Do_Exception(ex.epc, ex.expEvn);
					sh4ctx->cycle_counter += 5;
				}
			} while (sh4ctx->CpuRunning);

		} catch (...) {
#ifdef __EMSCRIPTEN__
			EM_ASM({ console.log('[rec_wasm] WARNING: mainloop exited via catch(...)'); });
#endif
		}

		sh4ctx->CpuRunning = false;

#ifdef __EMSCRIPTEN__
		EM_ASM({ console.log('[rec_wasm] Exited mainloop #' + $0 + ' cache=' + $1 +
			' blocks=' + $2 + ' interp=' + $3 + ' ts=' + $4); },
			mainloop_count, wasm_cache_size(), blockExecs, interpExecs, timeslices);
#endif
	}

	void handleException(host_context_t& context) override {}

	bool rewrite(host_context_t& context, void* faultAddress) override
	{
		return false;
	}

	void reset() override
	{
		wasm_clear_cache();
		blockByVaddr.clear();
		blockCodeHash.clear();
		compiledCount = 0;
		failCount = 0;
#ifdef __EMSCRIPTEN__
		EM_ASM({ console.log('[rec_wasm] reset: cleared WASM block cache'); });
#endif
	}

	void canonStart(const shil_opcode* op) override {}
	void canonParam(const shil_opcode* op, const shil_param* par, CanonicalParamType tp) override {}
	void canonCall(const shil_opcode* op, void* function) override {}
	void canonFinish(const shil_opcode* op) override {}

private:
	Sh4Context* sh4ctx = nullptr;
	Sh4CodeBuffer* codeBuffer = nullptr;
	u32 compiledCount = 0;
	u32 failCount = 0;
};

static WasmDynarec instance;

extern "C" void wasm_dynarec_init()
{
	if (!sh4Dynarec)
		sh4Dynarec = &instance;
}

#endif // FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_GENERIC
