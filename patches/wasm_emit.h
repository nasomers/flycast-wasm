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

// Differential-validator memory-write logging flag (see rec_wasm.cpp). Default
// 0 (production: keep the direct-RAM write fast path). rec_wasm.cpp defines it
// before including this header for validation builds.
#ifndef WASM_VAL_LOG_WRITES
#define WASM_VAL_LOG_WRITES 0
#endif

// ★ EMITTED GEN BUMP — ADOPTED (2026-07-28, the design decision):
// inline area-3 stores emit the g_fly_page_gen bump the C import path does,
// closing the February bypass (the tracked bug) exactly. SH4 alignment
// rules mean a 1/2/4-byte store never straddles a 4KB page, so ONE cell bump
// is exact (the C path's second-page handling is for block/DMA ranges).
// Measured cost (matched A/B, b10f496): ~0.2ms gross — accepted; repaid by
// insurance-tick retirement + size-8 write inlining later in the campaign.
// The tick may only be removed AFTER the write-parity farm certifies this
// path exact. 0 = legacy bypass (kept for A/B archaeology only).
#define FLY_EMIT_GEN_BUMP 1

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
	WIMPORT_SQ_PREF = 8,    // (addr) -> void — thin SQ flush, no fb machinery
	WIMPORT_DIV32U  = 9,    // (r1,r2,r3) -> i64 (rem<<32 | quo)
	WIMPORT_DIV32S  = 10,   // (r1,r2,r3) -> i64 (rem<<32 | quo)
	WIMPORT_DIV1    = 11,   // (a,b,T) -> i64 (T<<32 | a); writes sr.Q/M C-side
	WIMPORT_COUNT   = 12
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
// Locals 2-6 = scratch i32 (for intermediate values)
// Local 7+ = register cache i32s
// After all i32s = 1 i64 scratch (for dual-output ops like adc/mul_u64)
constexpr u32 LOCAL_CTX  = 0;
constexpr u32 LOCAL_RAM  = 1;
constexpr u32 LOCAL_TMP  = 2;
constexpr u32 LOCAL_TMP2 = 3;  // scratch for ftrv vector save
constexpr u32 LOCAL_TMP3 = 4;
constexpr u32 LOCAL_TMP4 = 5;
constexpr u32 LOCAL_TMP5 = 6;
constexpr u32 LOCAL_FIXED_I32_COUNT = 5;  // TMP through TMP5

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
	u32 nextLocal = 2 + LOCAL_FIXED_I32_COUNT;  // first available after params + fixed scratch

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
	u32 localCount() const { return nextLocal - (2 + LOCAL_FIXED_I32_COUNT); }

	// i64 scratch local index (set by compile() after all i32 locals are allocated)
	u32 _tmp64LocalIdx = 0;
	u32 tmp64Local() const { return _tmp64LocalIdx; }
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
// FLY_FORCE_FALLBACK_MASK — compile-time bitmask for differential debugging.
// Each bit forces a whole category of SHIL ops to fall back to the shil_fb
// import path instead of being natively emitted. Used to binary-search
// which native-emit category contains a correctness bug on games that run
// correctly under the SHIL interpreter (EXECUTOR_MODE 5) but fail under
// the WASM JIT (EXECUTOR_MODE 6).
//
//   bit 0 = INT_ALU     (mov, add, sub, and/or/xor/not, neg, shift, ext)
//   bit 1 = INT_MUL_DIV (mul_*, div_*, setpeq)
//   bit 2 = INT_CARRY   (adc, sbc, negc, rocl, rocr, shld, shad)
//   bit 3 = COMPARE     (test, seteq, setge, setgt, setae, setab)
//   bit 4 = FPU         (fadd/sub/mul/div/abs/neg/sqrt, fset*, fmac,
//                        fsrra, fipr, ftrv, frswap, fsca)
//   bit 5 = CONVERT     (cvt_f2i_t, cvt_i2f_n, cvt_i2f_z)
//   bit 6 = READM       (shop_readm only)
//   bit 7 = CONTROL     (jdyn, jcond)
//   bit 8 = SYSTEM      (sync_sr, sync_fpscr, pref, ifb, illegal, swaplb, xtrct)
//   bit 9 = WRITEM      (shop_writem only) — split out from bit 6 to
//                        bisect the MEMORY category which contains the
//                        bug breaking Sonic
//
// 0     = normal production (all categories emit natively)
// 0x1FF = every category falls back to shil_fb (block structure still
//         uses native register cache + dispatch, so a failure under 0x1FF
//         localizes the bug to the block scaffolding, not per-op emission)
#ifndef FLY_FORCE_FALLBACK_MASK
// TEMP DIAGNOSTIC: binary searching which category contains the bug that
// breaks Sonic (and presumably other FMV-cluster games) under WASM JIT.
// Step 1 (confirmed): 0x1FF (all fallback) → Sonic renders. Bug is in
// per-op native emit, not in block scaffolding.
// Step 2 confirmed: 0x00F (integer fallback) → Sonic breaks. Bug is in
//                   at least one of FPU/CONVERT/MEMORY/CONTROL/SYSTEM.
// Step 3 confirmed: 0x1F0 → Sonic breaks. There's a bug in bits 0-3 too
//                   (not only in bits 4-8).
// Step 4 confirmed: 0x1FC → RENDERING. Integer bug is in bit 2 (CARRY)
//                   or bit 3 (COMPARE), NOT in ALU or MUL/DIV.
// Step 5 confirmed: 0x1FB → RENDERING. CARRY is clean.
// Step 6 confirmed: 0x1F7 → RENDERING. COMPARE alone is also fine.
// Surprise: all four integer sub-categories work INDIVIDUALLY native,
// but 0x1F0 (all four together) breaks. It's a multi-category
// interaction — likely a register-cache / T-flag-visibility bug that
// only manifests when two specific categories emit natively in the
// same block and share a register.
// Step 7 confirmed: 0x1F6 → RENDERING. ALU+COMPARE pair not the culprit.
// Step 8 confirmed: 0x1F8 → RENDERING. ALU+MUL+CARRY together fine.
// Bug requires COMPARE native. Known pairs so far: ALU+COMPARE works.
// Step 9 confirmed: 0x1F5 → RENDERING. MUL+COMPARE fine too.
// Step 10 confirmed: 0x1F3 → RENDERING. All three COMPARE-pair combos
//                    work. Bug needs COMPARE native + at least two more
//                    integer categories.
// Step 11 confirmed: 0x1F4 → RENDERING. ALU+MUL+COMPARE triple fine.
// Step 12 confirmed: 0x1F2 → RENDERING. ALU+CARRY+COMPARE triple fine.
// Step 13 confirmed: 0x1F1 → RENDERING. All three triples work.
// Step 14 re-verify confirmed: 0x1F0 → RENDERING. Earlier BLANK result
//                    was flaky — the bug is NOT in the integer half at
//                    all. Every integer mask is fine.
// Step 15 confirmed: 0x00F → BLANK. Bug is in bits 4-8 half (FPU,
//                    CONVERT, MEMORY, CONTROL, SYSTEM). Integer half
//                    is 100% clean.
// Step 16 confirmed: 0x1CF → RENDERING. Bug NOT in FPU or CONVERT.
// Step 17 confirmed: 0x03F → BLANK. Bug localized to bits 6,7,8
//                    (MEMORY, CONTROL, SYSTEM).
// Step 18 confirmed: 0x0FF → RENDERING. SYSTEM clean.
// Step 19 confirmed: 0x1BF (MEMORY native, was both readm+writem) → BLANK.
// Step 20 (re-run)   (split bit 6 = readm, bit 9 = writem).
// Step 21 confirmed: 0x3BF (readm native) → BLANK. readm is the bug.
// Step 22 confirmed: 0x1FF (writem native) → RENDERING. Clean.
// Step 23 confirmed: bug is SPECIFICALLY in shop_readm native emission.
// For mode 7: force writem (bit 9) to go through shil_fb so its
// writes can be captured via g_shil_dry_run. All other ops emit
// natively so we test the real JIT path. If EXECUTOR_MODE != 7,
// mask = 0 (production).
//
// Session findings summary (see project memory):
// - Shadow comparison mode 5 + FORCE_CPP_DISPATCH found 0 SHIL-ref
//   divergences, so the SHIL interpreter layer is correct.
// - Bug is in the WASM JIT native emit path (wasm_emit.h), NOT in
//   SHIL lowering or the fallback interpreter.
// - Binary search showed readm native emission has SOMETHING wrong
//   (0x3BF "readm native only" → BLANK) but patching readm to always
//   take the slow path did NOT fix Sonic — meaning there are
//   additional native-emit bugs interacting. The combinatorial search
//   hit non-determinism on some masks which made single-run results
//   unreliable.
// - Next move for a future session: build a true WASM-JIT-vs-ref
//   shadow mode (currently only SHIL-vs-ref exists) so we can see the
//   first block where the NATIVE JIT output diverges from the reference
//   interpreter at every SHIL op. That gives block-level divergence
//   coordinates instead of combinatorial category masks.
// EXECUTOR_MODE is defined AFTER this header is included (in rec_wasm.cpp
// line ~840) so we can't check it here. Set the mask directly:
// 0x200 = writem fallback for mode 7 write-capture diagnostic.
// Set to 0 for production.
// DIAGNOSTIC 2026-04-18: 0x1FF confirmed bug is NOT in native op emission.
// Back to 0 (native ops) for next test: multi-block chain disabled.
#define FLY_FORCE_FALLBACK_MASK 0
#endif

#if FLY_FORCE_FALLBACK_MASK != 0
static u32 flyOpCategoryBit(u32 op) {
	switch (op) {
	case shop_mov32: case shop_mov64:
	case shop_add: case shop_sub:
	case shop_and: case shop_or: case shop_xor:
	case shop_not: case shop_neg:
	case shop_shl: case shop_shr: case shop_sar: case shop_ror:
	case shop_ext_s8: case shop_ext_s16:
		return 1u << 0;
	case shop_mul_u16: case shop_mul_s16: case shop_mul_i32:
	case shop_mul_u64: case shop_mul_s64:
	case shop_div1: case shop_div32u: case shop_div32s: case shop_div32p2:
	case shop_setpeq:
		return 1u << 1;
	case shop_adc: case shop_sbc: case shop_negc:
	case shop_rocl: case shop_rocr:
	case shop_shld: case shop_shad:
		return 1u << 2;
	case shop_test:
	case shop_seteq: case shop_setge: case shop_setgt:
	case shop_setae: case shop_setab:
		return 1u << 3;
	case shop_fadd: case shop_fsub: case shop_fmul: case shop_fdiv:
	case shop_fabs: case shop_fneg: case shop_fsqrt:
	case shop_fseteq: case shop_fsetgt:
	case shop_fmac: case shop_fsrra: case shop_fipr: case shop_ftrv:
	case shop_frswap: case shop_fsca:
		return 1u << 4;
	case shop_cvt_f2i_t: case shop_cvt_i2f_n: case shop_cvt_i2f_z:
		return 1u << 5;
	case shop_readm:
		return 1u << 6;
	case shop_writem:
		return 1u << 9;
	case shop_jdyn: case shop_jcond:
		return 1u << 7;
	case shop_sync_sr: case shop_sync_fpscr: case shop_pref:
	case shop_ifb: case shop_illegal: case shop_swaplb: case shop_xtrct:
		return 1u << 8;
	default:
		return 0;  // unknown / not classified — never force fallback
	}
}
#endif

// Set by buildMultiBlockModule around chain emission: ops whose inline
// emission relies on single-block cache invariants must take the standard
// fallback path inside chains. (Since the thin-import rework, no op needs
// this — kept for future single-block-only optimizations.)
static bool g_emitting_chain = false;

// Baked base address of the SH4 fsca sin/cos table (defined in rec_wasm.cpp;
// table lives in sh4_rom.cpp: f32_x2 sin_table[0x10000]).
extern "C" u32 fly_sin_table_addr();

static bool emitShilOp(WasmModuleBuilder& b, const shil_opcode& op,
                        RuntimeBlockInfo* block, u32 opIndex, RegCache& cache) {
#if FLY_FORCE_FALLBACK_MASK != 0
	// Diagnostic: force this op's whole category to fall back to shil_fb
	// so the block uses the interpreter path for this op instead of native
	// WASM emission. Categories are the coarsest possible split — the
	// intent is binary search, not fine-grained control.
	if ((FLY_FORCE_FALLBACK_MASK) & flyOpCategoryBit((u32)op.op))
		return false;
#endif
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
#ifndef JIT_PROD_BUILD
			// Dev counter (fastmem Phase 0): size-8 readm executions. The
			// MEM-DENSITY import counters can't attribute calls to op size;
			// 2*g_fly_mr8 ~= r_pf confirms float-pair reads ARE the wall.
			{
				extern u32 g_fly_mr8;
				u32 c8 = (u32)(uintptr_t)&g_fly_mr8;
				b.op_i32_const((s32)c8);
				b.op_i32_const((s32)c8);
				b.op_i32_load(0);
				b.op_i32_const(1);
				b.op_i32_add();
				b.op_i32_store(0);
			}
#endif
			// 64-bit read (float pairs, not cached): ★ FASTMEM (2026-07-23).
			// Phase 0 measured 99% of import read calls in fight content as
			// this path (r8_share_pct=99, 200-278K calls/frame ≈ 6-11ms).
			// Direct linear-memory fast path when BOTH words are area 3;
			// import fallback otherwise. TMP keeps the ORIGINAL address for
			// the fallback (MMIO must see the unmasked address); phys lives
			// in TMP2.
			b.op_local_get(LOCAL_TMP);
			b.op_i32_const(0x1FFFFFFF);
			b.op_i32_and();
			b.op_local_tee(LOCAL_TMP2);

			// ((phys | (phys+4)) >> 26) == 3 — equals 3 iff BOTH words are
			// area 3 (a carry out of bits 25:0 in phys+4 drives the OR's
			// bits 28:26 away from 0b011). Covers the straddling read at the
			// top of the 64MB window exactly.
			b.op_local_get(LOCAL_TMP2);
			b.op_i32_const(4);
			b.op_i32_add();
			b.op_i32_or();
			b.op_i32_const(26);
			b.op_i32_shr_u();
			b.op_i32_const(3);
			b.op_i32_eq();

			// ★ BRANCH HINT: RAM arm hot (measured 99% of size-8 reads).
			b.hintNextBranchLikely();
			b.op_if();
			{
				// ctx[rd] = ram[phys & RAM_MASK]
				b.op_local_get(LOCAL_CTX);
				b.op_local_get(LOCAL_RAM);
				b.op_local_get(LOCAL_TMP2);
				b.op_i32_const(0x00FFFFFF);
				b.op_i32_and();
				b.op_i32_add();
				b.op_i32_load(0);
				b.op_i32_store(op.rd.reg_offset());
				// ctx[rd+4] = ram[(phys+4) & RAM_MASK]
				b.op_local_get(LOCAL_CTX);
				b.op_local_get(LOCAL_RAM);
				b.op_local_get(LOCAL_TMP2);
				b.op_i32_const(4);
				b.op_i32_add();
				b.op_i32_const(0x00FFFFFF);
				b.op_i32_and();
				b.op_i32_add();
				b.op_i32_load(0);
				b.op_i32_store(op.rd.reg_offset() + 4);
			}
			b.op_else();
			{
				// Slow path: two 32-bit import reads (original address in TMP)
				b.op_local_get(LOCAL_TMP);
				b.op_call(WIMPORT_READ32);
				{
					u32 off = op.rd.reg_offset();
					b.op_local_set(LOCAL_TMP2);
					b.op_local_get(LOCAL_CTX);
					b.op_local_get(LOCAL_TMP2);
					b.op_i32_store(off);
				}
				// High word: original address + 4 (TMP intact — no recompute)
				b.op_local_get(LOCAL_TMP);
				b.op_i32_const(4);
				b.op_i32_add();
				b.op_call(WIMPORT_READ32);
				{
					u32 off = op.rd.reg_offset() + 4;
					b.op_local_set(LOCAL_TMP2);
					b.op_local_get(LOCAL_CTX);
					b.op_local_get(LOCAL_TMP2);
					b.op_i32_store(off);
				}
			}
			b.op_end();
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

			// ★ BRANCH HINT: RAM arm hot (imports ~68/frame vs ~118K inline).
			b.hintNextBranchLikely();
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
#ifndef JIT_PROD_BUILD
			// Dev counter (fastmem Phase 0): size-8 writem executions.
			{
				extern u32 g_fly_mw8;
				u32 c8 = (u32)(uintptr_t)&g_fly_mw8;
				b.op_i32_const((s32)c8);
				b.op_i32_const((s32)c8);
				b.op_i32_load(0);
				b.op_i32_const(1);
				b.op_i32_add();
				b.op_i32_store(0);
			}
#endif
#if WASM_VAL_LOG_WRITES
			// Validator/shadow builds: every write through the import so the
			// differential can log + undo it.
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
#else
			// ★ SQ-WRITE FASTMEM (2026-07-23): Phase 0 measured 90-95% of
			// write-import calls landing in the SQ region (0xE0-0xE3) —
			// the C handler is a plain mapBlock onto ctx.sq_buffer (mask
			// 63), zero side effects until pref (thin sq_pref import reads
			// the same ctx bytes — memory-coherent). Inline the store.
			// 64-bit write (float pairs): addr once into TMP.
			emitLoadParamCached(b, op.rs1, cache);
			if (!op.rs3.is_null()) {
				emitLoadParamCached(b, op.rs3, cache);
				b.op_i32_add();
			}
			b.op_local_set(LOCAL_TMP);

			// (addr >> 26) == 0x38 — the exact SQ decode the pref
			// canonical uses (0xE0000000-0xE3FFFFFF, unmasked address).
			b.op_local_get(LOCAL_TMP);
			b.op_i32_const(26);
			b.op_i32_shr_u();
			b.op_i32_const(0x38);
			b.op_i32_eq();

			b.op_if();
			{
				// ctx[addr & 0x3F] = ctx[rs2]; each word masked
				// independently (matches the C per-access mask — a
				// straddling write at 0x3C wraps to sq[0] both ways).
				b.op_local_get(LOCAL_CTX);
				b.op_local_get(LOCAL_TMP);
				b.op_i32_const(0x3F);
				b.op_i32_and();
				b.op_i32_add();
				b.op_local_get(LOCAL_CTX);
				b.op_i32_load(op.rs2.reg_offset());
				b.op_i32_store(0);

				b.op_local_get(LOCAL_CTX);
				b.op_local_get(LOCAL_TMP);
				b.op_i32_const(4);
				b.op_i32_add();
				b.op_i32_const(0x3F);
				b.op_i32_and();
				b.op_i32_add();
				b.op_local_get(LOCAL_CTX);
				b.op_i32_load(op.rs2.reg_offset() + 4);
				b.op_i32_store(0);
			}
			b.op_else();
			{
				// ★ SIZE-8 RAM-WRITE FASTMEM (2026-08-01, page-gen option 1
				// step 4, the design decision): the last fastmem
				// remainder (~0.3-1.5ms/frame of import calls). Same
				// both-words area-3 check as the size-8 read path; TMP keeps
				// the ORIGINAL address for the MMIO/TA-FIFO fallback, phys
				// lives in TMP2. Page-gen contract: SH4 requires 8-byte
				// alignment for 64-bit FMOV, so the pair never straddles a
				// 4KB page — ONE emitted gen bump is exact (mirrors the C
				// import path's fly_ram_written(addr,8)). NO branch hint:
				// fight content is TA-FIFO-dominated (import arm hot),
				// menu/world content is RAM-dominated — per-site
				// distribution is content-dependent, so hinting would guess.
				b.op_local_get(LOCAL_TMP);
				b.op_i32_const(0x1FFFFFFF);
				b.op_i32_and();
				b.op_local_tee(LOCAL_TMP2);
				b.op_local_get(LOCAL_TMP2);
				b.op_i32_const(4);
				b.op_i32_add();
				b.op_i32_or();
				b.op_i32_const(26);
				b.op_i32_shr_u();
				b.op_i32_const(3);
				b.op_i32_eq();

				b.op_if();
				{
					// ram[phys & RAM_MASK] = ctx[rs2]
					b.op_local_get(LOCAL_RAM);
					b.op_local_get(LOCAL_TMP2);
					b.op_i32_const(0x00FFFFFF);
					b.op_i32_and();
					b.op_local_tee(LOCAL_TMP2);   // TMP2 = ram offset now
					b.op_i32_add();
					b.op_local_get(LOCAL_CTX);
					b.op_i32_load(op.rs2.reg_offset());
					b.op_i32_store(0);
					// ram[offset + 4] = ctx[rs2 + 4]
					b.op_local_get(LOCAL_RAM);
					b.op_local_get(LOCAL_TMP2);
					b.op_i32_add();
					b.op_local_get(LOCAL_CTX);
					b.op_i32_load(op.rs2.reg_offset() + 4);
					b.op_i32_store(4);
#if FLY_EMIT_GEN_BUMP
					// g_fly_page_gen[offset >> 12]++ (one cell — aligned
					// pair cannot straddle; see comment above)
					{
						extern u32 g_fly_page_gen[];
						u32 genBase = (u32)(uintptr_t)&g_fly_page_gen[0];
						b.op_local_get(LOCAL_TMP2);
						b.op_i32_const(0x00FFF000);
						b.op_i32_and();
						b.op_i32_const(10);
						b.op_i32_shr_u();
						b.op_i32_const((s32)genBase);
						b.op_i32_add();
						b.op_local_tee(LOCAL_TMP2);
						b.op_local_get(LOCAL_TMP2);
						b.op_i32_load(0);
						b.op_i32_const(1);
						b.op_i32_add();
						b.op_i32_store(0);
					}
#endif
				}
				b.op_else();
				{
					// Import fallback (TMP intact — no rs1+rs3 recompute)
					b.op_local_get(LOCAL_TMP);
					b.op_local_get(LOCAL_CTX);
					b.op_i32_load(op.rs2.reg_offset());
					b.op_call(WIMPORT_WRITE32);

					b.op_local_get(LOCAL_TMP);
					b.op_i32_const(4);
					b.op_i32_add();
					b.op_local_get(LOCAL_CTX);
					b.op_i32_load(op.rs2.reg_offset() + 4);
					b.op_call(WIMPORT_WRITE32);
				}
				b.op_end();
			}
			b.op_end();
#endif
		} else {
#if WASM_VAL_LOG_WRITES
			// Validator build: route EVERY write through the import so the
			// differential validator can log + compare it (no direct-RAM fast
			// path, which would bypass logging).
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
#else
			// 1/2/4-byte write: SQ fast path, then direct RAM fast path
			emitLoadParamCached(b, op.rs1, cache);
			if (!op.rs3.is_null()) {
				emitLoadParamCached(b, op.rs3, cache);
				b.op_i32_add();
			}
			b.op_local_set(LOCAL_TMP);

			// ★ SQ-WRITE FASTMEM: (addr >> 26) == 0x38 → plain store into
			// ctx.sq_buffer[addr & 0x3F] (see size-8 arm for rationale).
			b.op_local_get(LOCAL_TMP);
			b.op_i32_const(26);
			b.op_i32_shr_u();
			b.op_i32_const(0x38);
			b.op_i32_eq();

			// ★ BRANCH HINT: ordinary stores dominate; SQ hits come from
			// dedicated flush loops only — SQ arm cold at the typical site.
			b.hintNextBranchUnlikely();
			b.op_if();
			{
				b.op_local_get(LOCAL_CTX);
				b.op_local_get(LOCAL_TMP);
				b.op_i32_const(0x3F);
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
			b.op_local_get(LOCAL_TMP);
			b.op_i32_const(0x1FFFFFFF);
			b.op_i32_and();
			b.op_local_tee(LOCAL_TMP);

			b.op_i32_const(26);
			b.op_i32_shr_u();
			b.op_i32_const(3);
			b.op_i32_eq();

			// ★ BRANCH HINT: RAM arm hot (import fallback = MMIO residue only).
			b.hintNextBranchLikely();
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
#if FLY_EMIT_GEN_BUMP
				// g_fly_page_gen[(phys & RAM_MASK) >> 12]++ — cell byte
				// offset collapses to (phys & 0xFFF000) >> 10. TMP still
				// holds phys; TMP2 is free in this arm.
				{
					extern u32 g_fly_page_gen[];
					u32 genBase = (u32)(uintptr_t)&g_fly_page_gen[0];
					b.op_local_get(LOCAL_TMP);
					b.op_i32_const(0x00FFF000);
					b.op_i32_and();
					b.op_i32_const(10);
					b.op_i32_shr_u();
					b.op_i32_const((s32)genBase);
					b.op_i32_add();
					b.op_local_tee(LOCAL_TMP2);
					b.op_local_get(LOCAL_TMP2);
					b.op_i32_load(0);
					b.op_i32_const(1);
					b.op_i32_add();
					b.op_i32_store(0);
				}
#endif
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
			b.op_end();   // close SQ-fastmem if/else
#endif
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
		// rd(i32) = (s32)rs1(f32)
		// NaN handling: WASM's i32.trunc_sat_f32_s returns 0 for NaN, but the
		// SH4 spec and the reference interpreter return 0x80000000. We check
		// for NaN up front (f32.eq(x,x) == 0 iff x is NaN) and emit the
		// spec-correct constant in that branch.
		emitPreStore(b, op.rd, cache);
		emitLoadParamF32(b, op.rs1);
		emitLoadParamF32(b, op.rs1);
		b.op_f32_eq();           // 1 if x == x (not NaN), 0 if NaN
		b.op_if(WASM_TYPE_I32);  // if (result i32)
			emitLoadParamF32(b, op.rs1);
			b.emitByte(0xFC);
			b.emitLEB128(0x00);  // i32.trunc_sat_f32_s
		b.op_else();
			b.op_i32_const((s32)0x80000000);
		b.op_end();
		emitPostStore(b, op.rd, cache);
		return true;

	// ---- Tier 3: Vector/SIMD FPU ops (inline WASM, avoids shil_fb cross-module call) ----

	case shop_fmac:
		// rd = rs2 * rs3 + rs1. SH4 FMAC is FUSED (canonical: std::fma —
		// single rounding). WASM has no scalar f32 FMA, so compute in f64:
		// the f32×f32 product is EXACT in f64 (24+24 ≤ 53 bits), the f64 add
		// rounds once, the demote rounds once — equal to true fused except
		// in astronomically rare f64→f32 double-rounding ties. The previous
		// f32.mul+f32.add double-rounded EVERY op (1-ULP drift vs reference,
		// caught by the bridge-shadow differential 2026-07-17). The SHIL
		// fallback computes the same f64 expression — bit-identical paths.
		b.op_local_get(LOCAL_CTX);
		emitLoadParamF32(b, op.rs1);        // fn (accumulator)
		b.op_f64_promote_f32();
		emitLoadParamF32(b, op.rs2);        // f0
		b.op_f64_promote_f32();
		emitLoadParamF32(b, op.rs3);        // fm
		b.op_f64_promote_f32();
		b.op_f64_mul();                     // (f64)f0 * (f64)fm  — exact
		b.op_f64_add();                     // + (f64)fn — one rounding
		b.op_f32_demote_f64();              // → f32 — one rounding
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
		// Uses f64 accumulation to match reference interpreter (sh4_fpu.cpp)
		// and shil_canonical.h — prevents 3D geometry drift from f32 rounding.
		u32 off1 = op.rs1.reg_offset(), off2 = op.rs2.reg_offset();
		b.op_local_get(LOCAL_CTX);  // base for store
		// Element 0: (f64)rs1[0] * (f64)rs2[0]
		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(off1);
		b.op_f64_promote_f32();
		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(off2);
		b.op_f64_promote_f32();
		b.op_f64_mul();
		// Element 1: + (f64)rs1[1] * (f64)rs2[1]
		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(off1 + 4);
		b.op_f64_promote_f32();
		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(off2 + 4);
		b.op_f64_promote_f32();
		b.op_f64_mul();
		b.op_f64_add();
		// Element 2: + (f64)rs1[2] * (f64)rs2[2]
		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(off1 + 8);
		b.op_f64_promote_f32();
		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(off2 + 8);
		b.op_f64_promote_f32();
		b.op_f64_mul();
		b.op_f64_add();
		// Element 3: + (f64)rs1[3] * (f64)rs2[3]
		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(off1 + 12);
		b.op_f64_promote_f32();
		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(off2 + 12);
		b.op_f64_promote_f32();
		b.op_f64_mul();
		b.op_f64_add();
		// Demote f64 result back to f32 and store
		b.op_f32_demote_f64();
		emitStoreRdF32(b, op.rd);
		return true;
	}

	case shop_ftrv: {
		// 4x4 matrix (rs2) * 4-vector (rs1) → rd
		// rd == rs1 always (in-place), so we must save input before writing.
		// Uses f64 accumulation to match reference interpreter precision.
		// Matrix layout: innerProduct<4>(fn, fm+col) where stride=4 floats
		u32 voff = op.rs1.reg_offset();
		u32 moff = op.rs2.reg_offset();

		// Save input vector fn[0..3] into scratch locals (aliasing: rd == rs1)
		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(voff);
		b.op_i32_reinterpret_f32();
		b.op_local_set(LOCAL_TMP2);

		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(voff + 4);
		b.op_i32_reinterpret_f32();
		b.op_local_set(LOCAL_TMP3);

		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(voff + 8);
		b.op_i32_reinterpret_f32();
		b.op_local_set(LOCAL_TMP4);

		b.op_local_get(LOCAL_CTX);
		b.op_f32_load(voff + 12);
		b.op_i32_reinterpret_f32();
		b.op_local_set(LOCAL_TMP5);

		// Compute 4 dot products: fd[col] = sum_j(fn[j] * fm[j*4 + col])
		// fm layout: fm[0],fm[1],fm[2],fm[3], fm[4],fm[5],...,fm[15]
		// innerProduct<4>(fn, fm+col) = fn[0]*fm[col] + fn[1]*fm[4+col] + fn[2]*fm[8+col] + fn[3]*fm[12+col]
		for (int col = 0; col < 4; col++) {
			b.op_local_get(LOCAL_CTX);  // base for store

			// Element 0: (f64)fn[0] * (f64)fm[col]
			b.op_local_get(LOCAL_TMP2);
			b.op_f32_reinterpret_i32();
			b.op_f64_promote_f32();
			b.op_local_get(LOCAL_CTX);
			b.op_f32_load(moff + col * 4);
			b.op_f64_promote_f32();
			b.op_f64_mul();

			// Element 1: + (f64)fn[1] * (f64)fm[4+col]
			b.op_local_get(LOCAL_TMP3);
			b.op_f32_reinterpret_i32();
			b.op_f64_promote_f32();
			b.op_local_get(LOCAL_CTX);
			b.op_f32_load(moff + (4 + col) * 4);
			b.op_f64_promote_f32();
			b.op_f64_mul();
			b.op_f64_add();

			// Element 2: + (f64)fn[2] * (f64)fm[8+col]
			b.op_local_get(LOCAL_TMP4);
			b.op_f32_reinterpret_i32();
			b.op_f64_promote_f32();
			b.op_local_get(LOCAL_CTX);
			b.op_f32_load(moff + (8 + col) * 4);
			b.op_f64_promote_f32();
			b.op_f64_mul();
			b.op_f64_add();

			// Element 3: + (f64)fn[3] * (f64)fm[12+col]
			b.op_local_get(LOCAL_TMP5);
			b.op_f32_reinterpret_i32();
			b.op_f64_promote_f32();
			b.op_local_get(LOCAL_CTX);
			b.op_f32_load(moff + (12 + col) * 4);
			b.op_f64_promote_f32();
			b.op_f64_mul();
			b.op_f64_add();

			// Demote to f32 and store
			b.op_f32_demote_f64();
			b.op_f32_store(voff + col * 4);
		}
		return true;
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

	case shop_shld: {
		// Variable shift left/right (unsigned) depending on sign of rs2
		// FIX: i32.sub operand order — push 0 first, then shift, so sub = 0-shift = -shift
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
			b.op_i32_const(0);
			emitLoadParamCached(b, op.rs2, cache);
			b.op_i32_sub();  // 0 - shift = -shift
			b.op_i32_const(0x1F);
			b.op_i32_and();
			b.op_i32_eqz();
			b.op_if(WASM_TYPE_I32);
				b.op_i32_const(0);  // result is 0
			b.op_else();
				emitLoadParamCached(b, op.rs1, cache);
				b.op_i32_const(0);
				emitLoadParamCached(b, op.rs2, cache);
				b.op_i32_sub();  // 0 - shift = -shift
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
		// FIX: i32.sub operand order — push 0 first, then shift, so sub = 0-shift = -shift
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
			b.op_i32_const(0);
			emitLoadParamCached(b, op.rs2, cache);
			b.op_i32_sub();  // 0 - shift = -shift
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
				b.op_i32_const(0);
				emitLoadParamCached(b, op.rs2, cache);
				b.op_i32_sub();  // 0 - shift = -shift
				b.op_i32_const(0x1F);
				b.op_i32_and();
				b.op_i32_shr_s();  // arithmetic shift
			b.op_end();
		b.op_end();
		emitPostStore(b, op.rd, cache);
		return true;
	}

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

	// ---- Dual-output ops (rd + rd2) using i64 scratch ----

	case shop_adc: {
		// u64 res = (u64)rs1 + rs2 + rs3(carry); rd = low32, rd2 = high32
		u32 t64 = cache.tmp64Local();
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		b.op_i64_extend_i32_u();
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i64_extend_i32_u();
		b.op_i64_add();
		emitLoadParamCached(b, op.rs3, cache);
		b.op_i64_extend_i32_u();
		b.op_i64_add();
		b.op_local_tee(t64);
		b.op_i32_wrap_i64();
		emitPostStore(b, op.rd, cache);

		emitPreStore(b, op.rd2, cache);
		b.op_local_get(t64);
		b.op_i64_const(32);
		b.op_i64_shr_u();
		b.op_i32_wrap_i64();
		emitPostStore(b, op.rd2, cache);
		return true;
	}

	case shop_sbc: {
		// u64 res = (u64)rs1 - rs2 - rs3(carry); rd = low32, rd2 = (res>>32)&1
		u32 t64 = cache.tmp64Local();
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		b.op_i64_extend_i32_u();
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i64_extend_i32_u();
		b.op_i64_sub();
		emitLoadParamCached(b, op.rs3, cache);
		b.op_i64_extend_i32_u();
		b.op_i64_sub();
		b.op_local_tee(t64);
		b.op_i32_wrap_i64();
		emitPostStore(b, op.rd, cache);

		emitPreStore(b, op.rd2, cache);
		b.op_local_get(t64);
		b.op_i64_const(32);
		b.op_i64_shr_u();
		b.op_i32_wrap_i64();
		b.op_i32_const(1);
		b.op_i32_and();
		emitPostStore(b, op.rd2, cache);
		return true;
	}

	case shop_negc: {
		// u64 res = -(u64)rs1 - rs2(carry); rd = low32, rd2 = (res>>32)&1
		u32 t64 = cache.tmp64Local();
		emitPreStore(b, op.rd, cache);
		b.op_i64_const(0);
		emitLoadParamCached(b, op.rs1, cache);
		b.op_i64_extend_i32_u();
		b.op_i64_sub();
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i64_extend_i32_u();
		b.op_i64_sub();
		b.op_local_tee(t64);
		b.op_i32_wrap_i64();
		emitPostStore(b, op.rd, cache);

		emitPreStore(b, op.rd2, cache);
		b.op_local_get(t64);
		b.op_i64_const(32);
		b.op_i64_shr_u();
		b.op_i32_wrap_i64();
		b.op_i32_const(1);
		b.op_i32_and();
		emitPostStore(b, op.rd2, cache);
		return true;
	}

	case shop_rocl: {
		// rd = (rs1 << 1) | rs2; rd2 = rs1 >> 31
		// IMPORTANT: rd == rs1 (both are Rn), so save rs1 bit 31 BEFORE
		// writing rd, otherwise rd2 reads the already-shifted value.
		emitLoadParamCached(b, op.rs1, cache);
		b.op_i32_const(31);
		b.op_i32_shr_u();
		b.op_local_set(LOCAL_TMP);  // save original rs1 >> 31

		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		b.op_i32_const(1);
		b.op_i32_shl();
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_or();
		emitPostStore(b, op.rd, cache);

		emitPreStore(b, op.rd2, cache);
		b.op_local_get(LOCAL_TMP);
		emitPostStore(b, op.rd2, cache);
		return true;
	}

	case shop_rocr: {
		// rd = (rs1 >> 1) | (rs2 << 31); rd2 = rs1 & 1
		// IMPORTANT: rd == rs1 (both are Rn), so save rs1 bit 0 BEFORE
		// writing rd, otherwise rd2 reads the already-shifted value.
		emitLoadParamCached(b, op.rs1, cache);
		b.op_i32_const(1);
		b.op_i32_and();
		b.op_local_set(LOCAL_TMP);  // save original rs1 & 1

		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		b.op_i32_const(1);
		b.op_i32_shr_u();
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_const(31);
		b.op_i32_shl();
		b.op_i32_or();
		emitPostStore(b, op.rd, cache);

		emitPreStore(b, op.rd2, cache);
		b.op_local_get(LOCAL_TMP);
		emitPostStore(b, op.rd2, cache);
		return true;
	}

	case shop_mul_u64: {
		// u64 res = (u64)(u32)rs1 * (u32)rs2; rd = low32, rd2 = high32
		u32 t64 = cache.tmp64Local();
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		b.op_i64_extend_i32_u();
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i64_extend_i32_u();
		b.op_i64_mul();
		b.op_local_tee(t64);
		b.op_i32_wrap_i64();
		emitPostStore(b, op.rd, cache);

		emitPreStore(b, op.rd2, cache);
		b.op_local_get(t64);
		b.op_i64_const(32);
		b.op_i64_shr_u();
		b.op_i32_wrap_i64();
		emitPostStore(b, op.rd2, cache);
		return true;
	}

	case shop_mul_s64: {
		// s64 res = (s64)(s32)rs1 * (s32)rs2; rd = low32, rd2 = high32
		u32 t64 = cache.tmp64Local();
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		b.op_i64_extend_i32_s();
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i64_extend_i32_s();
		b.op_i64_mul();
		b.op_local_tee(t64);
		b.op_i32_wrap_i64();
		emitPostStore(b, op.rd, cache);

		emitPreStore(b, op.rd2, cache);
		b.op_local_get(t64);
		b.op_i64_const(32);
		b.op_i64_shr_u();
		b.op_i32_wrap_i64();
		emitPostStore(b, op.rd2, cache);
		return true;
	}

	// ---- Byte comparison ----

	case shop_setpeq: {
		// rd = 1 if any byte of (rs1 ^ rs2) is zero, else 0
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		emitLoadParamCached(b, op.rs2, cache);
		b.op_i32_xor();
		b.op_local_tee(LOCAL_TMP);

		// byte0 == 0?
		b.op_i32_const(0xFF);
		b.op_i32_and();
		b.op_i32_eqz();

		// byte1 == 0?
		b.op_local_get(LOCAL_TMP);
		b.op_i32_const(8);
		b.op_i32_shr_u();
		b.op_i32_const(0xFF);
		b.op_i32_and();
		b.op_i32_eqz();
		b.op_i32_or();

		// byte2 == 0?
		b.op_local_get(LOCAL_TMP);
		b.op_i32_const(16);
		b.op_i32_shr_u();
		b.op_i32_const(0xFF);
		b.op_i32_and();
		b.op_i32_eqz();
		b.op_i32_or();

		// byte3 == 0?
		b.op_local_get(LOCAL_TMP);
		b.op_i32_const(24);
		b.op_i32_shr_u();
		b.op_i32_eqz();
		b.op_i32_or();

		emitPostStore(b, op.rd, cache);
		return true;
	}

	// (division ops now native/thin-import — see cases below)

	// ---- System ops that need fallback (flush+reload around call) ----
	case shop_sync_sr:
	case shop_sync_fpscr:
		emitFlushAll(b, cache);
		b.op_i32_const((s32)block->vaddr);
		b.op_i32_const((s32)opIndex);
		b.op_call(WIMPORT_SHIL_FB);
		emitReloadAll(b, cache);
		return true;

	// ---- Prefetch: inline the common no-op path ----
	// SH4 PREF only matters for store queue flush when (addr >> 26) == 0x38.
	// 95%+ of pref ops target non-SQ addresses and are pure no-ops.
	//
	// Flush and reload are INSIDE the if block so the no-op path is zero-cost.
	// We manually emit WASM store/load instructions instead of using
	// emitFlushAll/emitReloadAll, which would modify RegCache dirty flags at
	// C++ compile time even though the WASM if body only runs conditionally.
	// Keeping dirty flags unchanged is safe: no-op path locals are untouched,
	// SQ path flushes+reloads but worst case re-flushes same values later.
	case shop_pref: {
		// ★ THIN IMPORT (2026-07-17): SQ prefetch via dedicated env.sq_pref.
		// doSqWrite reads only ctx->sq_buffer (plain memory — the register
		// cache never holds it) and never touches GPRs, so NO flush, NO
		// reload, NO fallback machinery, and chains use it identically.
		// Replaces both the old inline-flush hack (single-block-only cache
		// contract) and the chain fb bailout. pref was 90% of ALL fallback
		// calls (DOA2: ~8.5K/frame through the full shil_fb path).
		emitLoadParamCached(b, op.rs1, cache);
		b.op_local_tee(LOCAL_TMP);
		b.op_i32_const(26);
		b.op_i32_shr_u();
		b.op_i32_const(0x38);
		b.op_i32_eq();
		b.op_if();
		b.op_local_get(LOCAL_TMP);
		b.op_call(WIMPORT_SQ_PREF);
		b.op_end();
		return true;
	}

	case shop_div32u:
	case shop_div32s: {
		// ★ THIN IMPORT: 64/32 division steps. Returns (rem<<32)|quo;
		// rd = quo, rd2 = rem (dual-output pattern, cf. mul_u64).
		u32 t64 = cache.tmp64Local();
		emitPreStore(b, op.rd, cache);
		emitLoadParamCached(b, op.rs1, cache);
		emitLoadParamCached(b, op.rs2, cache);
		emitLoadParamCached(b, op.rs3, cache);
		b.op_call(op.op == shop_div32u ? WIMPORT_DIV32U : WIMPORT_DIV32S);
		b.op_local_tee(t64);
		b.op_i32_wrap_i64();
		emitPostStore(b, op.rd, cache);

		emitPreStore(b, op.rd2, cache);
		b.op_local_get(t64);
		b.op_i64_const(32);
		b.op_i64_shr_u();
		b.op_i32_wrap_i64();
		emitPostStore(b, op.rd2, cache);
		return true;
	}

	case shop_div1: {
		// ★ FULLY INLINE (2026-07-17, farm-caught): div1 reads AND writes
		// sr.Q/M, which live in sr.status — a register the surrounding decode
		// manipulates via (possibly CACHED) reg ops. The earlier thin-import
		// version read stale ctx state and had its writes clobbered by the
		// cached local (validation farm: sr.status Q-bit diffs on DOA2
		// division blocks). Inline emission uses the same cached-or-ctx
		// status the neighbor ops use — coherent by construction.
		s32 stLocal = cache.getLocal(ctx_off::SR_STATUS);
		if (stLocal >= 0) {
			for (auto& [coff, ce] : cache.entries)
				if (coff == ctx_off::SR_STATUS) ce.dirty = true;
		}
		auto loadStatus = [&]() {
			if (stLocal >= 0) b.op_local_get((u32)stLocal);
			else { b.op_local_get(LOCAL_CTX); b.op_i32_load(ctx_off::SR_STATUS); }
		};

		// TMP=a  TMP3=b  TMP4=T(then oldA)  TMP2=status  TMP5=qxm(then Qnew/Tout)
		emitLoadParamCached(b, op.rs1, cache);
		b.op_local_set(LOCAL_TMP);
		emitLoadParamCached(b, op.rs2, cache);
		b.op_local_set(LOCAL_TMP3);
		emitLoadParamCached(b, op.rs3, cache);
		b.op_local_set(LOCAL_TMP4);
		loadStatus();
		b.op_local_set(LOCAL_TMP2);

		// qxm = ((S>>8) ^ (S>>9)) & 1
		b.op_local_get(LOCAL_TMP2);
		b.op_i32_const(8);
		b.op_i32_shr_u();
		b.op_local_get(LOCAL_TMP2);
		b.op_i32_const(9);
		b.op_i32_shr_u();
		b.op_i32_xor();
		b.op_i32_const(1);
		b.op_i32_and();
		b.op_local_set(LOCAL_TMP5);

		// S = (S & ~0x100) | ((a >>> 23) & 0x100)   [Q = sign(a)]
		b.op_local_get(LOCAL_TMP2);
		b.op_i32_const((s32)~0x100u);
		b.op_i32_and();
		b.op_local_get(LOCAL_TMP);
		b.op_i32_const(23);
		b.op_i32_shr_u();
		b.op_i32_const(0x100);
		b.op_i32_and();
		b.op_i32_or();
		b.op_local_set(LOCAL_TMP2);

		// a = (a << 1) | T
		b.op_local_get(LOCAL_TMP);
		b.op_i32_const(1);
		b.op_i32_shl();
		b.op_local_get(LOCAL_TMP4);
		b.op_i32_or();
		b.op_local_set(LOCAL_TMP);

		// oldA = a (TMP4 reused — T consumed)
		b.op_local_get(LOCAL_TMP);
		b.op_local_set(LOCAL_TMP4);

		// a = qxm ? a + b : a - b
		b.op_local_get(LOCAL_TMP5);
		b.op_if(0x7F);   // if (result i32)
		b.op_local_get(LOCAL_TMP);
		b.op_local_get(LOCAL_TMP3);
		b.op_i32_add();
		b.op_else();
		b.op_local_get(LOCAL_TMP);
		b.op_local_get(LOCAL_TMP3);
		b.op_i32_sub();
		b.op_end();
		b.op_local_set(LOCAL_TMP);

		// carry = qxm ? (a <u oldA) : (a >u oldA)
		b.op_local_get(LOCAL_TMP5);
		b.op_if(0x7F);
		b.op_local_get(LOCAL_TMP);
		b.op_local_get(LOCAL_TMP4);
		b.op_i32_lt_u();
		b.op_else();
		b.op_local_get(LOCAL_TMP);
		b.op_local_get(LOCAL_TMP4);
		b.op_i32_gt_u();
		b.op_end();

		// Qnew = ((S>>8) ^ (S>>9) ^ carry) & 1     [carry on stack]
		b.op_local_get(LOCAL_TMP2);
		b.op_i32_const(8);
		b.op_i32_shr_u();
		b.op_i32_xor();
		b.op_local_get(LOCAL_TMP2);
		b.op_i32_const(9);
		b.op_i32_shr_u();
		b.op_i32_xor();
		b.op_i32_const(1);
		b.op_i32_and();
		b.op_local_set(LOCAL_TMP5);

		// S = (S & ~0x100) | (Qnew << 8)
		b.op_local_get(LOCAL_TMP2);
		b.op_i32_const((s32)~0x100u);
		b.op_i32_and();
		b.op_local_get(LOCAL_TMP5);
		b.op_i32_const(8);
		b.op_i32_shl();
		b.op_i32_or();
		b.op_local_set(LOCAL_TMP2);

		// write status back (cached local already dirty-marked, or ctx)
		if (stLocal >= 0) {
			b.op_local_get(LOCAL_TMP2);
			b.op_local_set((u32)stLocal);
		} else {
			b.op_local_get(LOCAL_CTX);
			b.op_local_get(LOCAL_TMP2);
			b.op_i32_store(ctx_off::SR_STATUS);
		}

		// T_out = 1 ^ (Qnew ^ M) = 1 ^ (Qnew ^ ((S>>9)&1))
		b.op_local_get(LOCAL_TMP5);
		b.op_local_get(LOCAL_TMP2);
		b.op_i32_const(9);
		b.op_i32_shr_u();
		b.op_i32_const(1);
		b.op_i32_and();
		b.op_i32_xor();
		b.op_i32_const(1);
		b.op_i32_xor();
		b.op_local_set(LOCAL_TMP5);

		emitPreStore(b, op.rd, cache);
		b.op_local_get(LOCAL_TMP);
		emitPostStore(b, op.rd, cache);

		emitPreStore(b, op.rd2, cache);
		b.op_local_get(LOCAL_TMP5);
		emitPostStore(b, op.rd2, cache);
		return true;
	}

	case shop_div32p2: {
		// Division finalization — pure ALU, fully inline (canonical
		// semantics: sign of quotient in T bit 31, step parity in T bit 0).
		emitLoadParamCached(b, op.rs1, cache);   // a
		b.op_local_set(LOCAL_TMP);
		emitLoadParamCached(b, op.rs2, cache);   // b
		b.op_local_set(LOCAL_TMP2);
		emitLoadParamCached(b, op.rs3, cache);   // T
		b.op_local_set(LOCAL_TMP3);

		b.op_local_get(LOCAL_TMP3);
		b.op_i32_const((s32)0x80000000);
		b.op_i32_and();
		b.op_i32_eqz();
		b.op_if();
		{
			// if (!(T & 1)) a -= b
			b.op_local_get(LOCAL_TMP3);
			b.op_i32_const(1);
			b.op_i32_and();
			b.op_i32_eqz();
			b.op_if();
			b.op_local_get(LOCAL_TMP);
			b.op_local_get(LOCAL_TMP2);
			b.op_i32_sub();
			b.op_local_set(LOCAL_TMP);
			b.op_end();
		}
		b.op_else();
		{
			// if (b > 0) a--
			b.op_local_get(LOCAL_TMP2);
			b.op_i32_const(0);
			b.op_i32_gt_s();
			b.op_if();
			b.op_local_get(LOCAL_TMP);
			b.op_i32_const(1);
			b.op_i32_sub();
			b.op_local_set(LOCAL_TMP);
			b.op_end();
			// if (T & 1) a += b
			b.op_local_get(LOCAL_TMP3);
			b.op_i32_const(1);
			b.op_i32_and();
			b.op_if();
			b.op_local_get(LOCAL_TMP);
			b.op_local_get(LOCAL_TMP2);
			b.op_i32_add();
			b.op_local_set(LOCAL_TMP);
			b.op_end();
		}
		b.op_end();

		emitPreStore(b, op.rd, cache);
		b.op_local_get(LOCAL_TMP);
		emitPostStore(b, op.rd, cache);
		return true;
	}

	case shop_fsca: {
		// Hardware sin/cos — direct table load, zero calls. rd is the fr
		// pair (sin, cos); FPU regs are never register-cached, so plain
		// ctx stores are exact.
		u32 tbl = fly_sin_table_addr();
		u32 doff = op.rd.reg_offset();
		// LOCAL_TMP = tbl + (rs1 & 0xFFFF) * 8
		emitLoadParamCached(b, op.rs1, cache);
		b.op_i32_const(0xFFFF);
		b.op_i32_and();
		b.op_i32_const(8);
		b.op_i32_mul();
		b.op_i32_const((s32)tbl);
		b.op_i32_add();
		b.op_local_set(LOCAL_TMP);
		// fr[d] = sin
		b.op_local_get(LOCAL_CTX);
		b.op_local_get(LOCAL_TMP);
		b.op_f32_load(0);
		b.op_f32_store(doff);
		// fr[d+1] = cos
		b.op_local_get(LOCAL_CTX);
		b.op_local_get(LOCAL_TMP);
		b.op_f32_load(4);
		b.op_f32_store(doff + 4);
		return true;
	}

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
