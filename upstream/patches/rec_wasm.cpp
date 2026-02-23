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
int wasm_execute_block(u32 block_pc, u32 ctx_ptr, u32 ram_base);
int wasm_has_block(u32 block_pc);
void wasm_clear_cache();
void wasm_remove_block(u32 block_pc);
int wasm_cache_size();
double wasm_prof_compile_ms();
double wasm_prof_exec_sample_ms();
int wasm_prof_exec_samples();
int wasm_prof_exec_count();
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
// Profiling counters
// ============================================================
static u32 prof_native_ops_compiled = 0;   // SHIL ops compiled to native WASM
static u32 prof_fallback_ops_compiled = 0; // SHIL ops compiled as C++ fallback
static u32 prof_idle_loops_detected = 0;   // blocks detected as idle spin-wait loops
static u32 prof_multiblock_modules = 0;   // multi-block super-block modules compiled
static u32 prof_multiblock_total_blocks = 0; // total blocks in super-block modules
static u32 prof_fb_by_op[128] = {};        // runtime fallback calls by op type
static double prof_emulation_ms = 0;       // time in inner block dispatch loop
static double prof_system_ms = 0;          // time in UpdateSystem_INTC

// ============================================================
// C dispatch table: PC hash → indirect function table index
// ============================================================
// Each compiled WASM block is registered in Emscripten's
// __indirect_function_table. The dispatch table maps PC hashes
// to table indices. c_dispatch_loop uses call_indirect to call
// blocks entirely within WASM — no JS in the hot path.
#define JIT_TABLE_SIZE (1 << 20)  // 1M entries (~4MB)
#define JIT_TABLE_MASK (JIT_TABLE_SIZE - 1)
static u32 jit_dispatch_table[JIT_TABLE_SIZE];  // PC hash → table index (0 = miss)

// Dispatch loop exit status
static int g_dispatch_result = 0;    // 0=timeslice, 1=miss, 3=interrupt
static u32 g_dispatch_miss_pc = 0;

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

// Returns the heap offset of main RAM buffer for direct WASM memory access.
// On Emscripten, malloc'd pointers ARE linear memory offsets.
u32 EMSCRIPTEN_KEEPALIVE wasm_get_ram_base() {
	return (u32)(uintptr_t)&mem_b[0];
}

u32 EMSCRIPTEN_KEEPALIVE wasm_get_vram_base() {
	extern RamRegion vram;
	return (u32)(uintptr_t)&vram[0];
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

	// Profiling: count runtime fallback calls by op type
	if ((int)op.op >= 0 && (int)op.op < 128) prof_fb_by_op[(int)op.op]++;

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
#define EXECUTOR_MODE 6
#define SHIL_START_BLOCK 24168000

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


#if EXECUTOR_MODE == 0
	// REF executor (per-instruction charging)
	ref_execute_block(block);
#elif EXECUTOR_MODE == 3
	// PERIODIC SHADOW COMPARISON: SHIL for all blocks, with periodic ref comparison.
	// Every SHADOW_INTERVAL blocks, run 10 blocks through both ref and SHIL,
	// comparing full Sh4Context (512 bytes). This covers the entire execution range
	// at SHIL speed, catching rare SHIL op bugs that only manifest after millions of blocks.
	{
		static u32 shadow_diff_count = 0;
		// Do shadow comparison for 10 blocks every 500K blocks
		bool do_shadow = (g_wasm_block_count % 500000 < 10) && (shadow_diff_count < 100);

		if (do_shadow) {
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
			bool any_diff = false;
			int first_diff_off = -1;
			u32 first_diff_ref = 0, first_diff_shil = 0;
			int diff_count = 0;
			for (int off = 0; off < (int)sizeof(Sh4Context); off += 4) {
				// Skip cycle_counter and doSqWrite pointer
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
				shadow_diff_count++;
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

				EM_ASM({ console.log('[SHADOW3] #' + $0 +
					' blk=' + $1 +
					' pc=0x' + ($2>>>0).toString(16) +
					' diffs=' + $3 +
					' first_off=0x' + ($4>>>0).toString(16) +
					' field=' + UTF8ToString($5) +
					' ref=0x' + ($6>>>0).toString(16) +
					' shil=0x' + ($7>>>0).toString(16) +
					' nops=' + $8); },
					shadow_diff_count, g_wasm_block_count, block->vaddr,
					diff_count, first_diff_off, field_name,
					first_diff_ref, first_diff_shil,
					(u32)block->oplist.size());

				// For first 10 diffs, dump all differing offsets + oplist
				if (shadow_diff_count <= 10) {
					for (int off = 0; off < (int)sizeof(Sh4Context); off += 4) {
						if (off == 0x174 || off == 0x178) continue;
						u32 rv = *(u32*)(ref_bytes + off);
						u32 sv = *(u32*)(shil_bytes + off);
						if (rv != sv) {
							EM_ASM({ console.log('[SHADOW3-DIFF] off=0x' + ($0>>>0).toString(16) +
								' ref=0x' + ($1>>>0).toString(16) +
								' shil=0x' + ($2>>>0).toString(16)); },
								off, rv, sv);
						}
					}
					for (u32 i = 0; i < block->oplist.size() && i < 30; i++) {
						auto& sop = block->oplist[i];
						EM_ASM({ console.log('[SHADOW3-OP] [' + $0 + '] shop=' + $1 +
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
			} else {
				// Log periodic match confirmation
				if (g_wasm_block_count % 500000 == 0) {
					EM_ASM({ console.log('[SHADOW3-OK] blk=' + $0 +
						' pc=0x' + ($1>>>0).toString(16) +
						' nops=' + $2); },
						g_wasm_block_count, block->vaddr,
						(u32)block->oplist.size());
				}
			}
#endif
			// Use SHIL's result for continued execution (since we're running in SHIL mode)
			// Do NOT restore ref — this is SHIL execution with periodic checks
		} else {
			// Pure SHIL (majority of blocks)
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
		u32 ram_ptr = (u32)(uintptr_t)&mem_b[0];
		int trap = wasm_execute_block(block->vaddr, ctx_ptr, ram_ptr);
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
	{
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
#endif


	g_wasm_block_count++;
}

// ============================================================
// EM_JS bridge: compile + execute WASM blocks from JavaScript
// ============================================================

#ifdef __EMSCRIPTEN__

EM_JS(int, wasm_compile_block, (const u8* bytesPtr, u32 len, u32 block_pc), {
	if (!Module._prof) Module._prof = { compileMs: 0, execMs: 0, execSamples: 0, execCount: 0 };
	var t0 = performance.now();
	try {
		var wasmBytes = Module.HEAPU8.slice(bytesPtr, bytesPtr + len);
		var mod = new WebAssembly.Module(wasmBytes);
		// Use Emscripten's C export wrappers directly (no extra arrow-function layer).
		// Raw WASM exports have mangled names due to -O3 -flto, so we use Module._fn.
		// The main perf win is the C dispatch loop (call_indirect), not the import path.
		var instance = new WebAssembly.Instance(mod, {
			env: {
				memory: wasmMemory,
				read8:   Module._wasm_mem_read8,
				read16:  Module._wasm_mem_read16,
				read32:  Module._wasm_mem_read32,
				write8:  Module._wasm_mem_write8,
				write16: Module._wasm_mem_write16,
				write32: Module._wasm_mem_write32,
				ifb:     Module._wasm_exec_ifb,
				shil_fb: Module._wasm_exec_shil_fb
			}
		});

		// Register in shared indirect function table for call_indirect dispatch
		var table = wasmTable;
		if (!Module._jitTableBase) {
			Module._jitTableBase = table.length;
			Module._jitNextIdx = table.length;
			table.grow(4096);  // pre-allocate in bulk (not one-by-one)
		}
		var idx = Module._jitNextIdx++;
		if (idx >= table.length) {
			table.grow(4096);
		}
		table.set(idx, instance.exports.b);

		// Keep JS cache for debug/fallback (wasm_execute_block)
		if (!Module._wasmBlockCache) Module._wasmBlockCache = {};
		Module._wasmBlockCache[block_pc] = instance.exports.b;
		Module._prof.compileMs += performance.now() - t0;
		return idx;  // table index (>0 on success, 0 reserved for NULL)
	} catch (e) {
		console.error('[rec_wasm] compile fail PC=0x' + (block_pc >>> 0).toString(16) + ': ' + e.message);
		return 0;
	}
});

EM_JS(int, wasm_execute_block, (u32 block_pc, u32 ctx_ptr, u32 ram_base), {
	try {
		Module._prof.execCount++;
		// Sample every 1000th call for timing (overhead: ~0.1%)
		if ((Module._prof.execCount & 0x3FF) === 0) {
			var t0 = performance.now();
			Module._wasmBlockCache[block_pc](ctx_ptr, ram_base);
			Module._prof.execMs += performance.now() - t0;
			Module._prof.execSamples++;
		} else {
			Module._wasmBlockCache[block_pc](ctx_ptr, ram_base);
		}
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
	// Reset table allocation — old entries become unreachable
	Module._jitTableBase = 0;
	Module._jitNextIdx = 0;
});

EM_JS(void, wasm_remove_block, (u32 block_pc), {
	if (Module._wasmBlockCache) delete Module._wasmBlockCache[block_pc];
});

EM_JS(int, wasm_cache_size, (), {
	return Module._wasmBlockCache ? Object.keys(Module._wasmBlockCache).length : 0;
});

// Profiling data readers
EM_JS(double, wasm_prof_compile_ms, (), {
	return Module._prof ? Module._prof.compileMs : 0;
});
EM_JS(double, wasm_prof_exec_sample_ms, (), {
	return Module._prof ? Module._prof.execMs : 0;
});
EM_JS(int, wasm_prof_exec_samples, (), {
	return Module._prof ? Module._prof.execSamples : 0;
});
EM_JS(int, wasm_prof_exec_count, (), {
	return Module._prof ? Module._prof.execCount : 0;
});

// C dispatch loop: runs compiled WASM blocks via call_indirect.
// Blocks stay entirely within WASM — no JS in the hot path.
// Returns number of blocks executed. g_dispatch_result indicates exit reason:
//   0 = timeslice complete (cycle_counter <= 0)
//   1 = cache miss (g_dispatch_miss_pc = PC needing compilation)
//   3 = interrupt pending (needs C++ UpdateINTC)
static int c_dispatch_loop(u32 ctx_ptr, u32 ram_base) {
	typedef void (*block_fn_t)(u32, u32);
	Sh4Context& ctx = Sh4cntx;
	int blocks_run = 0;

	while (ctx.cycle_counter > 0) {
		u32 pc = ctx.pc;
		u32 key = (pc >> 1) & JIT_TABLE_MASK;
		u32 table_idx = jit_dispatch_table[key];

		if (table_idx == 0) {
			g_dispatch_result = 1;  // miss
			g_dispatch_miss_pc = pc;
			return blocks_run;
		}

		// Cast table index to function pointer — Emscripten compiles
		// this to call_indirect, staying entirely within WASM.
		block_fn_t fn = (block_fn_t)(uintptr_t)table_idx;
		fn(ctx_ptr, ram_base);
		blocks_run++;

		if (ctx.interrupt_pend) {
			g_dispatch_result = 3;  // interrupt
			return blocks_run;
		}
	}

	g_dispatch_result = 0;  // timeslice complete
	return blocks_run;
}

#else
static int wasm_compile_block(const u8*, u32, u32) { return 0; }
static int wasm_execute_block(u32, u32, u32) { return 0; }
static int wasm_has_block(u32) { return 0; }
static void wasm_remove_block(u32) {}
static void wasm_clear_cache() {}
static int wasm_cache_size() { return 0; }
static double wasm_prof_compile_ms() { return 0; }
static double wasm_prof_exec_sample_ms() { return 0; }
static int wasm_prof_exec_samples() { return 0; }
static int wasm_prof_exec_count() { return 0; }

static int c_dispatch_loop(u32, u32) { return 0; }
#endif

// ============================================================
// Build a complete WASM module for one compiled block
// ============================================================

static bool buildBlockModule(WasmModuleBuilder& b, RuntimeBlockInfo* block) {
	// Pre-scan for register usage — allocate WASM locals for cached regs
	RegCache cache;
	cache.scanBlock(block);

	// Idle loop detection: blocks that branch back to themselves
	bool is_idle_loop = false;
	u32 bcls = BET_GET_CLS(block->BlockType);
	if (bcls == BET_CLS_Static && block->BlockType != BET_StaticIntr
		&& block->BranchBlock == block->vaddr) {
		is_idle_loop = true;
	}
	if (bcls == BET_CLS_COND
		&& (block->BranchBlock == block->vaddr || block->NextBlock == block->vaddr)) {
		is_idle_loop = true;
	}
	if (is_idle_loop) prof_idle_loops_detected++;

	b.emitHeader();

	// Type section: 3 function signatures
	// Type 0: (i32, i32) -> void — block function (ctx_ptr, ram_base)
	// Type 1: (i32) -> i32       — read8/16/32
	// Type 2: (i32, i32) -> void — write8/16/32, ifb, shil_fb
	b.emitTypeSection(3);
	{
		u8 p0[] = { WASM_TYPE_I32, WASM_TYPE_I32 };
		b.emitFuncType(p0, 2, nullptr, 0);

		u8 p1[] = { WASM_TYPE_I32 };
		u8 r1[] = { WASM_TYPE_I32 };
		b.emitFuncType(p1, 1, r1, 1);

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

	// Locals: 1 temp i32 + N cached register i32s
	u32 totalExtraLocals = 1 + cache.localCount();
	u32 lc = totalExtraLocals;
	u8 lt = WASM_TYPE_I32;
	b.emitLocals(1, &lc, &lt);

	// Prologue: load cached registers from ctx memory into WASM locals
	for (auto& [offset, entry] : cache.entries) {
		b.op_local_get(LOCAL_CTX);
		b.op_i32_load(offset);
		b.op_local_set(entry.wasmLocal);
	}

	// Prologue: cycle_counter -= guest_cycles
	b.op_local_get(LOCAL_CTX);
	b.op_local_get(LOCAL_CTX);
	b.op_i32_load(ctx_off::CYCLE_COUNTER);
	b.op_i32_const((s32)block->guest_cycles);
	b.op_i32_sub();
	b.op_i32_store(ctx_off::CYCLE_COUNTER);

	// Emit each SHIL op with register cache
	for (u32 i = 0; i < block->oplist.size(); i++) {
		shil_opcode& op = block->oplist[i];
		if (!emitShilOp(b, op, block, i, cache)) {
			prof_fallback_ops_compiled++;
			// Unhandled op — flush, call fallback, reload
			emitFlushAll(b, cache);
			b.op_i32_const((s32)block->vaddr);
			b.op_i32_const((s32)i);
			b.op_call(WIMPORT_SHIL_FB);
			emitReloadAll(b, cache);
		} else {
			prof_native_ops_compiled++;
		}
	}

	// Epilogue: block exit reads sr.T/jdyn from cached locals
	emitBlockExit(b, block, cache);

	// Epilogue: writeback all dirty cached registers to ctx memory
	emitFlushAll(b, cache);

	// Idle loop fast-forward: set cycle_counter = 0 when looping back to self
	if (is_idle_loop) {
		if (bcls == BET_CLS_Static) {
			// Unconditional self-loop: always fast-forward
			b.op_local_get(LOCAL_CTX);
			b.op_i32_const(0);
			b.op_i32_store(ctx_off::CYCLE_COUNTER);
		} else {
			// Conditional self-loop: fast-forward only when branching back
			b.op_local_get(LOCAL_CTX);
			b.op_i32_load(ctx_off::PC);
			b.op_i32_const((s32)block->vaddr);
			b.op_i32_eq();
			b.op_if(); // void block type (0x40)
			b.op_local_get(LOCAL_CTX);
			b.op_i32_const(0);
			b.op_i32_store(ctx_off::CYCLE_COUNTER);
			b.op_end();
		}
	}

	b.endFuncBody();
	b.endSection();

	return true;
}

// ============================================================
// Multi-block module: chain connected blocks into one WASM module
// ============================================================

static constexpr int MULTIBLOCK_MAX = 4;

// Flush ALL cached entries to ctx memory unconditionally (ignores dirty flag).
// Used at multi-block exit points where compile-time dirty tracking is unreliable.
static void emitFlushAllUnconditional(WasmModuleBuilder& b, const RegCache& cache) {
	for (auto& [offset, entry] : cache.entries) {
		b.op_local_get(LOCAL_CTX);
		b.op_local_get(entry.wasmLocal);
		b.op_i32_store(offset);
	}
}

// Discover a chain of statically-connected blocks for multi-block compilation.
// Only follows unconditional static jumps to already-compiled blocks.
static std::vector<RuntimeBlockInfo*> discoverChain(RuntimeBlockInfo* entry) {
	std::vector<RuntimeBlockInfo*> chain;
	chain.push_back(entry);

	RuntimeBlockInfo* current = entry;
	while ((int)chain.size() < MULTIBLOCK_MAX) {
		u32 bcls = BET_GET_CLS(current->BlockType);
		if (bcls != BET_CLS_Static || current->BlockType == BET_StaticIntr)
			break;

		u32 nextPC = current->BranchBlock;
		if (nextPC == 0xFFFFFFFF) break;
		if (nextPC == current->vaddr) break;  // self-loop handled by idle detection

		auto it = blockByVaddr.find(nextPC);
		if (it == blockByVaddr.end()) break;

		RuntimeBlockInfo* next = it->second;

		// Avoid duplicates (loop in chain)
		bool dup = false;
		for (auto* b : chain) {
			if (b->vaddr == next->vaddr) { dup = true; break; }
		}
		if (dup) break;

		chain.push_back(next);
		current = next;
	}
	return chain;
}

// Build a multi-block WASM module with internal dispatch loop.
//
// WASM nesting (br depths from inside a block's if body):
//   block $exit {            // br(2) = exit
//     loop $dispatch {       // br(1) = re-dispatch (loop back)
//       if (idx == i) {      // br(0) = fall through to next if
//         ...
//       }
//     }
//   }
//   <final flush runs here after any br $exit>
//
// Register cache: shared across all blocks. Compile-time dirty tracking is
// unreliable across multiple blocks, so we use emitFlushAllUnconditional at
// all exit points. Within a single block's SHIL ops, emitFlushAll/emitReloadAll
// for ifb/shil_fb fallbacks works correctly.
static bool buildMultiBlockModule(WasmModuleBuilder& b,
                                   const std::vector<RuntimeBlockInfo*>& chain) {
	// Unified register cache across all blocks
	RegCache cache;
	for (auto* blk : chain) {
		cache.scanBlock(blk);
	}

	// PC → chain index map
	std::unordered_map<u32, u32> pcToIdx;
	for (u32 i = 0; i < chain.size(); i++) {
		pcToIdx[chain[i]->vaddr] = i;
	}

	// Extra local for dispatch index
	u32 LOCAL_NEXT_IDX = 3 + cache.localCount();

	b.emitHeader();

	// Type section: same 3 types
	b.emitTypeSection(3);
	{
		u8 p0[] = { WASM_TYPE_I32, WASM_TYPE_I32 };
		b.emitFuncType(p0, 2, nullptr, 0);
		u8 p1[] = { WASM_TYPE_I32 };
		u8 r1[] = { WASM_TYPE_I32 };
		b.emitFuncType(p1, 1, r1, 1);
		u8 p2[] = { WASM_TYPE_I32, WASM_TYPE_I32 };
		b.emitFuncType(p2, 2, nullptr, 0);
	}
	b.endSection();

	// Import section
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

	u32 typeIdx = 0;
	b.emitFunctionSection(1, &typeIdx);
	b.emitExportSection("b", WIMPORT_COUNT);

	b.beginCodeSection(1);
	b.beginFuncBody();

	// Locals: 1 temp + N cached regs + 1 dispatch index
	u32 totalExtraLocals = 1 + cache.localCount() + 1;
	u32 lc = totalExtraLocals;
	u8 lt = WASM_TYPE_I32;
	b.emitLocals(1, &lc, &lt);

	// Prologue: load all cached registers
	for (auto& [offset, entry] : cache.entries) {
		b.op_local_get(LOCAL_CTX);
		b.op_i32_load(offset);
		b.op_local_set(entry.wasmLocal);
	}

	// Initialize dispatch index = 0 (entry block)
	b.op_i32_const(0);
	b.op_local_set(LOCAL_NEXT_IDX);

	b.op_block();  // $exit — br(2) from if body, br(1) from loop body
	b.op_loop();   // $dispatch — br(1) from if body, br(0) from loop body

	// --- Cycle counter check ---
	b.op_local_get(LOCAL_CTX);
	b.op_i32_load(ctx_off::CYCLE_COUNTER);
	b.op_i32_const(0);
	b.op_i32_le_s();
	b.op_br_if(1);  // br $exit (from loop body: depth 1)

	// --- Interrupt check ---
	b.op_local_get(LOCAL_CTX);
	b.op_i32_load(0x16C);
	b.op_br_if(1);  // br $exit

	// --- Dispatch each block ---
	for (u32 i = 0; i < chain.size(); i++) {
		RuntimeBlockInfo* blk = chain[i];

		b.op_local_get(LOCAL_NEXT_IDX);
		b.op_i32_const((s32)i);
		b.op_i32_eq();
		b.op_if();  // if body: $exit=br(2), $dispatch=br(1), this if=br(0)

		// Mark all entries dirty before this block (conservative: ensures
		// correct flushing regardless of which previous block executed)
		for (auto& [offset, entry] : cache.entries)
			entry.dirty = true;

		// Decrement cycle_counter
		b.op_local_get(LOCAL_CTX);
		b.op_local_get(LOCAL_CTX);
		b.op_i32_load(ctx_off::CYCLE_COUNTER);
		b.op_i32_const((s32)blk->guest_cycles);
		b.op_i32_sub();
		b.op_i32_store(ctx_off::CYCLE_COUNTER);

		// Emit SHIL ops (shared cache, ifb/shil_fb uses flush+reload)
		for (u32 j = 0; j < blk->oplist.size(); j++) {
			shil_opcode& op = blk->oplist[j];
			if (!emitShilOp(b, op, blk, j, cache)) {
				prof_fallback_ops_compiled++;
				emitFlushAll(b, cache);
				b.op_i32_const((s32)blk->vaddr);
				b.op_i32_const((s32)j);
				b.op_call(WIMPORT_SHIL_FB);
				emitReloadAll(b, cache);
			} else {
				prof_native_ops_compiled++;
			}
		}

		// Block exit: writes next PC to ctx memory
		emitBlockExit(b, blk, cache);

		// Route to next block or exit
		u32 bcls_blk = BET_GET_CLS(blk->BlockType);

		if (bcls_blk == BET_CLS_Static && blk->BlockType != BET_StaticIntr) {
			auto target = pcToIdx.find(blk->BranchBlock);
			if (target != pcToIdx.end()) {
				// Target in chain: set dispatch index, loop back
				b.op_i32_const((s32)target->second);
				b.op_local_set(LOCAL_NEXT_IDX);
				b.op_br(1);  // br $dispatch (from if: depth 1)
			} else {
				// Target outside chain: exit
				b.op_br(2);  // br $exit (from if: depth 2)
			}
		} else if (bcls_blk == BET_CLS_COND) {
			// Conditional: check PC against known targets
			auto branchTarget = pcToIdx.find(blk->BranchBlock);
			auto nextTarget = pcToIdx.find(blk->NextBlock);

			if (branchTarget != pcToIdx.end() && nextTarget != pcToIdx.end()) {
				// Both targets in chain
				b.op_local_get(LOCAL_CTX);
				b.op_i32_load(ctx_off::PC);
				b.op_i32_const((s32)blk->BranchBlock);
				b.op_i32_eq();
				b.op_if();  // inner if: $exit=br(3), $dispatch=br(2), outer if=br(1)
				b.op_i32_const((s32)branchTarget->second);
				b.op_local_set(LOCAL_NEXT_IDX);
				b.op_else();
				b.op_i32_const((s32)nextTarget->second);
				b.op_local_set(LOCAL_NEXT_IDX);
				b.op_end();
				b.op_br(1);  // br $dispatch (from outer if: depth 1)
			} else if (branchTarget != pcToIdx.end()) {
				// Only branch target in chain
				b.op_local_get(LOCAL_CTX);
				b.op_i32_load(ctx_off::PC);
				b.op_i32_const((s32)blk->BranchBlock);
				b.op_i32_eq();
				b.op_if();  // inner if
				b.op_i32_const((s32)branchTarget->second);
				b.op_local_set(LOCAL_NEXT_IDX);
				b.op_br(2);  // br $dispatch (from inner if: depth 2)
				b.op_end();
				b.op_br(2);  // br $exit (from outer if: depth 2)
			} else if (nextTarget != pcToIdx.end()) {
				// Only fall-through in chain
				b.op_local_get(LOCAL_CTX);
				b.op_i32_load(ctx_off::PC);
				b.op_i32_const((s32)blk->NextBlock);
				b.op_i32_eq();
				b.op_if();  // inner if
				b.op_i32_const((s32)nextTarget->second);
				b.op_local_set(LOCAL_NEXT_IDX);
				b.op_br(2);  // br $dispatch (from inner if: depth 2)
				b.op_end();
				b.op_br(2);  // br $exit (from outer if: depth 2)
			} else {
				b.op_br(2);  // br $exit
			}
		} else {
			// Dynamic or other: must exit super-block
			b.op_br(2);  // br $exit
		}

		b.op_end();  // end if (block index check)
	}

	// Default: no block matched (shouldn't happen), exit
	b.op_br(1);  // br $exit (from loop body: depth 1)

	b.op_end();  // end loop $dispatch
	b.op_end();  // end block $exit

	// Final unconditional flush: runs on ALL exit paths
	// (cycle check, interrupt, target outside chain, dynamic branch)
	emitFlushAllUnconditional(b, cache);

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
#endif
		sh4ctx = &ctx;
		codeBuffer = &buf;
		compiledCount = 0;
		failCount = 0;
	}

	void compile(RuntimeBlockInfo* block, bool smc_checks, bool optimise) override
	{
		// Handle FPCB aliasing: SH4 address mirrors (0x0C/0x8C/0xAC) map to
		// the same FPCB index via (addr>>1)&FPCB_MASK. If another block
		// already occupies this slot, clear it to prevent bm_AddBlock verify
		// failure. Standard dynarec handles this via block-check code at the
		// start of each compiled block; we bypass FPCB dispatch entirely
		// (JS cache uses exact PC), so aliased entries persist.
		// Note: we access p_sh4rcb->fpcb directly because bm_GetBlock()
		// uses containsCode() which requires host_code_size > 0, but our
		// WASM blocks use dummy code pointers with zero host_code_size.
		{
			DynarecCodeEntryPtr& fpcb_entry =
				(DynarecCodeEntryPtr&)p_sh4rcb->fpcb[(block->addr >> 1) & FPCB_MASK];
			if ((void*)fpcb_entry != (void*)ngen_FailedToFindBlock) {
				fpcb_entry = ngen_FailedToFindBlock;
			}
		}

		blockByVaddr[block->vaddr] = block;

		// Store hash for SMC detection (first 2 bytes of block code)
		blockCodeHash[block->vaddr] = (u32)IReadMem16(block->vaddr);

#if EXECUTOR_MODE == 6
		// Only build WASM modules when using WASM execution
		WasmModuleBuilder builder;

		// Try multi-block: chain statically-connected blocks
		auto chain = discoverChain(block);
		if (chain.size() >= 2) {
			buildMultiBlockModule(builder, chain);
			prof_multiblock_modules++;
			prof_multiblock_total_blocks += (u32)chain.size();
		} else {
			buildBlockModule(builder, block);
		}

		const auto& bytes = builder.getBytes();
		int table_idx = wasm_compile_block(bytes.data(), (u32)bytes.size(), block->vaddr);

		if (table_idx > 0) {
			// Store in dispatch table — (pc>>1)&MASK handles address aliasing
			jit_dispatch_table[(block->vaddr >> 1) & JIT_TABLE_MASK] = (u32)table_idx;
			compiledCount++;
		} else {
			failCount++;
		}
#else
		compiledCount++;
#endif

		// Dummy code pointer for block manager (4 bytes per block)
		block->code = (DynarecCodeEntryPtr)codeBuffer->get();
		block->host_code_size = 4;
		if (codeBuffer->getFreeSpace() >= 4)
			codeBuffer->advance(4);

#ifdef __EMSCRIPTEN__
		if (compiledCount <= 10 || (compiledCount % 200 == 0)) {
			EM_ASM({ console.log('[rec_wasm] compiled=' + $0 + ' fail=' + $1 +
				' pc=0x' + ($2>>>0).toString(16) + ' ops=' + $3); },
				compiledCount, failCount, block->vaddr,
				(int)block->oplist.size());
		}
#endif
	}

	void mainloop(void* cntx) override
	{
		// CRITICAL: Branch instructions with delay slots call executeDelaySlot()
		// which dereferences Sh4Interpreter::Instance.
		Sh4Interpreter::Instance = Sh4Recompiler::Instance;

		// (per-frame cache flush removed — stale block theory disproven)

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
		double ml_start = emscripten_get_now();
		u32 fb_count_start = g_shil_fb_call_count;
		double compile_ms_start = wasm_prof_compile_ms();

		try {
			do {
				try {
					double emu_t0 = emscripten_get_now();
					u32 ctx_ptr = (u32)(uintptr_t)sh4ctx;
					u32 ram_ptr = (u32)(uintptr_t)&mem_b[0];

					// Restored: WASM dispatch via call_indirect (SHIL path)
					while (sh4ctx->cycle_counter > 0) {
						int nblocks = c_dispatch_loop(ctx_ptr, ram_ptr);
						blockExecs += nblocks;
						g_wasm_block_count += nblocks;

						if (g_dispatch_result == 0) {
							break;  // timeslice complete
						} else if (g_dispatch_result == 1) {
							// Cache miss — compile the block
							u32 miss_pc = g_dispatch_miss_pc;
							auto it = blockByVaddr.find(miss_pc);
							if (it == blockByVaddr.end()) {
								rdv_FailedToFindBlock(miss_pc);
								it = blockByVaddr.find(miss_pc);
							}
							if (it == blockByVaddr.end()) {
								sh4ctx->pc = miss_pc + 2;
								u16 rawOp = IReadMem16(miss_pc);
								if (sh4ctx->sr.FD == 1 && OpDesc[rawOp]->IsFloatingPoint())
									throw SH4ThrownException(miss_pc, Sh4Ex_FpuDisabled);
								OpPtr[rawOp](sh4ctx, rawOp);
								sh4ctx->cycle_counter -= 1;
								interpExecs++;
							}
						} else if (g_dispatch_result == 3) {
							// Interrupt pending
							UpdateINTC();
						}
					}

					double emu_t1 = emscripten_get_now();
					prof_emulation_ms += (emu_t1 - emu_t0);

					sh4ctx->cycle_counter += SH4_TIMESLICE;
					timeslices++;

					double sys_t0 = emscripten_get_now();
					UpdateSystem_INTC();
					double sys_t1 = emscripten_get_now();
					prof_system_ms += (sys_t1 - sys_t0);

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
		// Profiling dump at mainloop exit
		{
			double ml_elapsed = emscripten_get_now() - ml_start;
			double compile_ms = wasm_prof_compile_ms() - compile_ms_start;
			u32 fb_calls = g_shil_fb_call_count - fb_count_start;

			// Other time = total - emulation - system
			double other_ms = ml_elapsed - prof_emulation_ms - prof_system_ms;

			EM_ASM({
				var total = $0;
				var emu = $1;
				var sys = $2;
				var compile = $3;
				var other = $4;
				var blks = $5;
				var ts = $6;
				var fbCalls = $7;
				var nativeOps = $8;
				var fbOps = $9;

				var pct = function(v) { return total > 0 ? (v / total * 100).toFixed(1) : '?'; };
				console.log('');
				console.log('=== PROFILING REPORT (mainloop #' + $10 + ') ===');
				console.log('Dispatch: call_indirect (C dispatch loop, no JS)');
				console.log('Total wall time:  ' + total.toFixed(0) + ' ms');
				console.log('');
				console.log('--- Time Breakdown ---');
				console.log('Emulation (C dispatch+exec):  ' + emu.toFixed(0) + ' ms (' + pct(emu) + '%)');
				console.log('System (render/INTC):         ' + sys.toFixed(0) + ' ms (' + pct(sys) + '%)');
				console.log('Compilation:                  ' + compile.toFixed(1) + ' ms (' + pct(compile) + '%)');
				console.log('Other (overhead):             ' + other.toFixed(0) + ' ms (' + pct(other) + '%)');
				console.log('');
				console.log('--- Block Stats ---');
				console.log('Blocks executed:  ' + blks.toLocaleString());
				console.log('Timeslices:       ' + ts.toLocaleString());
				console.log('Blocks/timeslice: ' + (blks / ts).toFixed(1));
				console.log('Blocks/ms:        ' + (blks / total).toFixed(1));
				console.log('Idle loops:       ' + $11);
				console.log('');
				console.log('--- Op Coverage (compile-time) ---');
				console.log('Native WASM ops:  ' + nativeOps.toLocaleString());
				console.log('Fallback C++ ops: ' + fbOps.toLocaleString());
				console.log('Native ratio:     ' + (nativeOps / (nativeOps + fbOps) * 100).toFixed(1) + '%');
				console.log('');
				console.log('--- Runtime Fallback Calls ---');
				console.log('Total FB calls:   ' + fbCalls.toLocaleString());
				console.log('FB calls/block:   ' + (fbCalls / blks).toFixed(2));
				console.log('=== END PROFILING ===');
				console.log('');
			},
				ml_elapsed,          // $0 total
				prof_emulation_ms,   // $1 emu
				prof_system_ms,      // $2 sys
				compile_ms,          // $3 compile
				other_ms,            // $4 other
				blockExecs,          // $5 blks
				timeslices,          // $6 ts
				fb_calls,            // $7 fbCalls
				prof_native_ops_compiled,   // $8 nativeOps
				prof_fallback_ops_compiled, // $9 fbOps
				mainloop_count,      // $10 mainloop#
				prof_idle_loops_detected  // $11 idle loops
			);

			// Multi-block stats (separate EM_ASM to avoid 16-arg limit)
			EM_ASM({ console.log('Multi-blocks:     ' + $0 + ' modules (' + $1 + ' blocks)'); },
				prof_multiblock_modules, prof_multiblock_total_blocks);

			// Dump top fallback ops by frequency
			struct FbEntry { int op; u32 count; };
			FbEntry top[10] = {};
			for (int i = 0; i < 128; i++) {
				if (prof_fb_by_op[i] > 0) {
					// Insert into sorted top-10
					for (int j = 0; j < 10; j++) {
						if (prof_fb_by_op[i] > top[j].count) {
							for (int k = 9; k > j; k--) top[k] = top[k-1];
							top[j] = { i, prof_fb_by_op[i] };
							break;
						}
					}
				}
			}
			EM_ASM({ console.log('--- Top Fallback Ops (runtime) ---'); });
			for (int j = 0; j < 10 && top[j].count > 0; j++) {
				EM_ASM({ console.log('  shop_' + $0 + ': ' + $1.toLocaleString() + ' calls'); },
					top[j].op, top[j].count);
			}

			// Reset profiling for next mainloop
			prof_emulation_ms = 0;
			prof_system_ms = 0;
			memset(prof_fb_by_op, 0, sizeof(prof_fb_by_op));
		}
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
		memset(jit_dispatch_table, 0, sizeof(jit_dispatch_table));
		blockByVaddr.clear();
		blockCodeHash.clear();
		compiledCount = 0;
		failCount = 0;
#ifdef __EMSCRIPTEN__
		EM_ASM({ console.log('[rec_wasm] reset: cleared WASM block cache + dispatch table'); });
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
