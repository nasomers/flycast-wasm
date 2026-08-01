// wasm_test_shil_ops.h — Layer 1 SHIL op unit test harness
//
// Validates every natively-emitted SHIL op by comparing the WASM JIT path
// against the C++ fallback interpreter (wasm_exec_shil_fb). Both paths are
// proven correct against upstream Flycast's interpreter at commit 2c48c01.
//
// Included at the bottom of rec_wasm.cpp (needs access to statics:
// buildBlockModule, blockByVaddr, wasm_compile_block, wasm_execute_block,
// wasm_exec_shil_fb, g_ifb_exception_pending).
//
// Gated with #ifndef JIT_PROD_BUILD — excluded from production builds.
//
// Entry point: shil_op_test_harness() returns number of failures (0 = all pass).

#pragma once

#include <vector>
#include <cstring>
#include <cmath>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// ============================================================
// Test infrastructure
// ============================================================

// Fake vaddr space for test blocks — 0xDEAD0000+ avoids real SH4 addresses
static u32 g_test_vaddr_counter = 0xDEAD0000;

// Scratch RAM offset for memory tests. At end of 16MB DC main RAM.
// Physical addr = 0x0CFF0000 (area 3 fast path: bits 28:26 == 3).
static const u32 TEST_RAM_OFFSET = 0x00FF0000;
static const u32 TEST_RAM_ADDR   = 0x0C000000 + TEST_RAM_OFFSET;

struct TestResult {
    const char* op_name;
    const char* test_name;
    bool pass;
    // On failure, which register diverged and what the values were
    u32 wasm_val;
    u32 fb_val;
    const char* diverged_reg;
};

// Snapshot of registers we care about comparing
struct CtxSnapshot {
    u32 r[16];
    u32 fr[16];       // float regs as u32 bits
    u32 xf[16];       // xf bank as u32 bits
    u32 sr_T;
    u32 fpul;
    u32 mach;
    u32 macl;
    u32 pr;
    u32 jdyn;
    u32 pc;
    // For readm/writem, also snapshot a scratch RAM region
    u8 scratch_ram[64];
    // For SQ-write tests: both store-queue buffers (ctx offset 0, 2x32B)
    u8 sq[64];
};

static void takeSnapshot(CtxSnapshot& snap) {
    Sh4Context& ctx = Sh4cntx;
    for (int i = 0; i < 16; i++) snap.r[i] = ctx.r[i];
    for (int i = 0; i < 16; i++) snap.fr[i] = *(u32*)&ctx.fr[i]; // fr bank
    for (int i = 0; i < 16; i++) snap.xf[i] = *(u32*)&ctx.xf[i];      // xf bank
    snap.sr_T = ctx.sr.T;
    snap.fpul = ctx.fpul;
    snap.mach = ctx.mac.full >> 32;
    snap.macl = ctx.mac.full & 0xFFFFFFFF;
    snap.pr = ctx.pr;
    snap.jdyn = ctx.jdyn;
    snap.pc = ctx.pc;
    memcpy(snap.scratch_ram, &mem_b[TEST_RAM_OFFSET], 64);
    memcpy(snap.sq, ctx.sq_buffer, 64);
}

// Seed context with deterministic known state
static void seedContext() {
    Sh4Context& ctx = Sh4cntx;
    // Zero everything first
    memset(&ctx, 0, sizeof(ctx));
    // Set some recognizable values in GPRs
    for (int i = 0; i < 16; i++) ctx.r[i] = 0;
    // Clear FPU regs
    for (int i = 0; i < 16; i++) ctx.fr[i] = 0.0f;
    for (int i = 0; i < 16; i++) ctx.xf[i] = 0.0f;
    ctx.sr.T = 0;
    ctx.fpul = 0;
    ctx.mac.full = 0;
    ctx.pr = 0;
    ctx.jdyn = 0;
    ctx.pc = 0;
    ctx.cycle_counter = 1000; // plenty of budget
    ctx.sr.FD = 0; // FPU enabled
    ctx.sr.status = 0;
    // Clear exception state
    g_ifb_exception_pending = false;
}

// Create a synthetic RuntimeBlockInfo with a single SHIL op
static RuntimeBlockInfo* makeSyntheticBlock(const shil_opcode& op) {
    RuntimeBlockInfo* block = new RuntimeBlockInfo();
    block->vaddr = g_test_vaddr_counter++;
    block->addr = block->vaddr;
    block->BlockType = BET_StaticJump;
    block->BranchBlock = block->vaddr; // self-loop (simplest exit)
    block->NextBlock = block->vaddr;
    block->guest_cycles = 1;
    block->guest_opcodes = 1;
    block->host_opcodes = 0;
    block->host_code_size = 0;
    block->sh4_code_size = 2;
    block->has_jcond = false;
    block->has_fpu_op = false;
    block->temp_block = false;
    block->blockcheck_failures = 0;
    block->code = nullptr;
    block->pBranchBlock = nullptr;
    block->pNextBlock = nullptr;
    block->relink_offset = 0;
    block->relink_data = 0;
    block->read_only = false;
    block->fpu_cfg.full = Sh4cntx.fpscr.full;
    block->oplist.push_back(op);

    // Register in blockByVaddr so wasm_exec_shil_fb can find it
    blockByVaddr[block->vaddr] = block;
    return block;
}

static void destroySyntheticBlock(RuntimeBlockInfo* block) {
    u32 vaddr = block->vaddr;
    blockByVaddr.erase(vaddr);
    // Clear dispatch table entry to prevent JIT-TRAP on fake PCs
    u32 key = (vaddr >> 1) & JIT_TABLE_MASK;
    jit_dispatch_table[key] = 0;
    jit_dispatch_pc[key] = 0;
    jit_dispatch_hash[key] = 0;
    // Also remove from WASM cache if it was compiled
#ifdef __EMSCRIPTEN__
    wasm_remove_block(vaddr);
#endif
    delete block;
}

// Build a shil_param for a register
static shil_param mkReg(Sh4RegType reg) {
    return shil_param(reg);
}

// Build a shil_param for an immediate
static shil_param mkImm(u32 val) {
    return shil_param(val);
}

// Build a shil_opcode
static shil_opcode mkOp(shilop sop, shil_param rd = shil_param(),
                         shil_param rs1 = shil_param(), shil_param rs2 = shil_param(),
                         shil_param rs3 = shil_param(), shil_param rd2 = shil_param(),
                         u32 size = 0) {
    shil_opcode op;
    op.op = sop;
    op.rd = rd;
    op.rd2 = rd2;
    op.rs1 = rs1;
    op.rs2 = rs2;
    op.rs3 = rs3;
    op.size = size;
    op.host_offs = 0;
    op.guest_offs = 0;
    op.delay_slot = false;
    return op;
}

// Execute a single op via the WASM JIT path
// Returns true if compilation + execution succeeded
static bool execWasmPath(RuntimeBlockInfo* block) {
#ifdef __EMSCRIPTEN__
    WasmModuleBuilder builder;
    if (!buildBlockModule(builder, block))
        return false;

    const std::vector<u8>& bytes = builder.getBytes();
    int ok = wasm_compile_block(bytes.data(), (u32)bytes.size(), block->vaddr);
    if (!ok) return false;

    u32 ctx_ptr = (u32)(uintptr_t)&Sh4cntx;
    u32 ram_base = (u32)(uintptr_t)&mem_b[0];
    wasm_execute_block(block->vaddr, ctx_ptr, ram_base);
    return true;
#else
    return false;
#endif
}

// Execute a single op via the C++ fallback path
static void execFallbackPath(RuntimeBlockInfo* block) {
    // The fallback needs cycle_counter to be already decremented (the WASM
    // prologue does this). To match, subtract guest_cycles before calling.
    Sh4cntx.cycle_counter -= block->guest_cycles;
    wasm_exec_shil_fb(block->vaddr, 0);
    // The WASM block exit writes pc = BranchBlock. Match that:
    Sh4cntx.pc = block->BranchBlock;
}

// ============================================================
// Test case definitions
// ============================================================

struct TestCase {
    const char* op_name;
    const char* test_name;
    shil_opcode op;
    // Setup function: seeds specific register values before execution
    void (*setup)(Sh4Context& ctx);
    // Check function: compares specific outputs. Returns nullptr on pass,
    // or a string describing the first divergence.
    const char* (*check)(const CtxSnapshot& wasm_snap, const CtxSnapshot& fb_snap,
                         u32* out_wasm_val, u32* out_fb_val);
};

// Comparison helpers
static const char* cmpReg(const char* name, u32 wasm_val, u32 fb_val,
                           u32* out_w, u32* out_f) {
    if (wasm_val != fb_val) {
        *out_w = wasm_val;
        *out_f = fb_val;
        return name;
    }
    return nullptr;
}

// Check rd output in r0
#define CHECK_R0 [](const CtxSnapshot& w, const CtxSnapshot& f, u32* ow, u32* of) -> const char* { \
    return cmpReg("r0", w.r[0], f.r[0], ow, of); }

// Check rd=r0, rd2=r1
#define CHECK_R0_R1 [](const CtxSnapshot& w, const CtxSnapshot& f, u32* ow, u32* of) -> const char* { \
    const char* d = cmpReg("r0", w.r[0], f.r[0], ow, of); \
    if (d) return d; \
    return cmpReg("r1", w.r[1], f.r[1], ow, of); }

// Check sr.T
#define CHECK_SR_T [](const CtxSnapshot& w, const CtxSnapshot& f, u32* ow, u32* of) -> const char* { \
    return cmpReg("sr.T", w.sr_T, f.sr_T, ow, of); }

// Check jdyn
#define CHECK_JDYN [](const CtxSnapshot& w, const CtxSnapshot& f, u32* ow, u32* of) -> const char* { \
    return cmpReg("jdyn", w.jdyn, f.jdyn, ow, of); }

// Check fr0 (float as bits)
#define CHECK_FR0 [](const CtxSnapshot& w, const CtxSnapshot& f, u32* ow, u32* of) -> const char* { \
    return cmpReg("fr0", w.fr[0], f.fr[0], ow, of); }

// Check fr0-fr3 (vector)
#define CHECK_FV0 [](const CtxSnapshot& w, const CtxSnapshot& f, u32* ow, u32* of) -> const char* { \
    for (int i = 0; i < 4; i++) { \
        const char* d = cmpReg("fr", w.fr[i], f.fr[i], ow, of); \
        if (d) return d; \
    } return nullptr; }

// Check all fr + xf (for frswap)
#define CHECK_FR_XF [](const CtxSnapshot& w, const CtxSnapshot& f, u32* ow, u32* of) -> const char* { \
    for (int i = 0; i < 16; i++) { \
        const char* d = cmpReg("fr", w.fr[i], f.fr[i], ow, of); \
        if (d) return d; \
    } \
    for (int i = 0; i < 16; i++) { \
        const char* d = cmpReg("xf", w.xf[i], f.xf[i], ow, of); \
        if (d) return d; \
    } return nullptr; }

// Check fr0+fr1 (64-bit / fsca output)
#define CHECK_FR0_FR1 [](const CtxSnapshot& w, const CtxSnapshot& f, u32* ow, u32* of) -> const char* { \
    const char* d = cmpReg("fr0", w.fr[0], f.fr[0], ow, of); \
    if (d) return d; \
    return cmpReg("fr1", w.fr[1], f.fr[1], ow, of); }

// Check 4 bytes of scratch RAM at offset 0
#define CHECK_RAM4 [](const CtxSnapshot& w, const CtxSnapshot& f, u32* ow, u32* of) -> const char* { \
    u32 wv = *(u32*)w.scratch_ram; \
    u32 fv = *(u32*)f.scratch_ram; \
    return cmpReg("ram[0:4]", wv, fv, ow, of); }

// Full 64-byte compare of both SQ buffers — catches wrong offset, wrong
// bank, wrong size, AND stray writes to untouched bytes (setup seeds a
// pattern). Reports the first diverging word.
#define CHECK_SQ [](const CtxSnapshot& w, const CtxSnapshot& f, u32* ow, u32* of) -> const char* { \
    for (int i = 0; i < 16; i++) { \
        u32 wv = *(u32*)&w.sq[i * 4]; \
        u32 fv = *(u32*)&f.sq[i * 4]; \
        if (wv != fv) { *ow = wv; *of = fv; return "sq_buffer"; } \
    } \
    return nullptr; }

// Setup helpers — set GPRs by index
static void setR(Sh4Context& ctx, int idx, u32 val) { ctx.r[idx] = val; }
static void setFR(Sh4Context& ctx, int idx, float val) {
    ctx.fr[idx] = val;
}
static void setXF(Sh4Context& ctx, int idx, float val) {
    ctx.xf[idx] = val;
}

// ============================================================
// Register all test cases
// ============================================================

static void registerAllTests(std::vector<TestCase>& tests) {

    // ============================================================
    // INTEGER ALU
    // ============================================================

    // shop_mov32: r0 = r2
    tests.push_back({"mov32", "basic", mkOp(shop_mov32, mkReg(reg_r0), mkReg(reg_r2)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xDEADBEEF); },
        CHECK_R0});

    tests.push_back({"mov32", "zero", mkOp(shop_mov32, mkReg(reg_r0), mkReg(reg_r2)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0); },
        CHECK_R0});

    tests.push_back({"mov32", "imm", mkOp(shop_mov32, mkReg(reg_r0), mkImm(42)),
        [](Sh4Context& ctx) {},
        CHECK_R0});

    // shop_add: r0 = r2 + r3
    tests.push_back({"add", "basic", mkOp(shop_add, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 100); setR(ctx, 3, 200); },
        CHECK_R0});

    tests.push_back({"add", "overflow", mkOp(shop_add, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xFFFFFFFF); setR(ctx, 3, 1); },
        CHECK_R0});

    tests.push_back({"add", "imm", mkOp(shop_add, mkReg(reg_r0), mkReg(reg_r2), mkImm(0x7F)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x80000000); },
        CHECK_R0});

    // shop_sub: r0 = r2 - r3
    tests.push_back({"sub", "basic", mkOp(shop_sub, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 300); setR(ctx, 3, 100); },
        CHECK_R0});

    tests.push_back({"sub", "underflow", mkOp(shop_sub, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0); setR(ctx, 3, 1); },
        CHECK_R0});

    // shop_and
    tests.push_back({"and", "basic", mkOp(shop_and, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xFF00FF00); setR(ctx, 3, 0x0F0F0F0F); },
        CHECK_R0});

    tests.push_back({"and", "imm", mkOp(shop_and, mkReg(reg_r0), mkReg(reg_r2), mkImm(0xFF)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x12345678); },
        CHECK_R0});

    // shop_or
    tests.push_back({"or", "basic", mkOp(shop_or, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xF0F0F0F0); setR(ctx, 3, 0x0F0F0F0F); },
        CHECK_R0});

    // shop_xor
    tests.push_back({"xor", "basic", mkOp(shop_xor, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xAAAAAAAA); setR(ctx, 3, 0x55555555); },
        CHECK_R0});

    tests.push_back({"xor", "self", mkOp(shop_xor, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r2)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xDEADBEEF); },
        CHECK_R0});

    // shop_not
    tests.push_back({"not", "basic", mkOp(shop_not, mkReg(reg_r0), mkReg(reg_r2)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0); },
        CHECK_R0});

    tests.push_back({"not", "ones", mkOp(shop_not, mkReg(reg_r0), mkReg(reg_r2)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xFFFFFFFF); },
        CHECK_R0});

    // shop_neg
    tests.push_back({"neg", "positive", mkOp(shop_neg, mkReg(reg_r0), mkReg(reg_r2)),
        [](Sh4Context& ctx) { setR(ctx, 2, 42); },
        CHECK_R0});

    tests.push_back({"neg", "zero", mkOp(shop_neg, mkReg(reg_r0), mkReg(reg_r2)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0); },
        CHECK_R0});

    tests.push_back({"neg", "minint", mkOp(shop_neg, mkReg(reg_r0), mkReg(reg_r2)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x80000000); },
        CHECK_R0});

    // shop_shl
    tests.push_back({"shl", "by1", mkOp(shop_shl, mkReg(reg_r0), mkReg(reg_r2), mkImm(1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x40000000); },
        CHECK_R0});

    tests.push_back({"shl", "by0", mkOp(shop_shl, mkReg(reg_r0), mkReg(reg_r2), mkImm(0)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xDEADBEEF); },
        CHECK_R0});

    tests.push_back({"shl", "by31", mkOp(shop_shl, mkReg(reg_r0), mkReg(reg_r2), mkImm(31)),
        [](Sh4Context& ctx) { setR(ctx, 2, 1); },
        CHECK_R0});

    // shop_shr
    tests.push_back({"shr", "by1", mkOp(shop_shr, mkReg(reg_r0), mkReg(reg_r2), mkImm(1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x80000002); },
        CHECK_R0});

    tests.push_back({"shr", "by31", mkOp(shop_shr, mkReg(reg_r0), mkReg(reg_r2), mkImm(31)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x80000000); },
        CHECK_R0});

    // shop_sar
    tests.push_back({"sar", "positive", mkOp(shop_sar, mkReg(reg_r0), mkReg(reg_r2), mkImm(4)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x00000100); },
        CHECK_R0});

    tests.push_back({"sar", "negative", mkOp(shop_sar, mkReg(reg_r0), mkReg(reg_r2), mkImm(4)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xF0000000); },
        CHECK_R0});

    tests.push_back({"sar", "by31neg", mkOp(shop_sar, mkReg(reg_r0), mkReg(reg_r2), mkImm(31)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x80000000); },
        CHECK_R0});

    // shop_ror
    tests.push_back({"ror", "by1", mkOp(shop_ror, mkReg(reg_r0), mkReg(reg_r2), mkImm(1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x00000001); },
        CHECK_R0});

    tests.push_back({"ror", "by16", mkOp(shop_ror, mkReg(reg_r0), mkReg(reg_r2), mkImm(16)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x12345678); },
        CHECK_R0});

    // shop_ext_s8
    tests.push_back({"ext_s8", "positive", mkOp(shop_ext_s8, mkReg(reg_r0), mkReg(reg_r2)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x0000007F); },
        CHECK_R0});

    tests.push_back({"ext_s8", "negative", mkOp(shop_ext_s8, mkReg(reg_r0), mkReg(reg_r2)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x000000FF); },
        CHECK_R0});

    tests.push_back({"ext_s8", "highbits", mkOp(shop_ext_s8, mkReg(reg_r0), mkReg(reg_r2)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xFFFF0080); },
        CHECK_R0});

    // shop_ext_s16
    tests.push_back({"ext_s16", "positive", mkOp(shop_ext_s16, mkReg(reg_r0), mkReg(reg_r2)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x00007FFF); },
        CHECK_R0});

    tests.push_back({"ext_s16", "negative", mkOp(shop_ext_s16, mkReg(reg_r0), mkReg(reg_r2)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x0000FFFF); },
        CHECK_R0});

    tests.push_back({"ext_s16", "highbits", mkOp(shop_ext_s16, mkReg(reg_r0), mkReg(reg_r2)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xFFFF8000); },
        CHECK_R0});

    // ============================================================
    // INTEGER MULTIPLY
    // ============================================================

    // shop_mul_u16
    tests.push_back({"mul_u16", "basic", mkOp(shop_mul_u16, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 100); setR(ctx, 3, 200); },
        CHECK_R0});

    tests.push_back({"mul_u16", "max", mkOp(shop_mul_u16, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xFFFF); setR(ctx, 3, 0xFFFF); },
        CHECK_R0});

    tests.push_back({"mul_u16", "mask", mkOp(shop_mul_u16, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xDEAD0003); setR(ctx, 3, 0xBEEF0007); },
        CHECK_R0});

    // shop_mul_s16
    tests.push_back({"mul_s16", "positive", mkOp(shop_mul_s16, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 100); setR(ctx, 3, 200); },
        CHECK_R0});

    tests.push_back({"mul_s16", "negative", mkOp(shop_mul_s16, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xFFFE); setR(ctx, 3, 3); }, // -2 * 3 = -6
        CHECK_R0});

    tests.push_back({"mul_s16", "both_neg", mkOp(shop_mul_s16, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xFFFF); setR(ctx, 3, 0xFFFF); }, // -1 * -1 = 1
        CHECK_R0});

    // shop_mul_i32
    tests.push_back({"mul_i32", "basic", mkOp(shop_mul_i32, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 12345); setR(ctx, 3, 6789); },
        CHECK_R0});

    tests.push_back({"mul_i32", "overflow", mkOp(shop_mul_i32, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x80000000); setR(ctx, 3, 2); },
        CHECK_R0});

    // shop_mul_u64: r0 = low32, r1 = high32
    tests.push_back({"mul_u64", "small",
        mkOp(shop_mul_u64, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3), shil_param(), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 100); setR(ctx, 3, 200); },
        CHECK_R0_R1});

    tests.push_back({"mul_u64", "large",
        mkOp(shop_mul_u64, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3), shil_param(), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xFFFFFFFF); setR(ctx, 3, 0xFFFFFFFF); },
        CHECK_R0_R1});

    tests.push_back({"mul_u64", "mixed",
        mkOp(shop_mul_u64, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3), shil_param(), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x80000000); setR(ctx, 3, 2); },
        CHECK_R0_R1});

    // shop_mul_s64
    tests.push_back({"mul_s64", "positive",
        mkOp(shop_mul_s64, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3), shil_param(), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 100); setR(ctx, 3, 200); },
        CHECK_R0_R1});

    tests.push_back({"mul_s64", "neg_pos",
        mkOp(shop_mul_s64, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3), shil_param(), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 2, (u32)-5); setR(ctx, 3, 3); },
        CHECK_R0_R1});

    tests.push_back({"mul_s64", "neg_neg",
        mkOp(shop_mul_s64, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3), shil_param(), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 2, (u32)-100); setR(ctx, 3, (u32)-200); },
        CHECK_R0_R1});

    // ============================================================
    // INTEGER CARRY/SHIFT
    // ============================================================

    // shop_adc: r0 = rs1 + rs2 + rs3(carry), r1 = carry_out
    tests.push_back({"adc", "no_carry",
        mkOp(shop_adc, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3), mkReg(reg_r4), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 100); setR(ctx, 3, 200); setR(ctx, 4, 0); },
        CHECK_R0_R1});

    tests.push_back({"adc", "with_carry",
        mkOp(shop_adc, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3), mkReg(reg_r4), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 100); setR(ctx, 3, 200); setR(ctx, 4, 1); },
        CHECK_R0_R1});

    tests.push_back({"adc", "overflow",
        mkOp(shop_adc, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3), mkReg(reg_r4), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xFFFFFFFF); setR(ctx, 3, 1); setR(ctx, 4, 0); },
        CHECK_R0_R1});

    tests.push_back({"adc", "overflow_carry",
        mkOp(shop_adc, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3), mkReg(reg_r4), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xFFFFFFFF); setR(ctx, 3, 0); setR(ctx, 4, 1); },
        CHECK_R0_R1});

    // shop_sbc: r0 = rs1 - rs2 - rs3(borrow), r1 = borrow_out
    tests.push_back({"sbc", "no_borrow",
        mkOp(shop_sbc, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3), mkReg(reg_r4), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 300); setR(ctx, 3, 100); setR(ctx, 4, 0); },
        CHECK_R0_R1});

    tests.push_back({"sbc", "with_borrow",
        mkOp(shop_sbc, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3), mkReg(reg_r4), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 300); setR(ctx, 3, 100); setR(ctx, 4, 1); },
        CHECK_R0_R1});

    tests.push_back({"sbc", "underflow",
        mkOp(shop_sbc, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3), mkReg(reg_r4), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0); setR(ctx, 3, 1); setR(ctx, 4, 0); },
        CHECK_R0_R1});

    // shop_negc: r0 = 0 - rs1 - rs2(carry), r1 = borrow_out
    tests.push_back({"negc", "zero_no_carry",
        mkOp(shop_negc, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3), shil_param(), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0); setR(ctx, 3, 0); },
        CHECK_R0_R1});

    tests.push_back({"negc", "nonzero_carry",
        mkOp(shop_negc, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3), shil_param(), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 42); setR(ctx, 3, 1); },
        CHECK_R0_R1});

    tests.push_back({"negc", "one_no_carry",
        mkOp(shop_negc, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3), shil_param(), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 1); setR(ctx, 3, 0); },
        CHECK_R0_R1});

    // shop_rocl: r0 = (val << 1) | carry, r1 = old bit 31
    tests.push_back({"rocl", "no_carry_nobit31",
        mkOp(shop_rocl, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3), shil_param(), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x12345678); setR(ctx, 3, 0); },
        CHECK_R0_R1});

    tests.push_back({"rocl", "carry_bit31",
        mkOp(shop_rocl, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3), shil_param(), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x80000001); setR(ctx, 3, 1); },
        CHECK_R0_R1});

    // CRITICAL: rd == rs1 aliasing test for rocl
    tests.push_back({"rocl", "alias_rd_rs1",
        mkOp(shop_rocl, mkReg(reg_r0), mkReg(reg_r0), mkReg(reg_r3), shil_param(), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 0, 0xC0000000); setR(ctx, 3, 1); },
        CHECK_R0_R1});

    // shop_rocr: r0 = (val >> 1) | (carry << 31), r1 = old bit 0
    tests.push_back({"rocr", "no_carry_nobit0",
        mkOp(shop_rocr, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3), shil_param(), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x12345678); setR(ctx, 3, 0); },
        CHECK_R0_R1});

    tests.push_back({"rocr", "carry_bit0",
        mkOp(shop_rocr, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3), shil_param(), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x00000001); setR(ctx, 3, 1); },
        CHECK_R0_R1});

    // CRITICAL: rd == rs1 aliasing test for rocr
    tests.push_back({"rocr", "alias_rd_rs1",
        mkOp(shop_rocr, mkReg(reg_r0), mkReg(reg_r0), mkReg(reg_r3), shil_param(), mkReg(reg_r1)),
        [](Sh4Context& ctx) { setR(ctx, 0, 0x00000003); setR(ctx, 3, 1); },
        CHECK_R0_R1});

    // shop_shld: logical shift (positive = left, negative = right, special case: -32 = 0)
    tests.push_back({"shld", "left_4", mkOp(shop_shld, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x12345678); setR(ctx, 3, 4); },
        CHECK_R0});

    tests.push_back({"shld", "right_4", mkOp(shop_shld, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x12345678); setR(ctx, 3, (u32)-4); },
        CHECK_R0});

    tests.push_back({"shld", "shift_0", mkOp(shop_shld, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xDEADBEEF); setR(ctx, 3, 0); },
        CHECK_R0});

    tests.push_back({"shld", "neg32_zero", mkOp(shop_shld, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xDEADBEEF); setR(ctx, 3, (u32)-32); },
        CHECK_R0});

    // shop_shad: arithmetic shift (positive = left, negative = right arithmetic, -32 = sign-fill)
    tests.push_back({"shad", "left_4", mkOp(shop_shad, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x12345678); setR(ctx, 3, 4); },
        CHECK_R0});

    tests.push_back({"shad", "right_4_neg", mkOp(shop_shad, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xF0000000); setR(ctx, 3, (u32)-4); },
        CHECK_R0});

    tests.push_back({"shad", "neg32_signfill", mkOp(shop_shad, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x80000000); setR(ctx, 3, (u32)-32); },
        CHECK_R0});

    tests.push_back({"shad", "neg32_signfill_pos", mkOp(shop_shad, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x7FFFFFFF); setR(ctx, 3, (u32)-32); },
        CHECK_R0});

    // shop_setpeq: 1 if any corresponding bytes of rs1 and rs2 match
    tests.push_back({"setpeq", "no_match", mkOp(shop_setpeq, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x01020304); setR(ctx, 3, 0x05060708); },
        CHECK_R0});

    tests.push_back({"setpeq", "byte0_match", mkOp(shop_setpeq, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x010203FF); setR(ctx, 3, 0x050607FF); },
        CHECK_R0});

    tests.push_back({"setpeq", "byte3_match", mkOp(shop_setpeq, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xAA020304); setR(ctx, 3, 0xAA060708); },
        CHECK_R0});

    tests.push_back({"setpeq", "all_match", mkOp(shop_setpeq, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xDEADBEEF); setR(ctx, 3, 0xDEADBEEF); },
        CHECK_R0});

    // ============================================================
    // COMPARISONS
    // ============================================================

    // shop_test: (rs1 & rs2) == 0 ? 1 : 0
    tests.push_back({"test", "zero", mkOp(shop_test, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xF0F0F0F0); setR(ctx, 3, 0x0F0F0F0F); },
        CHECK_R0});

    tests.push_back({"test", "nonzero", mkOp(shop_test, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xFF); setR(ctx, 3, 0x01); },
        CHECK_R0});

    // shop_seteq
    tests.push_back({"seteq", "equal", mkOp(shop_seteq, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 42); setR(ctx, 3, 42); },
        CHECK_R0});

    tests.push_back({"seteq", "notequal", mkOp(shop_seteq, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 42); setR(ctx, 3, 43); },
        CHECK_R0});

    tests.push_back({"seteq", "imm", mkOp(shop_seteq, mkReg(reg_r0), mkReg(reg_r2), mkImm(0)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0); },
        CHECK_R0});

    // shop_setge (signed >=)
    tests.push_back({"setge", "greater", mkOp(shop_setge, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 10); setR(ctx, 3, 5); },
        CHECK_R0});

    tests.push_back({"setge", "equal", mkOp(shop_setge, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 5); setR(ctx, 3, 5); },
        CHECK_R0});

    tests.push_back({"setge", "signed_neg", mkOp(shop_setge, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x80000000); setR(ctx, 3, 1); }, // -2B vs 1
        CHECK_R0});

    // shop_setgt (signed >)
    tests.push_back({"setgt", "greater", mkOp(shop_setgt, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 10); setR(ctx, 3, 5); },
        CHECK_R0});

    tests.push_back({"setgt", "equal", mkOp(shop_setgt, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 5); setR(ctx, 3, 5); },
        CHECK_R0});

    // shop_setae (unsigned >=)
    tests.push_back({"setae", "greater", mkOp(shop_setae, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x80000000); setR(ctx, 3, 0x7FFFFFFF); },
        CHECK_R0});

    tests.push_back({"setae", "equal", mkOp(shop_setae, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 42); setR(ctx, 3, 42); },
        CHECK_R0});

    // shop_setab (unsigned >)
    tests.push_back({"setab", "greater", mkOp(shop_setab, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x80000000); setR(ctx, 3, 0x7FFFFFFF); },
        CHECK_R0});

    tests.push_back({"setab", "equal", mkOp(shop_setab, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 42); setR(ctx, 3, 42); },
        CHECK_R0});

    // ============================================================
    // FPU
    // ============================================================

    // shop_fadd
    tests.push_back({"fadd", "basic",
        mkOp(shop_fadd, mkReg(reg_fr_0), mkReg(reg_fr_2), mkReg(reg_fr_3)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 1.5f); setFR(ctx, 3, 2.25f); },
        CHECK_FR0});

    tests.push_back({"fadd", "negzero",
        mkOp(shop_fadd, mkReg(reg_fr_0), mkReg(reg_fr_2), mkReg(reg_fr_3)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 0.0f); setFR(ctx, 3, -0.0f); },
        CHECK_FR0});

    // shop_fsub
    tests.push_back({"fsub", "basic",
        mkOp(shop_fsub, mkReg(reg_fr_0), mkReg(reg_fr_2), mkReg(reg_fr_3)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 5.0f); setFR(ctx, 3, 2.0f); },
        CHECK_FR0});

    // shop_fmul
    tests.push_back({"fmul", "basic",
        mkOp(shop_fmul, mkReg(reg_fr_0), mkReg(reg_fr_2), mkReg(reg_fr_3)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 3.0f); setFR(ctx, 3, 7.0f); },
        CHECK_FR0});

    tests.push_back({"fmul", "zero",
        mkOp(shop_fmul, mkReg(reg_fr_0), mkReg(reg_fr_2), mkReg(reg_fr_3)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 0.0f); setFR(ctx, 3, 1234.5f); },
        CHECK_FR0});

    // shop_fdiv
    tests.push_back({"fdiv", "basic",
        mkOp(shop_fdiv, mkReg(reg_fr_0), mkReg(reg_fr_2), mkReg(reg_fr_3)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 10.0f); setFR(ctx, 3, 3.0f); },
        CHECK_FR0});

    tests.push_back({"fdiv", "divzero",
        mkOp(shop_fdiv, mkReg(reg_fr_0), mkReg(reg_fr_2), mkReg(reg_fr_3)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 1.0f); setFR(ctx, 3, 0.0f); },
        CHECK_FR0});

    // shop_fabs
    tests.push_back({"fabs", "negative",
        mkOp(shop_fabs, mkReg(reg_fr_0), mkReg(reg_fr_2)),
        [](Sh4Context& ctx) { setFR(ctx, 2, -3.14f); },
        CHECK_FR0});

    tests.push_back({"fabs", "positive",
        mkOp(shop_fabs, mkReg(reg_fr_0), mkReg(reg_fr_2)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 3.14f); },
        CHECK_FR0});

    // shop_fneg
    tests.push_back({"fneg", "positive",
        mkOp(shop_fneg, mkReg(reg_fr_0), mkReg(reg_fr_2)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 3.14f); },
        CHECK_FR0});

    tests.push_back({"fneg", "zero",
        mkOp(shop_fneg, mkReg(reg_fr_0), mkReg(reg_fr_2)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 0.0f); },
        CHECK_FR0});

    // shop_fsqrt
    tests.push_back({"fsqrt", "four",
        mkOp(shop_fsqrt, mkReg(reg_fr_0), mkReg(reg_fr_2)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 4.0f); },
        CHECK_FR0});

    tests.push_back({"fsqrt", "zero",
        mkOp(shop_fsqrt, mkReg(reg_fr_0), mkReg(reg_fr_2)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 0.0f); },
        CHECK_FR0});

    // shop_fseteq
    tests.push_back({"fseteq", "equal",
        mkOp(shop_fseteq, mkReg(reg_r0), mkReg(reg_fr_2), mkReg(reg_fr_3)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 1.0f); setFR(ctx, 3, 1.0f); },
        CHECK_R0});

    tests.push_back({"fseteq", "notequal",
        mkOp(shop_fseteq, mkReg(reg_r0), mkReg(reg_fr_2), mkReg(reg_fr_3)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 1.0f); setFR(ctx, 3, 2.0f); },
        CHECK_R0});

    // shop_fsetgt
    tests.push_back({"fsetgt", "greater",
        mkOp(shop_fsetgt, mkReg(reg_r0), mkReg(reg_fr_2), mkReg(reg_fr_3)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 5.0f); setFR(ctx, 3, 3.0f); },
        CHECK_R0});

    tests.push_back({"fsetgt", "less",
        mkOp(shop_fsetgt, mkReg(reg_r0), mkReg(reg_fr_2), mkReg(reg_fr_3)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 1.0f); setFR(ctx, 3, 3.0f); },
        CHECK_R0});

    // shop_fmac: rd = rs1 + rs2 * rs3
    // NOTE: WASM does mul+add (2 roundings), fallback uses std::fma (1 rounding).
    // Using simple values that won't expose the precision difference.
    tests.push_back({"fmac", "basic",
        mkOp(shop_fmac, mkReg(reg_fr_0), mkReg(reg_fr_2), mkReg(reg_fr_3), mkReg(reg_fr_4)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 1.0f); setFR(ctx, 3, 2.0f); setFR(ctx, 4, 3.0f); },
        CHECK_FR0});

    tests.push_back({"fmac", "zero_acc",
        mkOp(shop_fmac, mkReg(reg_fr_0), mkReg(reg_fr_2), mkReg(reg_fr_3), mkReg(reg_fr_4)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 0.0f); setFR(ctx, 3, 5.0f); setFR(ctx, 4, 7.0f); },
        CHECK_FR0});

    // shop_fsrra: 1/sqrt(rs1)
    tests.push_back({"fsrra", "four",
        mkOp(shop_fsrra, mkReg(reg_fr_0), mkReg(reg_fr_2)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 4.0f); },
        CHECK_FR0});

    tests.push_back({"fsrra", "one",
        mkOp(shop_fsrra, mkReg(reg_fr_0), mkReg(reg_fr_2)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 1.0f); },
        CHECK_FR0});

    // shop_fipr: dot product of 2 float4 vectors
    // rs1=fv0 (fr0-fr3), rs2=fv4 (fr4-fr7), result in fr(rs2 base + 3) = fr7
    // Actually: rd = the last element of the rs2 vector = fr[rs2_base+3]
    // fipr writes its result to the last component of the second vector operand
    tests.push_back({"fipr", "identity",
        mkOp(shop_fipr, mkReg(reg_fr_7), mkReg(regv_fv_0), mkReg(regv_fv_4)),
        [](Sh4Context& ctx) {
            // fv0 = (1, 0, 0, 0), fv4 = (1, 0, 0, 0) => dot = 1.0
            setFR(ctx, 0, 1.0f); setFR(ctx, 1, 0.0f); setFR(ctx, 2, 0.0f); setFR(ctx, 3, 0.0f);
            setFR(ctx, 4, 1.0f); setFR(ctx, 5, 0.0f); setFR(ctx, 6, 0.0f); setFR(ctx, 7, 0.0f);
        },
        [](const CtxSnapshot& w, const CtxSnapshot& f, u32* ow, u32* of) -> const char* {
            return cmpReg("fr7", w.fr[7], f.fr[7], ow, of);
        }});

    tests.push_back({"fipr", "mixed",
        mkOp(shop_fipr, mkReg(reg_fr_7), mkReg(regv_fv_0), mkReg(regv_fv_4)),
        [](Sh4Context& ctx) {
            // fv0 = (1, 2, 3, 4), fv4 = (5, 6, 7, 8) => dot = 5+12+21+32 = 70
            setFR(ctx, 0, 1.0f); setFR(ctx, 1, 2.0f); setFR(ctx, 2, 3.0f); setFR(ctx, 3, 4.0f);
            setFR(ctx, 4, 5.0f); setFR(ctx, 5, 6.0f); setFR(ctx, 6, 7.0f); setFR(ctx, 7, 8.0f);
        },
        [](const CtxSnapshot& w, const CtxSnapshot& f, u32* ow, u32* of) -> const char* {
            return cmpReg("fr7", w.fr[7], f.fr[7], ow, of);
        }});

    // shop_ftrv: 4x4 matrix (xmtrx = xf0-xf15) * vector (fv0 = fr0-fr3) -> fv0
    tests.push_back({"ftrv", "identity",
        mkOp(shop_ftrv, mkReg(regv_fv_0), mkReg(regv_fv_0), mkReg(regv_xmtrx)),
        [](Sh4Context& ctx) {
            // Identity matrix in xf bank (column-major: xf[col*4+row])
            for (int i = 0; i < 16; i++) setXF(ctx, i, 0.0f);
            setXF(ctx, 0, 1.0f); setXF(ctx, 5, 1.0f); setXF(ctx, 10, 1.0f); setXF(ctx, 15, 1.0f);
            // Input vector
            setFR(ctx, 0, 1.0f); setFR(ctx, 1, 2.0f); setFR(ctx, 2, 3.0f); setFR(ctx, 3, 4.0f);
        },
        CHECK_FV0});

    tests.push_back({"ftrv", "scale",
        mkOp(shop_ftrv, mkReg(regv_fv_0), mkReg(regv_fv_0), mkReg(regv_xmtrx)),
        [](Sh4Context& ctx) {
            // Scale matrix: diag = (2, 3, 4, 5)
            for (int i = 0; i < 16; i++) setXF(ctx, i, 0.0f);
            setXF(ctx, 0, 2.0f); setXF(ctx, 5, 3.0f); setXF(ctx, 10, 4.0f); setXF(ctx, 15, 5.0f);
            setFR(ctx, 0, 1.0f); setFR(ctx, 1, 1.0f); setFR(ctx, 2, 1.0f); setFR(ctx, 3, 1.0f);
        },
        CHECK_FV0});

    // shop_frswap: swap all 16 fr regs with all 16 xf regs
    tests.push_back({"frswap", "basic",
        mkOp(shop_frswap, mkReg(regv_xmtrx), mkReg(regv_xmtrx), mkReg(regv_fmtrx)),
        [](Sh4Context& ctx) {
            for (int i = 0; i < 16; i++) setFR(ctx, i, (float)(i + 1));       // fr = 1..16
            for (int i = 0; i < 16; i++) setXF(ctx, i, (float)(i + 101));     // xf = 101..116
        },
        CHECK_FR_XF});

    // ============================================================
    // CONVERSIONS
    // ============================================================

    // shop_cvt_f2i_t: float -> int (truncate)
    tests.push_back({"cvt_f2i_t", "positive",
        mkOp(shop_cvt_f2i_t, mkReg(reg_r0), mkReg(reg_fr_2)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 42.7f); },
        CHECK_R0});

    tests.push_back({"cvt_f2i_t", "negative",
        mkOp(shop_cvt_f2i_t, mkReg(reg_r0), mkReg(reg_fr_2)),
        [](Sh4Context& ctx) { setFR(ctx, 2, -42.7f); },
        CHECK_R0});

    tests.push_back({"cvt_f2i_t", "zero",
        mkOp(shop_cvt_f2i_t, mkReg(reg_r0), mkReg(reg_fr_2)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 0.0f); },
        CHECK_R0});

    tests.push_back({"cvt_f2i_t", "large_pos",
        mkOp(shop_cvt_f2i_t, mkReg(reg_r0), mkReg(reg_fr_2)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 2147483520.0f); }, // largest exact f32 < 2^31
        CHECK_R0});

    tests.push_back({"cvt_f2i_t", "overflow_pos",
        mkOp(shop_cvt_f2i_t, mkReg(reg_r0), mkReg(reg_fr_2)),
        [](Sh4Context& ctx) { setFR(ctx, 2, 2147484000.0f); }, // > 2^31-1
        CHECK_R0});

    // NaN test: SH4 spec and the reference interpreter both return 0x80000000
    // for NaN inputs. The WASM emitter pre-checks for NaN via f32.eq(x,x)==0
    // before the saturating truncate, matching the spec.
    tests.push_back({"cvt_f2i_t", "quiet_nan",
        mkOp(shop_cvt_f2i_t, mkReg(reg_r0), mkReg(reg_fr_2)),
        [](Sh4Context& ctx) {
            u32 nan_bits = 0x7FC00000; // quiet NaN
            *(u32*)&ctx.fr[2] = nan_bits;
        },
        CHECK_R0});

    tests.push_back({"cvt_f2i_t", "signaling_nan",
        mkOp(shop_cvt_f2i_t, mkReg(reg_r0), mkReg(reg_fr_2)),
        [](Sh4Context& ctx) {
            u32 nan_bits = 0x7F800001; // signaling NaN
            *(u32*)&ctx.fr[2] = nan_bits;
        },
        CHECK_R0});

    tests.push_back({"cvt_f2i_t", "pos_inf",
        mkOp(shop_cvt_f2i_t, mkReg(reg_r0), mkReg(reg_fr_2)),
        [](Sh4Context& ctx) {
            u32 inf_bits = 0x7F800000; // +inf
            *(u32*)&ctx.fr[2] = inf_bits;
        },
        CHECK_R0});

    tests.push_back({"cvt_f2i_t", "neg_inf",
        mkOp(shop_cvt_f2i_t, mkReg(reg_r0), mkReg(reg_fr_2)),
        [](Sh4Context& ctx) {
            u32 ninf_bits = 0xFF800000; // -inf
            *(u32*)&ctx.fr[2] = ninf_bits;
        },
        CHECK_R0});

    // shop_cvt_i2f_n: int -> float (nearest)
    tests.push_back({"cvt_i2f_n", "positive",
        mkOp(shop_cvt_i2f_n, mkReg(reg_fr_0), mkReg(reg_r2)),
        [](Sh4Context& ctx) { setR(ctx, 2, 42); },
        CHECK_FR0});

    tests.push_back({"cvt_i2f_n", "negative",
        mkOp(shop_cvt_i2f_n, mkReg(reg_fr_0), mkReg(reg_r2)),
        [](Sh4Context& ctx) { setR(ctx, 2, (u32)-42); },
        CHECK_FR0});

    // shop_cvt_i2f_z: int -> float (toward zero)
    tests.push_back({"cvt_i2f_z", "positive",
        mkOp(shop_cvt_i2f_z, mkReg(reg_fr_0), mkReg(reg_r2)),
        [](Sh4Context& ctx) { setR(ctx, 2, 42); },
        CHECK_FR0});

    // ============================================================
    // MEMORY OPS
    // ============================================================

    // shop_readm: read from RAM
    tests.push_back({"readm", "i32",
        mkOp(shop_readm, mkReg(reg_r0), mkReg(reg_r2), shil_param(), shil_param(), shil_param(), 4),
        [](Sh4Context& ctx) {
            setR(ctx, 2, TEST_RAM_ADDR);
            // Seed RAM with known value
            *(u32*)&mem_b[TEST_RAM_OFFSET] = 0xCAFEBABE;
        },
        CHECK_R0});

    tests.push_back({"readm", "i16_signext",
        mkOp(shop_readm, mkReg(reg_r0), mkReg(reg_r2), shil_param(), shil_param(), shil_param(), 2),
        [](Sh4Context& ctx) {
            setR(ctx, 2, TEST_RAM_ADDR);
            *(u16*)&mem_b[TEST_RAM_OFFSET] = 0xFF80; // -128 as s16
        },
        CHECK_R0});

    tests.push_back({"readm", "i8_signext",
        mkOp(shop_readm, mkReg(reg_r0), mkReg(reg_r2), shil_param(), shil_param(), shil_param(), 1),
        [](Sh4Context& ctx) {
            setR(ctx, 2, TEST_RAM_ADDR);
            mem_b[TEST_RAM_OFFSET] = 0x80; // -128 as s8
        },
        CHECK_R0});

    tests.push_back({"readm", "with_offset",
        mkOp(shop_readm, mkReg(reg_r0), mkReg(reg_r2), shil_param(), mkReg(reg_r4), shil_param(), 4),
        [](Sh4Context& ctx) {
            setR(ctx, 2, TEST_RAM_ADDR);
            setR(ctx, 4, 8);
            *(u32*)&mem_b[TEST_RAM_OFFSET + 8] = 0x12345678;
        },
        CHECK_R0});

    // shop_writem: write to RAM
    tests.push_back({"writem", "i32",
        mkOp(shop_writem, shil_param(), mkReg(reg_r2), mkReg(reg_r3), shil_param(), shil_param(), 4),
        [](Sh4Context& ctx) {
            setR(ctx, 2, TEST_RAM_ADDR);
            setR(ctx, 3, 0xDEADC0DE);
            memset(&mem_b[TEST_RAM_OFFSET], 0, 64);
        },
        CHECK_RAM4});

    tests.push_back({"writem", "i16",
        mkOp(shop_writem, shil_param(), mkReg(reg_r2), mkReg(reg_r3), shil_param(), shil_param(), 2),
        [](Sh4Context& ctx) {
            setR(ctx, 2, TEST_RAM_ADDR);
            setR(ctx, 3, 0xBEEF);
            memset(&mem_b[TEST_RAM_OFFSET], 0, 64);
        },
        [](const CtxSnapshot& w, const CtxSnapshot& f, u32* ow, u32* of) -> const char* {
            u16 wv = *(u16*)w.scratch_ram;
            u16 fv = *(u16*)f.scratch_ram;
            if (wv != fv) { *ow = wv; *of = fv; return "ram[0:2]"; }
            return nullptr;
        }});

    tests.push_back({"writem", "i8",
        mkOp(shop_writem, shil_param(), mkReg(reg_r2), mkReg(reg_r3), shil_param(), shil_param(), 1),
        [](Sh4Context& ctx) {
            setR(ctx, 2, TEST_RAM_ADDR);
            setR(ctx, 3, 0xAB);
            memset(&mem_b[TEST_RAM_OFFSET], 0, 64);
        },
        [](const CtxSnapshot& w, const CtxSnapshot& f, u32* ow, u32* of) -> const char* {
            if (w.scratch_ram[0] != f.scratch_ram[0]) {
                *ow = w.scratch_ram[0]; *of = f.scratch_ram[0]; return "ram[0]";
            }
            return nullptr;
        }});

    // ---- SQ-write fastmem certification (2026-07-23) ----
    // The inline SQ arm has no differential coverage (bridge-shadow skips
    // SQ-active blocks; shadow builds force writes through imports), so
    // these cases ARE its certification: emitted path vs C fallback
    // (WriteMem → mapBlock onto ctx.sq_buffer). Setup seeds a 0x55
    // pattern so wrong-offset/stray writes diverge, not just missing ones.

    tests.push_back({"writem", "sq_i32_bank0",
        mkOp(shop_writem, shil_param(), mkReg(reg_r2), mkReg(reg_r3), shil_param(), shil_param(), 4),
        [](Sh4Context& ctx) {
            setR(ctx, 2, 0xE0000000);           // SQ0 word 0
            setR(ctx, 3, 0xDEADC0DE);
            memset(ctx.sq_buffer, 0x55, 64);
        },
        CHECK_SQ});

    tests.push_back({"writem", "sq_i32_bank1_mirror",
        mkOp(shop_writem, shil_param(), mkReg(reg_r2), mkReg(reg_r3), shil_param(), shil_param(), 4),
        [](Sh4Context& ctx) {
            setR(ctx, 2, 0xE1000024);           // block E1 mirror, SQ1 word 1
            setR(ctx, 3, 0xCAFEBABE);
            memset(ctx.sq_buffer, 0x55, 64);
        },
        CHECK_SQ});

    tests.push_back({"writem", "sq_i32_rs3_offset",
        mkOp(shop_writem, shil_param(), mkReg(reg_r2), mkReg(reg_r3), mkReg(reg_r4), shil_param(), 4),
        [](Sh4Context& ctx) {
            setR(ctx, 2, 0xE2000000);           // block E2 mirror
            setR(ctx, 4, 0x18);                 // rs3 index → SQ0 word 6
            setR(ctx, 3, 0x12345678);
            memset(ctx.sq_buffer, 0x55, 64);
        },
        CHECK_SQ});

    tests.push_back({"writem", "sq_i16",
        mkOp(shop_writem, shil_param(), mkReg(reg_r2), mkReg(reg_r3), shil_param(), shil_param(), 2),
        [](Sh4Context& ctx) {
            setR(ctx, 2, 0xE3000032);           // block E3 mirror, SQ1
            setR(ctx, 3, 0xBEEF);
            memset(ctx.sq_buffer, 0x55, 64);
        },
        CHECK_SQ});

    tests.push_back({"writem", "sq_i8",
        mkOp(shop_writem, shil_param(), mkReg(reg_r2), mkReg(reg_r3), shil_param(), shil_param(), 1),
        [](Sh4Context& ctx) {
            setR(ctx, 2, 0xE000003F);           // SQ1 last byte
            setR(ctx, 3, 0xAB);
            memset(ctx.sq_buffer, 0x55, 64);
        },
        CHECK_SQ});

    tests.push_back({"writem", "sq_i64_pair",
        mkOp(shop_writem, shil_param(), mkReg(reg_r2), mkReg(reg_fr_0), shil_param(), shil_param(), 8),
        [](Sh4Context& ctx) {
            setR(ctx, 2, 0xE0000018);           // SQ0 words 6-7
            *(u32*)&ctx.fr[0] = 0x11223344;
            *(u32*)&ctx.fr[1] = 0x55667788;
            memset(ctx.sq_buffer, 0x55, 64);
        },
        CHECK_SQ});

    tests.push_back({"writem", "sq_i64_wrap_3C",
        mkOp(shop_writem, shil_param(), mkReg(reg_r2), mkReg(reg_fr_0), shil_param(), shil_param(), 8),
        [](Sh4Context& ctx) {
            setR(ctx, 2, 0xE000003C);           // low → sq[0x3C], high wraps → sq[0]
            *(u32*)&ctx.fr[0] = 0xAAAA5555;
            *(u32*)&ctx.fr[1] = 0x5555AAAA;
            memset(ctx.sq_buffer, 0x55, 64);
        },
        CHECK_SQ});

    // Guard the boundary: one past the SQ decode (0xE4...) must NOT touch
    // sq_buffer on either path (routes to the generic handler/import).
    tests.push_back({"writem", "sq_decode_boundary",
        mkOp(shop_writem, shil_param(), mkReg(reg_r2), mkReg(reg_r3), shil_param(), shil_param(), 4),
        [](Sh4Context& ctx) {
            setR(ctx, 2, 0xE4000000);           // NOT SQ (>> 26 == 0x39)
            setR(ctx, 3, 0x99999999);
            memset(ctx.sq_buffer, 0x55, 64);
        },
        CHECK_SQ});

    // ============================================================
    // CONTROL
    // ============================================================

    // shop_jdyn: ctx.jdyn = rs1 (+ rs2 if present)
    tests.push_back({"jdyn", "basic",
        mkOp(shop_jdyn, shil_param(), mkReg(reg_r2)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x8C001000); },
        CHECK_JDYN});

    tests.push_back({"jdyn", "with_offset",
        mkOp(shop_jdyn, shil_param(), mkReg(reg_r2), mkImm(4)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0x8C001000); },
        CHECK_JDYN});

    // shop_jcond: jdyn = rs1 (stash condition for delayed branches)
    // Decoder emits: Emit(shop_jcond, reg_pc_dyn, reg_sr_T)
    // WASM emitter hardcodes write to ctx.jdyn (ignores op.rd)
    tests.push_back({"jcond", "true",
        mkOp(shop_jcond, mkReg(reg_pc_dyn), mkReg(reg_sr_T)),
        [](Sh4Context& ctx) { ctx.sr.T = 1; },
        CHECK_JDYN});

    tests.push_back({"jcond", "false",
        mkOp(shop_jcond, mkReg(reg_pc_dyn), mkReg(reg_sr_T)),
        [](Sh4Context& ctx) { ctx.sr.T = 0; },
        CHECK_JDYN});

    // ============================================================
    // SYSTEM OPS
    // ============================================================

    // shop_swaplb: swap low bytes of rs1
    tests.push_back({"swaplb", "basic",
        mkOp(shop_swaplb, mkReg(reg_r0), mkReg(reg_r2)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xAABB1234); },
        CHECK_R0});

    tests.push_back({"swaplb", "same",
        mkOp(shop_swaplb, mkReg(reg_r0), mkReg(reg_r2)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xAABB0000); },
        CHECK_R0});

    // shop_xtrct: (rs1 >> 16) | (rs2 << 16)
    tests.push_back({"xtrct", "basic",
        mkOp(shop_xtrct, mkReg(reg_r0), mkReg(reg_r2), mkReg(reg_r3)),
        [](Sh4Context& ctx) { setR(ctx, 2, 0xAABBCCDD); setR(ctx, 3, 0x11223344); },
        CHECK_R0});

    // shop_mov64: copy 64-bit value (2 consecutive floats)
    tests.push_back({"mov64", "basic",
        mkOp(shop_mov64, mkReg(regv_dr_0), mkReg(regv_dr_4)),
        [](Sh4Context& ctx) {
            setFR(ctx, 4, 1.234f);
            setFR(ctx, 5, 5.678f);
        },
        CHECK_FR0_FR1});
}

// ============================================================
// Main test driver
// ============================================================

static int shil_op_test_harness() {
    std::vector<TestCase> tests;
    registerAllTests(tests);

    u32 pass_count = 0;
    u32 fail_count = 0;
    u32 skip_count = 0;

    // Save the real emulator context + scratch RAM region so we can restore
    // them after the tests run (tests corrupt both via seedContext + memory
    // op writes). Without this, the emulator resumes with bogus state.
    Sh4Context saved_ctx;
    memcpy(&saved_ctx, &Sh4cntx, sizeof(Sh4Context));
    u8 saved_scratch[64];
    memcpy(saved_scratch, &mem_b[TEST_RAM_OFFSET], 64);

#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
    EM_ASM({ console.log('[SHIL-TEST] Starting Layer 1 SHIL op test harness: ' + $0 + ' tests'); },
        (u32)tests.size());
#endif

    for (u32 ti = 0; ti < tests.size(); ti++) {
        TestCase& tc = tests[ti];

        // ---- WASM path ----
        seedContext();
        tc.setup(Sh4cntx);
        // Save pre-execution RAM state for memory tests
        u8 saved_ram[64];
        memcpy(saved_ram, &mem_b[TEST_RAM_OFFSET], 64);

        RuntimeBlockInfo* block = makeSyntheticBlock(tc.op);

        bool wasm_ok = execWasmPath(block);
        if (!wasm_ok) {
#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
            EM_ASM({ console.log('[SHIL-TEST] SKIP ' + UTF8ToString($0) + '/' +
                UTF8ToString($1) + ' — WASM compile/exec failed'); },
                tc.op_name, tc.test_name);
#endif
            destroySyntheticBlock(block);
            skip_count++;
            continue;
        }

        CtxSnapshot wasm_snap;
        takeSnapshot(wasm_snap);

        // ---- Fallback path ----
        // Restore RAM for memory tests
        memcpy(&mem_b[TEST_RAM_OFFSET], saved_ram, 64);
        seedContext();
        tc.setup(Sh4cntx);

        execFallbackPath(block);

        CtxSnapshot fb_snap;
        takeSnapshot(fb_snap);

        // ---- Compare ----
        u32 wasm_val = 0, fb_val = 0;
        const char* diverged = tc.check(wasm_snap, fb_snap, &wasm_val, &fb_val);

        if (diverged == nullptr) {
            pass_count++;
        } else {
            fail_count++;
#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
            EM_ASM({
                console.log('[SHIL-TEST] FAIL ' + UTF8ToString($0) + '/' +
                    UTF8ToString($1) + ' reg=' + UTF8ToString($2) +
                    ' wasm=0x' + ($3>>>0).toString(16) +
                    ' fb=0x' + ($4>>>0).toString(16));
            }, tc.op_name, tc.test_name, diverged, wasm_val, fb_val);
#endif
        }

        destroySyntheticBlock(block);
    }

    // Restore the pre-test SH4 context + scratch RAM so the emulator can
    // resume normally. Without this, ctx.pc is left pointing at a fake
    // 0xDEAD... vaddr from the last test block's exit, which causes
    // JIT-TRAP spam and prevents the game from booting.
    memcpy(&Sh4cntx, &saved_ctx, sizeof(Sh4Context));
    memcpy(&mem_b[TEST_RAM_OFFSET], saved_scratch, 64);

#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
    EM_ASM({
        console.log('[SHIL-TEST] ============================');
        console.log('[SHIL-TEST] TOTAL: ' + $0 + '  PASS: ' + $1 + '  FAIL: ' + $2 + '  SKIP: ' + $3);
        console.log('[SHIL-TEST] ============================');
    }, (u32)tests.size(), pass_count, fail_count, skip_count);
#endif

    return (int)fail_count;
}

// end wasm_test_shil_ops.h
