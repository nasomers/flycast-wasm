// wasm_emit.h — SHIL → WASM instruction emitters for Flycast JIT
//
// Translates individual SHIL IR opcodes into WASM instructions using
// WasmModuleBuilder. Operates on Sh4Context in shared linear memory.

#pragma once
#include "wasm_module_builder.h"
#include "hw/sh4/dyna/shil.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/sh4/dyna/decoder.h"
#include <unordered_map>

// Import function indices (must match the order in rec_wasm.cpp buildModule)
enum WasmImportFunc : u32 {
	WIMPORT_READ8  = 0,
	WIMPORT_READ16 = 1,
	WIMPORT_READ32 = 2,
	WIMPORT_WRITE8  = 3,
	WIMPORT_WRITE16 = 4,
	WIMPORT_WRITE32 = 5,
	WIMPORT_IFB     = 6,    // (opcode, pc) -> void
	WIMPORT_SHIL_FB = 7,    // (block_vaddr, op_idx) -> void
	WIMPORT_COUNT   = 8
};

// Sh4Context field offsets (verified against getRegOffset in shil.cpp)
// These are passed as offsets to i32.load/i32.store with ctx_ptr as base.
namespace ctx_off {
	// Use getRegOffset() at compile time via shil_param::reg_offset()
	// These constants are for fields not accessible via reg_offset:
	constexpr u32 PC            = 0x148;  // offsetof(Sh4Context, pc)
	constexpr u32 JDYN          = 0x14C;  // offsetof(Sh4Context, jdyn)
	constexpr u32 SR_STATUS     = 0x150;  // offsetof(Sh4Context, sr.status)
	constexpr u32 SR_T          = 0x154;  // offsetof(Sh4Context, sr.T)
	constexpr u32 CYCLE_COUNTER = 0x174;  // offsetof(Sh4Context, cycle_counter)
}

// Local variable indices in the compiled WASM function
// Local 0 = ctx_ptr (function parameter)
// Local 1 = ram_base (function parameter — heap offset of Dreamcast main RAM)
// Local 2 = temp i32 (for intermediate values)
constexpr u32 LOCAL_CTX = 0;
constexpr u32 LOCAL_RAM = 1;
constexpr u32 LOCAL_TMP = 2;

// ============================================================
// Register Cache — maps Sh4Context offsets to WASM locals
// ============================================================
// Caches frequently-used integer registers in WASM locals instead
// of loading/storing from linear memory every op. V8 maps locals
// directly to CPU registers (essentially free) vs i32.load/store
// which go through the linear memory path (3-5x slower).

struct RegCacheEntry {
	u32 wasmLocal;   // WASM local index (starting at 3)
	bool dirty;      // needs writeback at block exit
};

struct RegCache {
	std::unordered_map<u32, RegCacheEntry> entries;  // key = ctx offset
	u32 nextLocal = 3;  // first available (0=ctx, 1=ram, 2=tmp)

	void addOffset(u32 offset) {
		if (entries.find(offset) == entries.end()) {
			RegCacheEntry e;
			e.wasmLocal = nextLocal++;
			e.dirty = false;
			entries[offset] = e;
		}
	}

	// Pre-scan: walk oplist, find all referenced integer registers
	void scanBlock(RuntimeBlockInfo* block) {
		for (size_t i = 0; i < block->oplist.size(); i++) {
			const shil_opcode& op = block->oplist[i];
			if (op.rs1.is_r32i()) addOffset(op.rs1.reg_offset());
			if (op.rs2.is_r32i()) addOffset(op.rs2.reg_offset());
			if (op.rs3.is_r32i()) addOffset(op.rs3.reg_offset());
			if (op.rd.is_r32i())  addOffset(op.rd.reg_offset());
			if (op.rd2.is_r32i()) addOffset(op.rd2.reg_offset());
			// shop_jdyn writes to JDYN (not a register param)
			if (op.op == shop_jdyn) addOffset(ctx_off::JDYN);
			// shop_jcond writes to jdyn (rd = reg_pc_dyn), not sr.T
			if (op.op == shop_jcond) addOffset(ctx_off::JDYN);
		}
		// Block exit may read sr.T or jdyn
		u32 bcls = BET_GET_CLS(block->BlockType);
		if (bcls == BET_CLS_COND) {
			// Delayed conditional (BT/S, BF/S) reads jdyn; immediate reads sr.T
			if (block->has_jcond) addOffset(ctx_off::JDYN);
			else addOffset(ctx_off::SR_T);
		}
		if (bcls == BET_CLS_Dynamic) addOffset(ctx_off::JDYN);
	}

	// Lookup: returns WASM local index or -1 if not cached
	s32 getLocal(u32 ctxOffset) const {
		auto it = entries.find(ctxOffset);
		if (it != entries.end()) return (s32)it->second.wasmLocal;
		return -1;
	}

	// Mark dirty (for stores)
	void markDirty(u32 ctxOffset) {
		auto it = entries.find(ctxOffset);
		if (it != entries.end()) it->second.dirty = true;
	}

	// Number of allocated cache locals
	u32 localCount() const { return nextLocal - 3; }
};

// ============================================================
// Helper: load a value from a shil_param onto the WASM stack
// ============================================================
static inline void emitLoadParam(WasmModuleBuilder& b, const shil_param& p) {
	if (p.is_imm()) {
		b.op_i32_const((s32)p._imm);
	} else if (p.is_r32i()) {
		b.op_local_get(LOCAL_CTX);
		b.op_i32_load(p.reg_offset());
	} else if (p.is_r32f()) {
		// Load float as i32 bits (reinterpret later if needed for f32 ops)
		b.op_local_get(LOCAL_CTX);
		b.op_i32_load(p.reg_offset());
	}
}

// Load a float param onto the WASM stack as f32
static inline void emitLoadParamF32(WasmModuleBuilder& b, const shil_param& p) {
	if (p.is_imm()) {
		// Immediate reinterpreted as float bits
		float val;
		u32 bits = p._imm;
		memcpy(&val, &bits, 4);
		b.op_f32_const(val);
	} else {
		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(p.reg_offset());
	}
}

// ============================================================
// Helper: store the top-of-stack value to a shil_param destination
// Stack must have: [ctx_ptr, value]
// ============================================================
// NOTE: Caller must push ctx_ptr BEFORE the value computation.
// Pattern: op_local_get(LOCAL_CTX), <compute value>, op_i32_store(offset)

// Store i32 value to rd (assumes value is already on stack)
// Caller must have already pushed ctx_ptr before value.
static inline void emitStoreRd(WasmModuleBuilder& b, const shil_param& rd) {
	b.op_i32_store(rd.reg_offset());
}

// Store f32 value to rd (assumes value is already on stack)
static inline void emitStoreRdF32(WasmModuleBuilder& b, const shil_param& rd) {
	b.op_f32_store(rd.reg_offset());
}

// ============================================================
// Cache-aware load: use local.get if cached, else memory load
// ============================================================
static inline void emitLoadParamCached(WasmModuleBuilder& b, const shil_param& p, const RegCache& cache) {
	if (p.is_imm()) {
		b.op_i32_const((s32)p._imm);
	} else if (p.is_r32i()) {
		s32 local = cache.getLocal(p.reg_offset());
		if (local >= 0) {
			b.op_local_get((u32)local);
		} else {
			b.op_local_get(LOCAL_CTX);
			b.op_i32_load(p.reg_offset());
		}
	} else if (p.is_r32f()) {
		// Float: not cached in V1, fall through to memory
		b.op_local_get(LOCAL_CTX);
		b.op_i32_load(p.reg_offset());
	}
}

// ============================================================
// Cache-aware store helpers for i32 destinations
// ============================================================
// emitPreStore: push ctx_ptr only if rd is NOT cached
// emitPostStore: local.set if cached, i32.store if not
// Must be paired: emitPreStore before value computation, emitPostStore after.

static inline void emitPreStore(WasmModuleBuilder& b, const shil_param& rd, const RegCache& cache) {
	if (rd.is_r32i()) {
		s32 local = cache.getLocal(rd.reg_offset());
		if (local >= 0) return;  // cached: no ctx_ptr needed
	}
	b.op_local_get(LOCAL_CTX);
}

static inline void emitPostStore(WasmModuleBuilder& b, const shil_param& rd, RegCache& cache) {
	if (rd.is_r32i()) {
		s32 local = cache.getLocal(rd.reg_offset());
		if (local >= 0) {
			b.op_local_set((u32)local);
			cache.markDirty(rd.reg_offset());
			return;
		}
	}
	b.op_i32_store(rd.reg_offset());
}

// Offset-based variants for fixed ctx fields (jdyn, sr.T)
static inline void emitPreStoreOffset(WasmModuleBuilder& b, u32 offset, const RegCache& cache) {
	if (cache.getLocal(offset) >= 0) return;
	b.op_local_get(LOCAL_CTX);
}

static inline void emitPostStoreOffset(WasmModuleBuilder& b, u32 offset, RegCache& cache) {
	s32 local = cache.getLocal(offset);
	if (local >= 0) {
		b.op_local_set((u32)local);
		cache.markDirty(offset);
	} else {
		b.op_i32_store(offset);
	}
}

// ============================================================
// Flush/reload all cached registers
// ============================================================
// Flush: write all dirty cached locals back to ctx memory
static inline void emitFlushAll(WasmModuleBuilder& b, RegCache& cache) {
	for (auto& [offset, entry] : cache.entries) {
		if (entry.dirty) {
			b.op_local_get(LOCAL_CTX);
			b.op_local_get(entry.wasmLocal);
			b.op_i32_store(offset);
			entry.dirty = false;
		}
	}
}

// Reload: load all cached registers from ctx memory (after C++ fallback call)
static inline void emitReloadAll(WasmModuleBuilder& b, RegCache& cache) {
	for (auto& [offset, entry] : cache.entries) {
		b.op_local_get(LOCAL_CTX);
		b.op_i32_load(offset);
		b.op_local_set(entry.wasmLocal);
		entry.dirty = false;
	}
}

// ============================================================
// Emit a complete SHIL op. Returns true if handled, false if
// fallback is needed.
// ============================================================
static bool emitShilOp(WasmModuleBuilder& b, const shil_opcode& op,
                        RuntimeBlockInfo* block, u32 opIndex, RegCache& cache) {
	switch (op.op) {

	// ---- Tier 1: Integer ALU ----

	case shop_mov32:
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_add:
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_add();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_sub:
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_sub();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_and:
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_and();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_or:
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_or();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_xor:
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_xor();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_not:
		// rd = ~rs1 = rs1 XOR -1
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		b.op_i32_const(-1);
		b.op_i32_xor();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_neg:
		// rd = 0 - rs1
		emitPreStore(b, op.rd, cache);
		b.op_i32_const(0);
		emitLoadParamCached(b, op.rs1, cache);
		b.op_i32_sub();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_shl:
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_shl();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_shr:
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_shr_u();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_sar:
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_shr_s();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_ror:
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_rotr();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_ext_s8:
		// Sign-extend 8→32: (val << 24) >> 24
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		b.op_i32_const(24);
		b.op_i32_shl();
		b.op_i32_const(24);
		b.op_i32_shr_s();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_ext_s16:
		// Sign-extend 16→32: (val << 16) >> 16
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		b.op_i32_const(16);
		b.op_i32_shl();
		b.op_i32_const(16);
		b.op_i32_shr_s();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_mul_u16:
		// rd = (u16)rs1 * (u16)rs2
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		b.op_i32_const(0xFFFF);
		b.op_i32_and();
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_const(0xFFFF);
		b.op_i32_and();
		b.op_i32_mul();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_mul_s16:
		// rd = (s16)rs1 * (s16)rs2
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		b.op_i32_const(16);
		b.op_i32_shl();
		b.op_i32_const(16);
		b.op_i32_shr_s();
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_const(16);
		b.op_i32_shl();
		b.op_i32_const(16);
		b.op_i32_shr_s();
		b.op_i32_mul();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_mul_i32:
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_mul();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_swaplb:
		// Swap low bytes: ((val >> 8) & 0xFF) | ((val & 0xFF) << 8) | (val & 0xFFFF0000)
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		b.op_local_tee(LOCAL_TMP);
		b.op_i32_const(8);
		b.op_i32_shr_u();
		b.op_i32_const(0xFF);
		b.op_i32_and();
		b.op_local_get(LOCAL_TMP);
		b.op_i32_const(0xFF);
		b.op_i32_and();
		b.op_i32_const(8);
		b.op_i32_shl();
		b.op_i32_or();
		b.op_local_get(LOCAL_TMP);
		b.op_i32_const((s32)0xFFFF0000u);
		b.op_i32_and();
		b.op_i32_or();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_xtrct:
		// rd = (rs1 >> 16) | (rs2 << 16)
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		b.op_i32_const(16);
		b.op_i32_shr_u();
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_const(16);
		b.op_i32_shl();
		b.op_i32_or();
		emitPostStore(b, op.rd, cache);
		return true;

	// ---- Comparisons ----

	case shop_test:
		// rd = (rs1 & rs2) == 0
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_and();
		b.op_i32_eqz();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_seteq:
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_eq();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_setge:
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_ge_s();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_setgt:
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_gt_s();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_setae:
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_ge_u();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_setab:
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_gt_u();
		emitPostStore(b, op.rd, cache);
		return true;

	// ---- Dynamic jump / conditional ----

	case shop_jdyn:
		// Store jump target to jdyn (cached or memory)
		emitPreStoreOffset(b, ctx_off::JDYN, cache);
		emitLoadParamCached(b, op.rs1, cache);
		if (!op.rs2.is_null()) {
			emitLoadParamCached(b, op.rs2, cache);
			b.op_i32_add();
		}
		emitPostStoreOffset(b, ctx_off::JDYN, cache);
		return true;

	case shop_jcond:
		// Save sr.T into jdyn for delayed conditional branches (BT/S, BF/S).
		// Condition evaluated BEFORE delay slot, branch AFTER.
		emitPreStoreOffset(b, ctx_off::JDYN, cache);
		emitLoadParamCached(b, op.rs1, cache);
		emitPostStoreOffset(b, ctx_off::JDYN, cache);
		return true;

	// ---- Memory operations ----

	case shop_readm: {
		// Compute address: rs1 + rs3 (if rs3 not null), store in LOCAL_TMP
		emitLoadParamCached(b, op.rs1, cache);
		if (!op.rs3.is_null()) {
			emitLoadParamCached(b, op.rs3, cache);
			b.op_i32_add();
		}
		b.op_local_set(LOCAL_TMP);

		if (op.size == 8) {
			// 64-bit read: two 32-bit reads (float pairs, not cached)
			b.op_local_get(LOCAL_TMP);
			b.op_call(WIMPORT_READ32);
			{
				u32 off = op.rd.reg_offset();
				b.op_local_set(LOCAL_TMP);
				b.op_local_get(LOCAL_CTX);
				b.op_local_get(LOCAL_TMP);
				b.op_i32_store(off);
			}
			// High word: recompute address
			emitLoadParamCached(b, op.rs1, cache);
			if (!op.rs3.is_null()) {
				emitLoadParamCached(b, op.rs3, cache);
				b.op_i32_add();
			}
			b.op_i32_const(4);
			b.op_i32_add();
			b.op_call(WIMPORT_READ32);
			{
				u32 off = op.rd.reg_offset() + 4;
				b.op_local_set(LOCAL_TMP);
				b.op_local_get(LOCAL_CTX);
				b.op_local_get(LOCAL_TMP);
				b.op_i32_store(off);
			}
		} else {
			// 1/2/4-byte read: direct RAM fast path for area 3
			emitPreStore(b, op.rd, cache);  // push ctx only if rd not cached

			b.op_local_get(LOCAL_TMP);
			b.op_i32_const(0x1FFFFFFF);
			b.op_i32_and();
			b.op_local_tee(LOCAL_TMP);

			b.op_i32_const(26);
			b.op_i32_shr_u();
			b.op_i32_const(3);
			b.op_i32_eq();

			b.op_if(WASM_TYPE_I32);
			{
				b.op_local_get(LOCAL_RAM);
				b.op_local_get(LOCAL_TMP);
				b.op_i32_const(0x00FFFFFF);
				b.op_i32_and();
				b.op_i32_add();
				switch (op.size) {
				case 1: b.op_i32_load8_s(0); break;
				case 2: b.op_i32_load16_s(0); break;
				default: b.op_i32_load(0); break;
				}
			}
			b.op_else();
			{
				// Slow path: recompute original addr, call import
				emitLoadParamCached(b, op.rs1, cache);
				if (!op.rs3.is_null()) {
					emitLoadParamCached(b, op.rs3, cache);
					b.op_i32_add();
				}
				u32 readFunc;
				switch (op.size) {
				case 1: readFunc = WIMPORT_READ8; break;
				case 2: readFunc = WIMPORT_READ16; break;
				default: readFunc = WIMPORT_READ32; break;
				}
				b.op_call(readFunc);
			}
			b.op_end();

			emitPostStore(b, op.rd, cache);
		}
		return true;
	}

	case shop_writem: {
		if (op.size == 8) {
			// 64-bit write: two 32-bit writes (float pairs)
			emitLoadParamCached(b, op.rs1, cache);
			if (!op.rs3.is_null()) {
				emitLoadParamCached(b, op.rs3, cache);
				b.op_i32_add();
			}
			b.op_local_get(LOCAL_CTX);
			b.op_i32_load(op.rs2.reg_offset());
			b.op_call(WIMPORT_WRITE32);

			emitLoadParamCached(b, op.rs1, cache);
			if (!op.rs3.is_null()) {
				emitLoadParamCached(b, op.rs3, cache);
				b.op_i32_add();
			}
			b.op_i32_const(4);
			b.op_i32_add();
			b.op_local_get(LOCAL_CTX);
			b.op_i32_load(op.rs2.reg_offset() + 4);
			b.op_call(WIMPORT_WRITE32);
		} else {
			// 1/2/4-byte write: direct RAM fast path for area 3
			emitLoadParamCached(b, op.rs1, cache);
			if (!op.rs3.is_null()) {
				emitLoadParamCached(b, op.rs3, cache);
				b.op_i32_add();
			}
			b.op_local_set(LOCAL_TMP);

			b.op_local_get(LOCAL_TMP);
			b.op_i32_const(0x1FFFFFFF);
			b.op_i32_and();
			b.op_local_tee(LOCAL_TMP);

			b.op_i32_const(26);
			b.op_i32_shr_u();
			b.op_i32_const(3);
			b.op_i32_eq();

			b.op_if();
			{
				b.op_local_get(LOCAL_RAM);
				b.op_local_get(LOCAL_TMP);
				b.op_i32_const(0x00FFFFFF);
				b.op_i32_and();
				b.op_i32_add();
				emitLoadParamCached(b, op.rs2, cache);
				switch (op.size) {
				case 1: b.op_i32_store8(0); break;
				case 2: b.op_i32_store16(0); break;
				default: b.op_i32_store(0); break;
				}
			}
			b.op_else();
			{
				emitLoadParamCached(b, op.rs1, cache);
				if (!op.rs3.is_null()) {
					emitLoadParamCached(b, op.rs3, cache);
					b.op_i32_add();
				}
				emitLoadParamCached(b, op.rs2, cache);
				u32 writeFunc;
				switch (op.size) {
				case 1: writeFunc = WIMPORT_WRITE8; break;
				case 2: writeFunc = WIMPORT_WRITE16; break;
				default: writeFunc = WIMPORT_WRITE32; break;
				}
				b.op_call(writeFunc);
			}
			b.op_end();
		}
		return true;
	}

	// ---- Interpreter fallback (single SH4 opcode) ----

	case shop_ifb:
		// Flush cache: ifb can modify arbitrary ctx state
		emitFlushAll(b, cache);
		if (op.rs1._imm) {
			b.op_local_get(LOCAL_CTX);
			b.op_i32_const((s32)op.rs2._imm);
			b.op_i32_store(ctx_off::PC);
		}
		b.op_i32_const((s32)op.rs3._imm);
		b.op_i32_const((s32)(block->vaddr + op.guest_offs - (op.delay_slot ? 2 : 0)));
		b.op_call(WIMPORT_IFB);
		emitReloadAll(b, cache);
		return true;

	// ---- Tier 2: FPU ops (float regs not cached in V1) ----

	case shop_fadd:
		b.op_local_get(LOCAL_CTX);
		emitLoadParamF32(b, op.rs1);
		emitLoadParamF32(b, op.rs2);
		b.op_f32_add();
		emitStoreRdF32(b, op.rd);
		return true;

	case shop_fsub:
		b.op_local_get(LOCAL_CTX);
		emitLoadParamF32(b, op.rs1);
		emitLoadParamF32(b, op.rs2);
		b.op_f32_sub();
		emitStoreRdF32(b, op.rd);
		return true;

	case shop_fmul:
		b.op_local_get(LOCAL_CTX);
		emitLoadParamF32(b, op.rs1);
		emitLoadParamF32(b, op.rs2);
		b.op_f32_mul();
		emitStoreRdF32(b, op.rd);
		return true;

	case shop_fdiv:
		b.op_local_get(LOCAL_CTX);
		emitLoadParamF32(b, op.rs1);
		emitLoadParamF32(b, op.rs2);
		b.op_f32_div();
		emitStoreRdF32(b, op.rd);
		return true;

	case shop_fabs:
		b.op_local_get(LOCAL_CTX);
		emitLoadParamF32(b, op.rs1);
		b.op_f32_abs();
		emitStoreRdF32(b, op.rd);
		return true;

	case shop_fneg:
		b.op_local_get(LOCAL_CTX);
		emitLoadParamF32(b, op.rs1);
		b.op_f32_neg();
		emitStoreRdF32(b, op.rd);
		return true;

	case shop_fsqrt:
		b.op_local_get(LOCAL_CTX);
		emitLoadParamF32(b, op.rs1);
		b.op_f32_sqrt();
		emitStoreRdF32(b, op.rd);
		return true;

	case shop_fseteq:
		// rd(i32) = (rs1 == rs2) ? 1 : 0
		emitPreStore(b, op.rd, cache);
		emitLoadParamF32(b, op.rs1);
		emitLoadParamF32(b, op.rs2);
		b.op_f32_eq();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_fsetgt:
		// rd(i32) = (rs1 > rs2) ? 1 : 0
		emitPreStore(b, op.rd, cache);
		emitLoadParamF32(b, op.rs1);
		emitLoadParamF32(b, op.rs2);
		b.op_f32_gt();
		emitPostStore(b, op.rd, cache);
		return true;

	case shop_cvt_i2f_n:
	case shop_cvt_i2f_z:
		// rd(f32) = (float)(s32)rs1 — i32 source cached, f32 dest not
		b.op_local_get(LOCAL_CTX);
		emitLoadParamCached(b, op.rs1, cache);
		b.op_f32_convert_i32_s();
		emitStoreRdF32(b, op.rd);
		return true;

	case shop_cvt_f2i_t:
		// rd(i32) = (s32)rs1(f32) — f32 source not cached, i32 dest cached
		emitPreStore(b, op.rd, cache);
		emitLoadParamF32(b, op.rs1);
		b.emitByte(0xFC);
		b.emitLEB128(0x00); // i32.trunc_sat_f32_s
		emitPostStore(b, op.rd, cache);
		return true;

	// ---- Tier 3: Vector/SIMD FPU ops (inline WASM, avoids shil_fb cross-module call) ----

	case shop_fmac:
		// rd = rs2 * rs3 + rs1  (fused multiply-add)
		b.op_local_get(LOCAL_CTX);
		emitLoadParamF32(b, op.rs1);        // fn (accumulator)
		emitLoadParamF32(b, op.rs2);        // f0
		emitLoadParamF32(b, op.rs3);        // fm
		b.op_f32_mul();                     // f0 * fm
		b.op_f32_add();                     // + fn
		emitStoreRdF32(b, op.rd);
		return true;

	case shop_fsrra:
		// rd = 1.0f / sqrt(rs1)
		b.op_local_get(LOCAL_CTX);
		b.op_f32_const(1.0f);
		emitLoadParamF32(b, op.rs1);
		b.op_f32_sqrt();
		b.op_f32_div();
		emitStoreRdF32(b, op.rd);
		return true;

	case shop_fipr: {
		// 4-element dot product: rd = sum(rs1[i] * rs2[i]) for i=0..3
		// Uses f32 accumulation (matches hardware precision)
		u32 off1 = op.rs1.reg_offset(), off2 = op.rs2.reg_offset();
		b.op_local_get(LOCAL_CTX);  // base for store
		// Element 0
		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(off1);
		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(off2);
		b.op_f32_mul();
		// Element 1
		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(off1 + 4);
		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(off2 + 4);
		b.op_f32_mul();
		b.op_f32_add();
		// Element 2
		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(off1 + 8);
		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(off2 + 8);
		b.op_f32_mul();
		b.op_f32_add();
		// Element 3
		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(off1 + 12);
		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(off2 + 12);
		b.op_f32_mul();
		b.op_f32_add();
		// Store result
		emitStoreRdF32(b, op.rd);
		return true;
	}

	case shop_ftrv: {
		// 4x4 matrix (rs2) * 4-vector (rs1) → rd
		// rd == rs1 always (in-place), so we must read all inputs before writing.
		// Strategy: compute all 4 results using a WASM local for temp storage.
		// Matrix layout: m[col][row] at moff + (col*4 + row)*4
		u32 voff = op.rs1.reg_offset();
		u32 moff = op.rs2.reg_offset();
		u32 doff = op.rd.reg_offset();

		// Use LOCAL_TMP as scratch for partial results
		// We need 3 temps: store results 0-2 on stack via locals, write result 3 last
		// Actually simpler: read all 4 vector elements first into locals
		// But we only have LOCAL_TMP. Instead: compute & store each row,
		// but since doff==voff (aliasing), we must save input first.

		// Save v[0..3] to LOCAL_TMP and stack by reading them upfront
		// We'll use a different approach: compute from last to first won't help
		// since matrix reads v[j] for all j in each row.
		// Best approach: use i32 reinterpret to save vector in temp locals.
		// We don't have extra locals... but we can store results to ctx at
		// a temporary offset. However, that's fragile.

		// Simplest correct approach: fall through to shil_fb for ftrv
		// since it already handles aliasing with temp copy.
		// The cross-module call cost is acceptable since ftrv is ~16 multiplies.
		return false;
	}

	case shop_frswap: {
		// Swap 16 floats (64 bytes) between rs1 and rd register banks
		u32 off1 = op.rs1.reg_offset(), off2 = op.rd.reg_offset();
		for (int i = 0; i < 16; i++) {
			// tmp = ctx[off1+i*4]
			b.op_local_get(LOCAL_CTX);
			b.op_i32_load(off1 + i * 4);
			b.op_local_set(LOCAL_TMP);
			// ctx[off1+i*4] = ctx[off2+i*4]
			b.op_local_get(LOCAL_CTX);
			b.op_local_get(LOCAL_CTX);
			b.op_i32_load(off2 + i * 4);
			b.op_i32_store(off1 + i * 4);
			// ctx[off2+i*4] = tmp
			b.op_local_get(LOCAL_CTX);
			b.op_local_get(LOCAL_TMP);
			b.op_i32_store(off2 + i * 4);
		}
		return true;
	}

#if 0  // BISECT: shld/shad disabled
	case shop_shld: {
		// Variable shift left/right (unsigned) depending on sign of rs2
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs2, cache);  // shift amount
		b.op_i32_const(0);
		b.op_i32_ge_s();                        // shift >= 0?
		b.op_if(WASM_TYPE_I32);
			// shift >= 0: val << (shift & 0x1F)
			emitLoadParamCached(b, op.rs1, cache);
			emitLoadParamCached(b, op.rs2, cache);
			b.op_i32_const(0x1F);
			b.op_i32_and();
			b.op_i32_shl();
		b.op_else();
			// shift < 0: check if (-shift & 0x1F) == 0
			emitLoadParamCached(b, op.rs2, cache);
			b.op_i32_const(0);
			b.op_i32_sub();  // negate
			b.op_i32_const(0x1F);
			b.op_i32_and();
			b.op_i32_eqz();
			b.op_if(WASM_TYPE_I32);
				b.op_i32_const(0);  // result is 0
			b.op_else();
				emitLoadParamCached(b, op.rs1, cache);
				emitLoadParamCached(b, op.rs2, cache);
				b.op_i32_const(0);
				b.op_i32_sub();  // negate
				b.op_i32_const(0x1F);
				b.op_i32_and();
				b.op_i32_shr_u();
			b.op_end();
		b.op_end();
		emitPostStore(b, op.rd, cache);
		return true;
	}

	case shop_shad: {
		// Variable arithmetic shift depending on sign of rs2
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_const(0);
		b.op_i32_ge_s();
		b.op_if(WASM_TYPE_I32);
			// shift >= 0: val << (shift & 0x1F)
			emitLoadParamCached(b, op.rs1, cache);
			emitLoadParamCached(b, op.rs2, cache);
			b.op_i32_const(0x1F);
			b.op_i32_and();
			b.op_i32_shl();
		b.op_else();
			emitLoadParamCached(b, op.rs2, cache);
			b.op_i32_const(0);
			b.op_i32_sub();
			b.op_i32_const(0x1F);
			b.op_i32_and();
			b.op_i32_eqz();
			b.op_if(WASM_TYPE_I32);
				// (-shift & 0x1F) == 0: val >> 31
				emitLoadParamCached(b, op.rs1, cache);
				b.op_i32_const(31);
				b.op_i32_shr_s();
			b.op_else();
				emitLoadParamCached(b, op.rs1, cache);
				emitLoadParamCached(b, op.rs2, cache);
				b.op_i32_const(0);
				b.op_i32_sub();
				b.op_i32_const(0x1F);
				b.op_i32_and();
				b.op_i32_shr_s();  // arithmetic shift
			b.op_end();
		b.op_end();
		emitPostStore(b, op.rd, cache);
		return true;
	}
#endif  // BISECT shld/shad

	case shop_mov64:
		// Copy 64 bits (float register pairs, not cached)
		if (op.rs1.is_reg() && op.rd.is_reg()) {
			u32 srcOff = op.rs1.reg_offset();
			u32 dstOff = op.rd.reg_offset();
			b.op_local_get(LOCAL_CTX);
			b.op_local_get(LOCAL_CTX);
			b.op_i32_load(srcOff);
			b.op_i32_store(dstOff);
			b.op_local_get(LOCAL_CTX);
			b.op_local_get(LOCAL_CTX);
			b.op_i32_load(srcOff + 4);
			b.op_i32_store(dstOff + 4);
			return true;
		}
		return false;

	// ---- System ops that need fallback (flush+reload around call) ----
	case shop_sync_sr:
	case shop_sync_fpscr:
	case shop_pref:
		emitFlushAll(b, cache);
		b.op_i32_const((s32)block->vaddr);
		b.op_i32_const((s32)opIndex);
		b.op_call(WIMPORT_SHIL_FB);
		emitReloadAll(b, cache);
		return true;

	default:
		return false;
	}
}

// ============================================================
// Emit block exit code based on BlockEndType
// ============================================================
static void emitBlockExit(WasmModuleBuilder& b, RuntimeBlockInfo* block, const RegCache& cache) {
	u32 bcls = BET_GET_CLS(block->BlockType);

	switch (bcls) {
	case BET_CLS_Static:
		if (block->BlockType == BET_StaticIntr) {
			b.op_local_get(LOCAL_CTX);
			b.op_i32_const((s32)block->NextBlock);
			b.op_i32_store(ctx_off::PC);
		} else {
			b.op_local_get(LOCAL_CTX);
			b.op_i32_const((s32)block->BranchBlock);
			b.op_i32_store(ctx_off::PC);
		}
		break;

	case BET_CLS_Dynamic: {
		// ctx.pc = jdyn — read from cached local if available
		b.op_local_get(LOCAL_CTX);
		s32 jdynLocal = cache.getLocal(ctx_off::JDYN);
		if (jdynLocal >= 0) {
			b.op_local_get((u32)jdynLocal);
		} else {
			b.op_local_get(LOCAL_CTX);
			b.op_i32_load(ctx_off::JDYN);
		}
		b.op_i32_store(ctx_off::PC);
		break;
	}

	case BET_CLS_COND: {
		// if (cond_val == cond) pc = BranchBlock else pc = NextBlock
		// For delayed conditional (BT/S, BF/S): cond_val = jdyn (saved before delay slot)
		// For immediate conditional (BT, BF): cond_val = sr.T
		u32 cond = (block->BlockType == BET_Cond_1) ? 1 : 0;

		b.op_local_get(LOCAL_CTX);  // base for store

		if (block->has_jcond) {
			// Read jdyn (condition saved before delay slot)
			s32 jdynLocal = cache.getLocal(ctx_off::JDYN);
			if (jdynLocal >= 0) {
				b.op_local_get((u32)jdynLocal);
			} else {
				b.op_local_get(LOCAL_CTX);
				b.op_i32_load(ctx_off::JDYN);
			}
		} else {
			// Read sr.T directly
			s32 srTLocal = cache.getLocal(ctx_off::SR_T);
			if (srTLocal >= 0) {
				b.op_local_get((u32)srTLocal);
			} else {
				b.op_local_get(LOCAL_CTX);
				b.op_i32_load(ctx_off::SR_T);
			}
		}
		if (cond == 1) {
			b.op_if(WASM_TYPE_I32);
		} else {
			b.op_i32_eqz();
			b.op_if(WASM_TYPE_I32);
		}
		b.op_i32_const((s32)block->BranchBlock);
		b.op_else();
		b.op_i32_const((s32)block->NextBlock);
		b.op_end();

		b.op_i32_store(ctx_off::PC);
		break;
	}
	}
}
