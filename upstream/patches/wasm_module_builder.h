// wasm_module_builder.h — WASM binary format builder for Flycast JIT
//
// Builds valid WebAssembly modules byte-by-byte. No external dependencies.
// Used by rec_wasm.cpp to compile SH4 basic blocks into WASM functions.

#pragma once
#include "types.h"
#include <vector>
#include <cstring>

// WASM value types
constexpr u8 WASM_TYPE_I32 = 0x7F;
constexpr u8 WASM_TYPE_I64 = 0x7E;
constexpr u8 WASM_TYPE_F32 = 0x7D;
constexpr u8 WASM_TYPE_F64 = 0x7C;
constexpr u8 WASM_TYPE_FUNC = 0x60;

// WASM section IDs
constexpr u8 WASM_SEC_TYPE = 1;
constexpr u8 WASM_SEC_IMPORT = 2;
constexpr u8 WASM_SEC_FUNCTION = 3;
constexpr u8 WASM_SEC_EXPORT = 7;
constexpr u8 WASM_SEC_CODE = 10;

// WASM import/export kinds
constexpr u8 WASM_IMPORT_FUNC = 0x00;
constexpr u8 WASM_IMPORT_MEMORY = 0x02;
constexpr u8 WASM_EXPORT_FUNC = 0x00;

// WASM opcodes
namespace wop {
	constexpr u8 unreachable   = 0x00;
	constexpr u8 nop           = 0x01;
	constexpr u8 block         = 0x02;
	constexpr u8 loop_         = 0x03;
	constexpr u8 if_           = 0x04;
	constexpr u8 else_         = 0x05;
	constexpr u8 end           = 0x0B;
	constexpr u8 br            = 0x0C;
	constexpr u8 br_if         = 0x0D;
	constexpr u8 return_       = 0x0F;
	constexpr u8 call          = 0x10;
	constexpr u8 drop          = 0x1A;
	constexpr u8 select        = 0x1B;
	constexpr u8 local_get     = 0x20;
	constexpr u8 local_set     = 0x21;
	constexpr u8 local_tee     = 0x22;
	constexpr u8 i32_load      = 0x28;
	constexpr u8 i64_load      = 0x29;
	constexpr u8 f32_load      = 0x2A;
	constexpr u8 f64_load      = 0x2B;
	constexpr u8 i32_load8_s   = 0x2C;
	constexpr u8 i32_load8_u   = 0x2D;
	constexpr u8 i32_load16_s  = 0x2E;
	constexpr u8 i32_load16_u  = 0x2F;
	constexpr u8 i32_store     = 0x36;
	constexpr u8 f32_store     = 0x38;
	constexpr u8 i32_store8    = 0x3A;
	constexpr u8 i32_store16   = 0x3B;
	constexpr u8 i32_const     = 0x41;
	constexpr u8 f32_const     = 0x43;
	constexpr u8 i32_eqz       = 0x45;
	constexpr u8 i32_eq        = 0x46;
	constexpr u8 i32_ne        = 0x47;
	constexpr u8 i32_lt_s      = 0x48;
	constexpr u8 i32_lt_u      = 0x49;
	constexpr u8 i32_gt_s      = 0x4A;
	constexpr u8 i32_gt_u      = 0x4B;
	constexpr u8 i32_le_s      = 0x4C;
	constexpr u8 i32_le_u      = 0x4D;
	constexpr u8 i32_ge_s      = 0x4E;
	constexpr u8 i32_ge_u      = 0x4F;
	constexpr u8 f32_eq        = 0x5B;
	constexpr u8 f32_gt        = 0x5E;
	constexpr u8 i32_clz       = 0x67;
	constexpr u8 i32_add       = 0x6A;
	constexpr u8 i32_sub       = 0x6B;
	constexpr u8 i32_mul       = 0x6C;
	constexpr u8 i32_div_s     = 0x6D;
	constexpr u8 i32_div_u     = 0x6E;
	constexpr u8 i32_rem_s     = 0x6F;
	constexpr u8 i32_rem_u     = 0x70;
	constexpr u8 i32_and       = 0x71;
	constexpr u8 i32_or        = 0x72;
	constexpr u8 i32_xor       = 0x73;
	constexpr u8 i32_shl       = 0x74;
	constexpr u8 i32_shr_s     = 0x75;
	constexpr u8 i32_shr_u     = 0x76;
	constexpr u8 i32_rotl      = 0x77;
	constexpr u8 i32_rotr      = 0x78;
	constexpr u8 f32_abs       = 0x8B;
	constexpr u8 f32_neg       = 0x8C;
	constexpr u8 f32_sqrt      = 0x91;
	constexpr u8 f32_add       = 0x92;
	constexpr u8 f32_sub       = 0x93;
	constexpr u8 f32_mul       = 0x94;
	constexpr u8 f32_div       = 0x95;
	// i64 arithmetic
	constexpr u8 i64_clz       = 0x79;
	constexpr u8 i64_add       = 0x7C;
	constexpr u8 i64_sub       = 0x7D;
	constexpr u8 i64_mul       = 0x7E;
	constexpr u8 i64_and       = 0x83;
	constexpr u8 i64_or        = 0x84;
	constexpr u8 i64_xor       = 0x85;
	constexpr u8 i64_shl       = 0x86;
	constexpr u8 i64_shr_s     = 0x87;
	constexpr u8 i64_shr_u     = 0x88;
	constexpr u8 i64_const     = 0x42;

	// f64 ops
	constexpr u8 f64_add       = 0xA0;
	constexpr u8 f64_mul       = 0xA2;

	// Conversions
	constexpr u8 i32_wrap_i64  = 0xA7;
	constexpr u8 i32_trunc_f32_s = 0xA8;
	constexpr u8 i64_extend_i32_s = 0xAC;
	constexpr u8 i64_extend_i32_u = 0xAD;
	constexpr u8 f32_convert_i32_s = 0xB2;
	constexpr u8 f64_promote_f32   = 0xBB;
	constexpr u8 i32_reinterpret_f32 = 0xBC;
	constexpr u8 f32_demote_f64    = 0xB6;
	constexpr u8 f32_reinterpret_i32 = 0xBE;
}

class WasmModuleBuilder {
public:
	// --- Low-level encoding ---

	void emitByte(u8 b) { bytes.push_back(b); }

	void emitU32LE(u32 v) {
		bytes.push_back(v & 0xFF);
		bytes.push_back((v >> 8) & 0xFF);
		bytes.push_back((v >> 16) & 0xFF);
		bytes.push_back((v >> 24) & 0xFF);
	}

	void emitLEB128(u32 v) {
		do {
			u8 b = v & 0x7F;
			v >>= 7;
			if (v != 0) b |= 0x80;
			bytes.push_back(b);
		} while (v != 0);
	}

	void emitSignedLEB128(s32 v) {
		bool more = true;
		while (more) {
			u8 b = v & 0x7F;
			v >>= 7;
			if ((v == 0 && (b & 0x40) == 0) || (v == -1 && (b & 0x40) != 0))
				more = false;
			else
				b |= 0x80;
			bytes.push_back(b);
		}
	}

	void emitBytes(const void* data, size_t len) {
		const u8* p = (const u8*)data;
		bytes.insert(bytes.end(), p, p + len);
	}

	void emitName(const char* name) {
		u32 len = (u32)strlen(name);
		emitLEB128(len);
		emitBytes(name, len);
	}

	// --- Section management ---

	void beginSection(u8 sectionId) {
		emitByte(sectionId);
		sectionSizePos = (u32)bytes.size();
		// Placeholder for section size (5 bytes max LEB128 for u32)
		bytes.push_back(0); bytes.push_back(0); bytes.push_back(0);
		bytes.push_back(0); bytes.push_back(0);
		sectionContentStart = (u32)bytes.size();
	}

	void endSection() {
		u32 contentSize = (u32)bytes.size() - sectionContentStart;
		// Patch the 5-byte LEB128 size at sectionSizePos
		patchLEB128_5(sectionSizePos, contentSize);
	}

	// --- Module header ---

	void emitHeader() {
		// Magic: \0asm
		emitByte(0x00); emitByte(0x61); emitByte(0x73); emitByte(0x6D);
		// Version: 1
		emitByte(0x01); emitByte(0x00); emitByte(0x00); emitByte(0x00);
	}

	// --- Type section ---

	void emitTypeSection(u32 count) {
		beginSection(WASM_SEC_TYPE);
		emitLEB128(count);
	}

	void emitFuncType(const u8* params, u32 paramCount, const u8* results, u32 resultCount) {
		emitByte(WASM_TYPE_FUNC);
		emitLEB128(paramCount);
		for (u32 i = 0; i < paramCount; i++) emitByte(params[i]);
		emitLEB128(resultCount);
		for (u32 i = 0; i < resultCount; i++) emitByte(results[i]);
	}

	// --- Import section ---

	void emitImportSection(u32 count) {
		beginSection(WASM_SEC_IMPORT);
		emitLEB128(count);
	}

	void emitImportMemory(const char* module, const char* name, u32 initialPages) {
		emitName(module);
		emitName(name);
		emitByte(WASM_IMPORT_MEMORY);
		emitByte(0x00); // flags: no max
		emitLEB128(initialPages);
	}

	void emitImportFunc(const char* module, const char* name, u32 typeIdx) {
		emitName(module);
		emitName(name);
		emitByte(WASM_IMPORT_FUNC);
		emitLEB128(typeIdx);
	}

	// --- Function section ---

	void emitFunctionSection(u32 count, const u32* typeIndices) {
		beginSection(WASM_SEC_FUNCTION);
		emitLEB128(count);
		for (u32 i = 0; i < count; i++) emitLEB128(typeIndices[i]);
		endSection();
	}

	// --- Export section ---

	void emitExportSection(const char* name, u32 funcIdx) {
		beginSection(WASM_SEC_EXPORT);
		emitLEB128(1); // 1 export
		emitName(name);
		emitByte(WASM_EXPORT_FUNC);
		emitLEB128(funcIdx);
		endSection();
	}

	// --- Code section ---

	void beginCodeSection(u32 funcCount) {
		beginSection(WASM_SEC_CODE);
		emitLEB128(funcCount);
	}

	void beginFuncBody() {
		funcBodySizePos = (u32)bytes.size();
		// Placeholder for body size (5 bytes)
		bytes.push_back(0); bytes.push_back(0); bytes.push_back(0);
		bytes.push_back(0); bytes.push_back(0);
		funcBodyStart = (u32)bytes.size();
	}

	void emitLocals(u32 groupCount, const u32* counts, const u8* types) {
		emitLEB128(groupCount);
		for (u32 i = 0; i < groupCount; i++) {
			emitLEB128(counts[i]);
			emitByte(types[i]);
		}
	}

	void endFuncBody() {
		emitByte(wop::end); // function end
		u32 bodySize = (u32)bytes.size() - funcBodyStart;
		patchLEB128_5(funcBodySizePos, bodySize);
	}

	// --- WASM instructions ---

	void op_local_get(u32 idx) { emitByte(wop::local_get); emitLEB128(idx); }
	void op_local_set(u32 idx) { emitByte(wop::local_set); emitLEB128(idx); }
	void op_local_tee(u32 idx) { emitByte(wop::local_tee); emitLEB128(idx); }

	void op_i32_const(s32 val) { emitByte(wop::i32_const); emitSignedLEB128(val); }
	void op_f32_const(float val) {
		emitByte(wop::f32_const);
		u32 bits;
		memcpy(&bits, &val, 4);
		emitU32LE(bits);
	}

	// Memory load/store (align=log2 of natural alignment)
	void op_i32_load(u32 offset, u32 align = 2) {
		emitByte(wop::i32_load); emitLEB128(align); emitLEB128(offset);
	}
	void op_i32_load8_s(u32 offset) {
		emitByte(wop::i32_load8_s); emitLEB128(0); emitLEB128(offset);
	}
	void op_i32_load8_u(u32 offset) {
		emitByte(wop::i32_load8_u); emitLEB128(0); emitLEB128(offset);
	}
	void op_i32_load16_s(u32 offset) {
		emitByte(wop::i32_load16_s); emitLEB128(1); emitLEB128(offset);
	}
	void op_i32_load16_u(u32 offset) {
		emitByte(wop::i32_load16_u); emitLEB128(1); emitLEB128(offset);
	}
	void op_i32_store(u32 offset, u32 align = 2) {
		emitByte(wop::i32_store); emitLEB128(align); emitLEB128(offset);
	}
	void op_i32_store8(u32 offset) {
		emitByte(wop::i32_store8); emitLEB128(0); emitLEB128(offset);
	}
	void op_i32_store16(u32 offset) {
		emitByte(wop::i32_store16); emitLEB128(1); emitLEB128(offset);
	}
	void op_f32_load(u32 offset, u32 align = 2) {
		emitByte(wop::f32_load); emitLEB128(align); emitLEB128(offset);
	}
	void op_f32_store(u32 offset, u32 align = 2) {
		emitByte(wop::f32_store); emitLEB128(align); emitLEB128(offset);
	}

	// Arithmetic / logic
	void op_i32_add()   { emitByte(wop::i32_add); }
	void op_i32_sub()   { emitByte(wop::i32_sub); }
	void op_i32_mul()   { emitByte(wop::i32_mul); }
	void op_i32_div_s() { emitByte(wop::i32_div_s); }
	void op_i32_div_u() { emitByte(wop::i32_div_u); }
	void op_i32_rem_s() { emitByte(wop::i32_rem_s); }
	void op_i32_rem_u() { emitByte(wop::i32_rem_u); }
	void op_i32_and()   { emitByte(wop::i32_and); }
	void op_i32_or()    { emitByte(wop::i32_or); }
	void op_i32_xor()   { emitByte(wop::i32_xor); }
	void op_i32_shl()   { emitByte(wop::i32_shl); }
	void op_i32_shr_s() { emitByte(wop::i32_shr_s); }
	void op_i32_shr_u() { emitByte(wop::i32_shr_u); }
	void op_i32_rotl()  { emitByte(wop::i32_rotl); }
	void op_i32_rotr()  { emitByte(wop::i32_rotr); }
	void op_i32_clz()   { emitByte(wop::i32_clz); }

	// Comparison
	void op_i32_eqz()   { emitByte(wop::i32_eqz); }
	void op_i32_eq()    { emitByte(wop::i32_eq); }
	void op_i32_ne()    { emitByte(wop::i32_ne); }
	void op_i32_lt_s()  { emitByte(wop::i32_lt_s); }
	void op_i32_lt_u()  { emitByte(wop::i32_lt_u); }
	void op_i32_gt_s()  { emitByte(wop::i32_gt_s); }
	void op_i32_gt_u()  { emitByte(wop::i32_gt_u); }
	void op_i32_le_s()  { emitByte(wop::i32_le_s); }
	void op_i32_le_u()  { emitByte(wop::i32_le_u); }
	void op_i32_ge_s()  { emitByte(wop::i32_ge_s); }
	void op_i32_ge_u()  { emitByte(wop::i32_ge_u); }

	// Float ops
	void op_f32_add()   { emitByte(wop::f32_add); }
	void op_f32_sub()   { emitByte(wop::f32_sub); }
	void op_f32_mul()   { emitByte(wop::f32_mul); }
	void op_f32_div()   { emitByte(wop::f32_div); }
	void op_f32_abs()   { emitByte(wop::f32_abs); }
	void op_f32_neg()   { emitByte(wop::f32_neg); }
	void op_f32_sqrt()  { emitByte(wop::f32_sqrt); }
	void op_f32_eq()    { emitByte(wop::f32_eq); }
	void op_f32_gt()    { emitByte(wop::f32_gt); }

	// i64 arithmetic
	void op_i64_add()   { emitByte(wop::i64_add); }
	void op_i64_sub()   { emitByte(wop::i64_sub); }
	void op_i64_mul()   { emitByte(wop::i64_mul); }
	void op_i64_and()   { emitByte(wop::i64_and); }
	void op_i64_or()    { emitByte(wop::i64_or); }
	void op_i64_xor()   { emitByte(wop::i64_xor); }
	void op_i64_shl()   { emitByte(wop::i64_shl); }
	void op_i64_shr_s() { emitByte(wop::i64_shr_s); }
	void op_i64_shr_u() { emitByte(wop::i64_shr_u); }
	void op_i64_const(s64 val) {
		emitByte(wop::i64_const);
		// signed LEB128 for i64
		bool more = true;
		while (more) {
			u8 b = val & 0x7F;
			val >>= 7;
			if ((val == 0 && (b & 0x40) == 0) || (val == -1 && (b & 0x40) != 0))
				more = false;
			else
				b |= 0x80;
			bytes.push_back(b);
		}
	}

	// i64 conversions
	void op_i32_wrap_i64()       { emitByte(wop::i32_wrap_i64); }
	void op_i64_extend_i32_s()   { emitByte(wop::i64_extend_i32_s); }
	void op_i64_extend_i32_u()   { emitByte(wop::i64_extend_i32_u); }

	// Double (f64) ops — used for fipr/ftrv double-precision accumulation
	void op_f64_add()            { emitByte(wop::f64_add); }
	void op_f64_mul()            { emitByte(wop::f64_mul); }
	void op_f64_promote_f32()    { emitByte(wop::f64_promote_f32); }
	void op_f32_demote_f64()     { emitByte(wop::f32_demote_f64); }

	// Conversions
	void op_i32_trunc_f32_s()    { emitByte(wop::i32_trunc_f32_s); }
	void op_f32_convert_i32_s()  { emitByte(wop::f32_convert_i32_s); }
	void op_i32_reinterpret_f32() { emitByte(wop::i32_reinterpret_f32); }
	void op_f32_reinterpret_i32() { emitByte(wop::f32_reinterpret_i32); }

	// Control flow
	void op_call(u32 funcIdx) { emitByte(wop::call); emitLEB128(funcIdx); }
	void op_return()     { emitByte(wop::return_); }
	void op_drop()       { emitByte(wop::drop); }
	void op_select()     { emitByte(wop::select); }
	void op_unreachable() { emitByte(wop::unreachable); }

	void op_if(u8 blockType = 0x40) { emitByte(wop::if_); emitByte(blockType); }
	void op_else()       { emitByte(wop::else_); }
	void op_end()        { emitByte(wop::end); }
	void op_block(u8 blockType = 0x40) { emitByte(wop::block); emitByte(blockType); }
	void op_loop(u8 blockType = 0x40) { emitByte(wop::loop_); emitByte(blockType); }
	void op_br(u32 depth) { emitByte(wop::br); emitLEB128(depth); }
	void op_br_if(u32 depth) { emitByte(wop::br_if); emitLEB128(depth); }

	// --- Output ---

	const std::vector<u8>& getBytes() const { return bytes; }
	size_t size() const { return bytes.size(); }

private:
	std::vector<u8> bytes;
	u32 sectionSizePos = 0;
	u32 sectionContentStart = 0;
	u32 funcBodySizePos = 0;
	u32 funcBodyStart = 0;

	// Write a u32 as a 5-byte fixed-length LEB128 at a specific position
	void patchLEB128_5(u32 pos, u32 value) {
		bytes[pos + 0] = (value & 0x7F) | 0x80;
		bytes[pos + 1] = ((value >> 7) & 0x7F) | 0x80;
		bytes[pos + 2] = ((value >> 14) & 0x7F) | 0x80;
		bytes[pos + 3] = ((value >> 21) & 0x7F) | 0x80;
		bytes[pos + 4] = (value >> 28) & 0x0F;
	}
};
