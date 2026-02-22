// wasm_emit.h — SHIL → WASM instruction emitters for Flycast JIT
//
// Translates individual SHIL IR opcodes into WASM instructions using
// WasmModuleBuilder. Operates on Sh4Context in shared linear memory.

#pragma once
#include "wasm_module_builder.h"
#include "hw/sh4/dyna/shil.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/sh4/dyna/decoder.h"

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
// Local 1 = temp i32 (for intermediate values)
constexpr u32 LOCAL_CTX = 0;
constexpr u32 LOCAL_TMP = 1;

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
// Emit a complete SHIL op. Returns true if handled, false if
// fallback is needed.
// ============================================================
static bool emitShilOp(WasmModuleBuilder& b, const shil_opcode& op,
                        RuntimeBlockInfo* block, u32 opIndex) {
	switch (op.op) {

	// ---- Tier 1: Integer ALU ----

	case shop_mov32:
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		emitStoreRd(b, op.rd);
		return true;

	case shop_add:
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		emitLoadParam(b, op.rs2);
		b.op_i32_add();
		emitStoreRd(b, op.rd);
		return true;

	case shop_sub:
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		emitLoadParam(b, op.rs2);
		b.op_i32_sub();
		emitStoreRd(b, op.rd);
		return true;

	case shop_and:
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		emitLoadParam(b, op.rs2);
		b.op_i32_and();
		emitStoreRd(b, op.rd);
		return true;

	case shop_or:
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		emitLoadParam(b, op.rs2);
		b.op_i32_or();
		emitStoreRd(b, op.rd);
		return true;

	case shop_xor:
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		emitLoadParam(b, op.rs2);
		b.op_i32_xor();
		emitStoreRd(b, op.rd);
		return true;

	case shop_not:
		// rd = ~rs1 = rs1 XOR -1
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		b.op_i32_const(-1);
		b.op_i32_xor();
		emitStoreRd(b, op.rd);
		return true;

	case shop_neg:
		// rd = 0 - rs1
		b.op_local_get(LOCAL_CTX);
		b.op_i32_const(0);
		emitLoadParam(b, op.rs1);
		b.op_i32_sub();
		emitStoreRd(b, op.rd);
		return true;

	case shop_shl:
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		emitLoadParam(b, op.rs2);
		b.op_i32_shl();
		emitStoreRd(b, op.rd);
		return true;

	case shop_shr:
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		emitLoadParam(b, op.rs2);
		b.op_i32_shr_u();
		emitStoreRd(b, op.rd);
		return true;

	case shop_sar:
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		emitLoadParam(b, op.rs2);
		b.op_i32_shr_s();
		emitStoreRd(b, op.rd);
		return true;

	case shop_ror:
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		emitLoadParam(b, op.rs2);
		b.op_i32_rotr();
		emitStoreRd(b, op.rd);
		return true;

	case shop_ext_s8:
		// Sign-extend 8→32: (val << 24) >> 24
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		b.op_i32_const(24);
		b.op_i32_shl();
		b.op_i32_const(24);
		b.op_i32_shr_s();
		emitStoreRd(b, op.rd);
		return true;

	case shop_ext_s16:
		// Sign-extend 16→32: (val << 16) >> 16
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		b.op_i32_const(16);
		b.op_i32_shl();
		b.op_i32_const(16);
		b.op_i32_shr_s();
		emitStoreRd(b, op.rd);
		return true;

	case shop_mul_u16:
		// rd = (u16)rs1 * (u16)rs2
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		b.op_i32_const(0xFFFF);
		b.op_i32_and();
		emitLoadParam(b, op.rs2);
		b.op_i32_const(0xFFFF);
		b.op_i32_and();
		b.op_i32_mul();
		emitStoreRd(b, op.rd);
		return true;

	case shop_mul_s16:
		// rd = (s16)rs1 * (s16)rs2
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		b.op_i32_const(16);
		b.op_i32_shl();
		b.op_i32_const(16);
		b.op_i32_shr_s();
		emitLoadParam(b, op.rs2);
		b.op_i32_const(16);
		b.op_i32_shl();
		b.op_i32_const(16);
		b.op_i32_shr_s();
		b.op_i32_mul();
		emitStoreRd(b, op.rd);
		return true;

	case shop_mul_i32:
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		emitLoadParam(b, op.rs2);
		b.op_i32_mul();
		emitStoreRd(b, op.rd);
		return true;

	case shop_swaplb:
		// Swap low bytes: ((val >> 8) & 0xFF) | ((val & 0xFF) << 8) | (val & 0xFFFF0000)
		// Simplified: rotate16 of lower 16 bits, keep upper 16
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
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
		emitStoreRd(b, op.rd);
		return true;

	case shop_xtrct:
		// rd = (rs1 >> 16) | (rs2 << 16)  — matches canonical/C++ fallback
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		b.op_i32_const(16);
		b.op_i32_shr_u();
		emitLoadParam(b, op.rs2);
		b.op_i32_const(16);
		b.op_i32_shl();
		b.op_i32_or();
		emitStoreRd(b, op.rd);
		return true;

	// ---- Comparisons ----

	case shop_test:
		// rd = (rs1 & rs2) == 0
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		emitLoadParam(b, op.rs2);
		b.op_i32_and();
		b.op_i32_eqz();
		emitStoreRd(b, op.rd);
		return true;

	case shop_seteq:
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		emitLoadParam(b, op.rs2);
		b.op_i32_eq();
		emitStoreRd(b, op.rd);
		return true;

	case shop_setge:
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		emitLoadParam(b, op.rs2);
		b.op_i32_ge_s();
		emitStoreRd(b, op.rd);
		return true;

	case shop_setgt:
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		emitLoadParam(b, op.rs2);
		b.op_i32_gt_s();
		emitStoreRd(b, op.rd);
		return true;

	case shop_setae:
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		emitLoadParam(b, op.rs2);
		b.op_i32_ge_u();
		emitStoreRd(b, op.rd);
		return true;

	case shop_setab:
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		emitLoadParam(b, op.rs2);
		b.op_i32_gt_u();
		emitStoreRd(b, op.rd);
		return true;

	// ---- Dynamic jump / conditional ----

	case shop_jdyn:
		// Store jump target to ctx.jdyn
		// jdyn = rs1 (+ rs2 if present, e.g., bsrf Rn → jdyn = Rn + PC+4)
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		if (!op.rs2.is_null()) {
			emitLoadParam(b, op.rs2);
			b.op_i32_add();
		}
		b.op_i32_store(ctx_off::JDYN);
		return true;

	case shop_jcond:
		// Store condition to ctx.sr.T
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		b.op_i32_store(ctx_off::SR_T);
		return true;

	// ---- Memory operations ----

	case shop_readm: {
		// Compute address: rs1 + rs3 (if rs3 not null)
		// Store address in LOCAL_TMP for reuse in 64-bit case
		emitLoadParam(b, op.rs1);
		if (!op.rs3.is_null()) {
			emitLoadParam(b, op.rs3);
			b.op_i32_add();
		}

		if (op.size == 8) {
			// 64-bit read: two 32-bit reads → rd (low) and rd+4 (high)
			b.op_local_tee(LOCAL_TMP);
			b.op_call(WIMPORT_READ32);
			// Store low word to rd
			{
				u32 off = op.rd.reg_offset();
				b.op_local_set(LOCAL_TMP); // save result
				b.op_local_get(LOCAL_CTX);
				b.op_local_get(LOCAL_TMP);
				b.op_i32_store(off);
			}
			// Read high word (addr + 4) — recompute address since LOCAL_TMP was reused
			emitLoadParam(b, op.rs1);
			if (!op.rs3.is_null()) {
				emitLoadParam(b, op.rs3);
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
			u32 readFunc;
			switch (op.size) {
			case 1: readFunc = WIMPORT_READ8; break;
			case 2: readFunc = WIMPORT_READ16; break;
			default: readFunc = WIMPORT_READ32; break;
			}
			b.op_call(readFunc);

			b.op_local_set(LOCAL_TMP);
			b.op_local_get(LOCAL_CTX);
			b.op_local_get(LOCAL_TMP);
			b.op_i32_store(op.rd.reg_offset());
		}
		return true;
	}

	case shop_writem: {
		if (op.size == 8) {
			// 64-bit write: two 32-bit writes
			// Write low word
			emitLoadParam(b, op.rs1);
			if (!op.rs3.is_null()) {
				emitLoadParam(b, op.rs3);
				b.op_i32_add();
			}
			// Load low word from rs2
			b.op_local_get(LOCAL_CTX);
			b.op_i32_load(op.rs2.reg_offset());
			b.op_call(WIMPORT_WRITE32);

			// Write high word (addr + 4)
			emitLoadParam(b, op.rs1);
			if (!op.rs3.is_null()) {
				emitLoadParam(b, op.rs3);
				b.op_i32_add();
			}
			b.op_i32_const(4);
			b.op_i32_add();
			b.op_local_get(LOCAL_CTX);
			b.op_i32_load(op.rs2.reg_offset() + 4);
			b.op_call(WIMPORT_WRITE32);
		} else {
			emitLoadParam(b, op.rs1);
			if (!op.rs3.is_null()) {
				emitLoadParam(b, op.rs3);
				b.op_i32_add();
			}
			emitLoadParam(b, op.rs2);

			u32 writeFunc;
			switch (op.size) {
			case 1: writeFunc = WIMPORT_WRITE8; break;
			case 2: writeFunc = WIMPORT_WRITE16; break;
			default: writeFunc = WIMPORT_WRITE32; break;
			}
			b.op_call(writeFunc);
		}
		return true;
	}

	// ---- Interpreter fallback (single SH4 opcode) ----

	case shop_ifb:
		// rs1._imm: if nonzero, set ctx.pc = rs2._imm before executing
		if (op.rs1._imm) {
			b.op_local_get(LOCAL_CTX);
			b.op_i32_const((s32)op.rs2._imm);
			b.op_i32_store(ctx_off::PC);
		}
		// Call ifb(opcode, pc)
		b.op_i32_const((s32)op.rs3._imm);  // SH4 opcode
		b.op_i32_const((s32)(block->vaddr + op.guest_offs - (op.delay_slot ? 2 : 0)));
		b.op_call(WIMPORT_IFB);
		return true;

	// ---- Tier 2: FPU ops ----

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
		// rd = (rs1 == rs2) ? 1 : 0
		b.op_local_get(LOCAL_CTX);
		emitLoadParamF32(b, op.rs1);
		emitLoadParamF32(b, op.rs2);
		b.op_f32_eq();
		emitStoreRd(b, op.rd);
		return true;

	case shop_fsetgt:
		// rd = (rs1 > rs2) ? 1 : 0
		b.op_local_get(LOCAL_CTX);
		emitLoadParamF32(b, op.rs1);
		emitLoadParamF32(b, op.rs2);
		b.op_f32_gt();
		emitStoreRd(b, op.rd);
		return true;

	case shop_cvt_i2f_n:
	case shop_cvt_i2f_z:
		// rd(f32) = (float)(s32)rs1
		b.op_local_get(LOCAL_CTX);
		emitLoadParam(b, op.rs1);
		b.op_f32_convert_i32_s();
		emitStoreRdF32(b, op.rd);
		return true;

	case shop_cvt_f2i_t:
		// rd(i32) = (s32)rs1(f32)  — truncate toward zero
		// Need to handle NaN and overflow per SH4 semantics
		// For now, use WASM trunc with a simple fallback
		b.op_local_get(LOCAL_CTX);
		emitLoadParamF32(b, op.rs1);
		// Use trunc_sat instead of trunc to avoid trapping on NaN/overflow
		// WASM trunc_f32_s traps on NaN — we need saturating version
		// Saturating: 0xFC 0x00 (i32.trunc_sat_f32_s)
		b.emitByte(0xFC);
		b.emitLEB128(0x00); // i32.trunc_sat_f32_s
		emitStoreRd(b, op.rd);
		return true;

	case shop_mov64:
		// Copy 64 bits (two 32-bit values) from rs1 to rd
		if (op.rs1.is_reg() && op.rd.is_reg()) {
			u32 srcOff = op.rs1.reg_offset();
			u32 dstOff = op.rd.reg_offset();
			// Copy first word
			b.op_local_get(LOCAL_CTX);
			b.op_local_get(LOCAL_CTX);
			b.op_i32_load(srcOff);
			b.op_i32_store(dstOff);
			// Copy second word
			b.op_local_get(LOCAL_CTX);
			b.op_local_get(LOCAL_CTX);
			b.op_i32_load(srcOff + 4);
			b.op_i32_store(dstOff + 4);
			return true;
		}
		return false;

	// ---- System ops that need fallback ----
	case shop_sync_sr:
	case shop_sync_fpscr:
	case shop_pref:
		// These need the full C++ implementation
		b.op_i32_const((s32)block->vaddr);
		b.op_i32_const((s32)opIndex);
		b.op_call(WIMPORT_SHIL_FB);
		return true;

	default:
		return false;
	}
}

// ============================================================
// Emit block exit code based on BlockEndType
// ============================================================
static void emitBlockExit(WasmModuleBuilder& b, RuntimeBlockInfo* block) {
	u32 bcls = BET_GET_CLS(block->BlockType);

	switch (bcls) {
	case BET_CLS_Static:
		if (block->BlockType == BET_StaticIntr) {
			// ctx.pc = NextBlock
			b.op_local_get(LOCAL_CTX);
			b.op_i32_const((s32)block->NextBlock);
			b.op_i32_store(ctx_off::PC);
		} else {
			// ctx.pc = BranchBlock
			b.op_local_get(LOCAL_CTX);
			b.op_i32_const((s32)block->BranchBlock);
			b.op_i32_store(ctx_off::PC);
		}
		break;

	case BET_CLS_Dynamic:
		// ctx.pc = ctx.jdyn — all dynamic exits use jdyn
		// (shop_jdyn already stored the target, even for RTS which copies PR→jdyn)
		b.op_local_get(LOCAL_CTX);
		b.op_local_get(LOCAL_CTX);
		b.op_i32_load(ctx_off::JDYN);
		b.op_i32_store(ctx_off::PC);
		break;

	case BET_CLS_COND: {
		// if (sr.T == cond) pc = BranchBlock else pc = NextBlock
		u32 cond = (block->BlockType == BET_Cond_1) ? 1 : 0;

		b.op_local_get(LOCAL_CTX);  // base for store

		b.op_local_get(LOCAL_CTX);
		b.op_i32_load(ctx_off::SR_T);
		if (cond == 1) {
			// T == 1: branch taken
			b.op_if(WASM_TYPE_I32);
		} else {
			// T == 0: branch taken
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
