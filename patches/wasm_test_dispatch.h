// wasm_test_dispatch.h — Layer 1b dispatch-layer unit test harness
//
// Validates the JIT dispatch table (jit_dispatch_table / jit_dispatch_pc /
// jit_dispatch_hash) and its interaction with c_dispatch_loop. These tests
// exercise the subsystems that sit BETWEEN SHIL execution (already proven
// correct by wasm_test_shil_ops.h) and real-game code fetch.
//
// Tests:
//   1. collision_guard       — two PCs hashing to the same table key must
//                              not cross-execute; second PC must miss.
//   2. smc_first_opcode      — RAM-byte mutation at PC must invalidate entry.
//   3. smc_inner_byte        — RAM-byte mutation INSIDE block (past PC+0/1)
//                              is NOT caught by current hash scheme — this
//                              test documents the limitation. A FAIL here
//                              means the emulator silently runs stale code
//                              when games rewrite inner instructions.
//   4. mirror_0c_8c          — 0x0C and 0x8C RAM mirrors share the same
//                              dispatch-key but must be treated as distinct
//                              PCs (jit_dispatch_pc guard).
//   5. stale_entry_after_smc — after an SMC miss, the entry must be zeroed
//                              and re-dispatching the same PC must miss.
//
// Included at the bottom of rec_wasm.cpp AFTER wasm_test_shil_ops.h, so it
// reuses makeSyntheticBlock / destroySyntheticBlock helpers and has access
// to all statics (c_dispatch_loop, jit_dispatch_table, etc.).
//
// Gated with #ifndef JIT_PROD_BUILD — excluded from production builds.
//
// Entry point: dispatch_test_harness() returns number of failures (0 = all pass).

#pragma once

// ============================================================
// Test helpers (specific to dispatch tests)
// ============================================================

// Build a block at a SPECIFIC vaddr (unlike makeSyntheticBlock which
// auto-increments). Used when tests need to control the exact PC.
static RuntimeBlockInfo* makeBlockAtPC(u32 vaddr, const shil_opcode& op,
                                        u32 guest_cycles = 1,
                                        u32 sh4_code_size = 2) {
    RuntimeBlockInfo* block = new RuntimeBlockInfo();
    block->vaddr = vaddr;
    block->addr = vaddr;
    block->BlockType = BET_StaticJump;
    block->BranchBlock = vaddr;         // self-loop
    block->NextBlock = vaddr;
    block->guest_cycles = guest_cycles;
    block->guest_opcodes = 1;
    block->host_opcodes = 0;
    block->host_code_size = 0;
    block->sh4_code_size = sh4_code_size;
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
    blockByVaddr[vaddr] = block;
    return block;
}

// Compile + register a block directly in the dispatch table (mirrors the
// dispatch-table setup in WasmDynarec::compile, without FPCB/chain logic).
// Returns false if compilation failed.
static bool compileAndRegisterDispatch(RuntimeBlockInfo* block) {
#ifdef __EMSCRIPTEN__
    WasmModuleBuilder builder;
    if (!buildBlockModule(builder, block))
        return false;
    const std::vector<u8>& bytes = builder.getBytes();
    int table_idx = wasm_compile_block(bytes.data(), (u32)bytes.size(), block->vaddr);
    if (table_idx <= 0) return false;
    // Use same priming helper as WasmDynarec::compile — keeps tests
    // and production on identical dispatch-metadata paths.
    primeDispatchEntry(block->vaddr, block->sh4_code_size, (u32)table_idx);
    return true;
#else
    (void)block;
    return false;
#endif
}

// Run c_dispatch_loop for at most N cycles.
// Writes result + blocks_run to out params.
static void runDispatch(int max_cycles, u32* out_result, int* out_blocks) {
    Sh4cntx.cycle_counter = max_cycles;
    g_dispatch_result = 0xDEADBEEF;  // sentinel to detect failure to set
    g_dispatch_miss_pc = 0;
    u32 ctx_ptr = (u32)(uintptr_t)&Sh4cntx;
    u32 ram_base = (u32)(uintptr_t)&mem_b[0];
    int blocks = c_dispatch_loop(ctx_ptr, ram_base);
    *out_result = g_dispatch_result;
    *out_blocks = blocks;
}

static void clearDispatchSlot(u32 pc) {
    u32 key = (pc >> 1) & JIT_TABLE_MASK;
    jit_dispatch_table[key] = 0;
    jit_dispatch_pc[key] = 0;
    jit_dispatch_hash[key] = 0;
    jit_dispatch_sz[key] = 0;
}

// Fully remove a test block (table entry, map entry, JS cache, heap).
static void teardownBlock(RuntimeBlockInfo* block) {
    if (!block) return;
    u32 vaddr = block->vaddr;
    clearDispatchSlot(vaddr);
    blockByVaddr.erase(vaddr);
#ifdef __EMSCRIPTEN__
    wasm_remove_block(vaddr);
#endif
    delete block;
}

// ============================================================
// Logging helpers
// ============================================================

#define DISP_LOG_PASS(name) \
    EM_ASM({ console.log('[DISP-TEST] PASS ' + UTF8ToString($0)); }, (name))

#define DISP_LOG_FAIL(name, fmt_args_block) \
    do { EM_ASM({ fmt_args_block }, (name)); } while (0)

// ============================================================
// Individual tests
// Each returns 0 on pass, 1 on fail.
// ============================================================

// --- Test 1: collision_guard -------------------------------------
// Two PCs that hash to the same (pc>>1)&JIT_TABLE_MASK key but differ in
// upper bits. Block registered under PC_A; attempt to dispatch PC_B must
// miss (jit_dispatch_pc guard) and must NOT execute PC_A's block.
static int test_collision_guard() {
    const char* tname = "collision_guard";
    // JIT_TABLE_SIZE = 1<<20. Colliding PCs differ by 2 * JIT_TABLE_SIZE.
    // 0xBEEF0000 / 0xBF0F0000 — both in P2 (area 5), outside area-3 SMC check.
    const u32 pc_a = 0xBEEF0000;
    const u32 pc_b = pc_a + (2 * JIT_TABLE_SIZE);  // = 0xBF0F0000

    if (((pc_a >> 1) & JIT_TABLE_MASK) != ((pc_b >> 1) & JIT_TABLE_MASK)) {
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' setup: keys do not collide'); }, tname);
        return 1;
    }

    clearDispatchSlot(pc_a);
    clearDispatchSlot(pc_b);
    blockByVaddr.erase(pc_a);
    blockByVaddr.erase(pc_b);

    // Block A writes r0 = 0x11111111.
    shil_opcode op_a = mkOp(shop_mov32, mkReg(reg_r0), mkImm(0x11111111));
    RuntimeBlockInfo* blk_a = makeBlockAtPC(pc_a, op_a);
    if (!compileAndRegisterDispatch(blk_a)) {
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' compile A'); }, tname);
        teardownBlock(blk_a);
        return 1;
    }

    // Seed ctx: r0 marker, pc = PC_B (colliding key, different PC).
    seedContext();
    Sh4cntx.r[0] = 0xCAFEBABE;
    Sh4cntx.pc = pc_b;

    u32 result; int blocks;
    runDispatch(1, &result, &blocks);

    bool pass = (result == 1)                  // miss
             && (blocks == 0)                   // no block ran
             && (Sh4cntx.r[0] == 0xCAFEBABE)    // r0 untouched
             && (g_dispatch_miss_pc == pc_b);   // miss pc correct

    if (!pass) {
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' result=' + $1 + ' blocks=' + $2 +
            ' r0=0x' + ($3>>>0).toString(16) +
            ' miss_pc=0x' + ($4>>>0).toString(16) +
            ' (expected result=1 blocks=0 r0=0xcafebabe miss_pc=0x' + ($5>>>0).toString(16) + ')'); },
            tname, result, blocks, Sh4cntx.r[0], g_dispatch_miss_pc, pc_b);
    } else {
        DISP_LOG_PASS(tname);
    }

    teardownBlock(blk_a);
    return pass ? 0 : 1;
}

// --- Test 2: smc_first_opcode ------------------------------------
// Block registered at area-3 RAM PC. First opcode in RAM is primed to match
// the hash. Dispatch succeeds. Mutate first opcode in RAM. Dispatch must
// miss, clear the entry, and set g_dispatch_miss_pc.
static int test_smc_first_opcode() {
    const char* tname = "smc_first_opcode";
    // 0x0CFE0000 — in P0 area 3 (RAM), near end of 16MB main RAM.
    const u32 pc = 0x0CFE0000;
    const u32 phys = pc & 0x1FFFFFFF;
    const u32 ram_off = phys & RAM_MASK;

    // Save RAM bytes we touch (2 bytes at PC).
    u16 saved_bytes;
    memcpy(&saved_bytes, &mem_b[ram_off], 2);

    clearDispatchSlot(pc);
    blockByVaddr.erase(pc);

    // Prime RAM with a known first-opcode value (0xABCD).
    *(u16*)(&mem_b[ram_off]) = 0xABCD;

    // Block writes r1 = 0x22222222 (distinct from other tests).
    shil_opcode op = mkOp(shop_mov32, mkReg(reg_r1), mkImm(0x22222222));
    RuntimeBlockInfo* blk = makeBlockAtPC(pc, op);
    if (!compileAndRegisterDispatch(blk)) {
        memcpy(&mem_b[ram_off], &saved_bytes, 2);
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' compile'); }, tname);
        teardownBlock(blk);
        return 1;
    }

    // --- Pre-check: dispatch hits cleanly ---
    seedContext();
    Sh4cntx.pc = pc;
    u32 result; int blocks;
    runDispatch(1, &result, &blocks);
    bool pre_ok = (blocks == 1) && (Sh4cntx.r[1] == 0x22222222);
    if (!pre_ok) {
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' pre-check: blocks=' + $1 + ' r1=0x' + ($2>>>0).toString(16) +
            ' (expected blocks=1 r1=0x22222222)'); },
            tname, blocks, Sh4cntx.r[1]);
        memcpy(&mem_b[ram_off], &saved_bytes, 2);
        teardownBlock(blk);
        return 1;
    }

    // --- Mutate first opcode in RAM ---
    *(u16*)(&mem_b[ram_off]) = 0xBEEF;  // different from 0xABCD

    // --- Dispatch again: must miss ---
    u32 key = (pc >> 1) & JIT_TABLE_MASK;
    seedContext();
    Sh4cntx.pc = pc;
    Sh4cntx.r[1] = 0xDEADCAFE;  // marker — block must NOT run
    runDispatch(1, &result, &blocks);

    bool pass = (result == 1)                        // miss
             && (blocks == 0)                         // block did not run
             && (Sh4cntx.r[1] == 0xDEADCAFE)          // marker intact
             && (g_dispatch_miss_pc == pc)            // correct miss pc
             && (jit_dispatch_table[key] == 0)        // entry cleared
             && (jit_dispatch_pc[key] == 0)
             && (jit_dispatch_hash[key] == 0);

    if (!pass) {
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' result=' + $1 + ' blocks=' + $2 +
            ' r1=0x' + ($3>>>0).toString(16) +
            ' miss_pc=0x' + ($4>>>0).toString(16) +
            ' tbl=' + $5 + ' pc_slot=0x' + ($6>>>0).toString(16) +
            ' hash=0x' + ($7>>>0).toString(16)); },
            tname, result, blocks, Sh4cntx.r[1], g_dispatch_miss_pc,
            jit_dispatch_table[key], jit_dispatch_pc[key], jit_dispatch_hash[key]);
    } else {
        DISP_LOG_PASS(tname);
    }

    // Restore RAM bytes and tear down.
    memcpy(&mem_b[ram_off], &saved_bytes, 2);
    // Block may have been torn down by the SMC path in c_dispatch_loop —
    // erase from blockByVaddr is idempotent; wasm_remove_block already ran.
    // We still delete the heap object (the SMC path erases the map entry
    // but does not delete the block pointer).
    clearDispatchSlot(pc);
    blockByVaddr.erase(pc);
    delete blk;
    return pass ? 0 : 1;
}

// --- Test 3: smc_inner_byte --------------------------------------
// Block covers 2 SH4 instructions (sh4_code_size=4). First opcode at PC
// matches hash. Mutate RAM at PC+2 (second instruction). With the
// widened hash (covers all sh4_code_size bytes), the mutation must be
// detected and dispatch must miss.
//
// Expected outcome: PASS. A FAIL here means the widened hash isn't
// actually covering inner bytes — check primeDispatchEntry uses the
// block's sh4_code_size, not a hardcoded 2.
static int test_smc_inner_byte() {
    const char* tname = "smc_inner_byte";
    const u32 pc = 0x0CFE0100;  // distinct from test_smc_first_opcode
    const u32 phys = pc & 0x1FFFFFFF;
    const u32 ram_off = phys & RAM_MASK;

    // Save 4 bytes (2 instructions).
    u32 saved_bytes;
    memcpy(&saved_bytes, &mem_b[ram_off], 4);

    clearDispatchSlot(pc);
    blockByVaddr.erase(pc);

    // Prime RAM: first opcode = 0x1234, second opcode = 0x5678.
    *(u16*)(&mem_b[ram_off + 0]) = 0x1234;
    *(u16*)(&mem_b[ram_off + 2]) = 0x5678;

    // Block spans 2 SH4 instructions (sh4_code_size=4, guest_cycles=2).
    shil_opcode op = mkOp(shop_mov32, mkReg(reg_r2), mkImm(0x33333333));
    RuntimeBlockInfo* blk = makeBlockAtPC(pc, op, /*guest_cycles*/2, /*sh4_code_size*/4);
    if (!compileAndRegisterDispatch(blk)) {
        memcpy(&mem_b[ram_off], &saved_bytes, 4);
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' compile'); }, tname);
        teardownBlock(blk);
        return 1;
    }

    // Pre-check: clean dispatch works.
    seedContext();
    Sh4cntx.pc = pc;
    u32 result; int blocks;
    runDispatch(1, &result, &blocks);
    bool pre_ok = (blocks == 1) && (Sh4cntx.r[2] == 0x33333333);
    if (!pre_ok) {
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' pre-check: blocks=' + $1 + ' r2=0x' + ($2>>>0).toString(16)); },
            tname, blocks, Sh4cntx.r[2]);
        memcpy(&mem_b[ram_off], &saved_bytes, 4);
        teardownBlock(blk);
        return 1;
    }

    // Mutate INNER opcode (PC+2) — first opcode at PC unchanged.
    *(u16*)(&mem_b[ram_off + 2]) = 0xDEAD;

    // Expected CORRECT behavior: dispatch misses (inner byte changed, code
    // is now different). Actual CURRENT behavior: dispatch hits because
    // only PC+0 is hashed.
    seedContext();
    Sh4cntx.pc = pc;
    Sh4cntx.r[2] = 0xDEADCAFE;  // marker
    runDispatch(1, &result, &blocks);

    // PASS if the miss was detected (expected correct behavior).
    // FAIL documents the known hazard.
    bool miss_detected = (result == 1) && (blocks == 0)
                      && (Sh4cntx.r[2] == 0xDEADCAFE);
    bool stale_hit     = (result == 0) && (blocks == 1)
                      && (Sh4cntx.r[2] == 0x33333333);

    if (miss_detected) {
        DISP_LOG_PASS(tname);
    } else if (stale_hit) {
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' — inner-byte SMC NOT detected (stale block ran). ' +
            'Hash scheme covers only PC+0/1. Games that rewrite inner ' +
            'instructions will execute stale compiled code.'); }, tname);
    } else {
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' — unexpected state: result=' + $1 + ' blocks=' + $2 +
            ' r2=0x' + ($3>>>0).toString(16)); },
            tname, result, blocks, Sh4cntx.r[2]);
    }

    // Cleanup — block still valid because SMC didn't fire.
    memcpy(&mem_b[ram_off], &saved_bytes, 4);
    teardownBlock(blk);
    return miss_detected ? 0 : 1;
}

// --- Test 4: mirror_0c_8c ----------------------------------------
// 0x0C and 0x8C are RAM mirrors (same physical RAM, different virtual PCs).
// They collide on dispatch key but have different jit_dispatch_pc values;
// the guard must fire and the second must miss.
static int test_mirror_0c_8c() {
    const char* tname = "mirror_0c_8c";
    const u32 pc_cached   = 0x8CFE0200;   // P1 (cached)
    const u32 pc_uncached = 0x0CFE0200;   // P0 (same physical RAM)
    const u32 phys = pc_cached & 0x1FFFFFFF;
    const u32 ram_off = phys & RAM_MASK;

    // Keys must match (mirror pairs alias at the key level).
    if (((pc_cached >> 1) & JIT_TABLE_MASK) != ((pc_uncached >> 1) & JIT_TABLE_MASK)) {
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' setup: mirror keys do not match'); }, tname);
        return 1;
    }

    u16 saved_bytes;
    memcpy(&saved_bytes, &mem_b[ram_off], 2);

    clearDispatchSlot(pc_cached);
    clearDispatchSlot(pc_uncached);
    blockByVaddr.erase(pc_cached);
    blockByVaddr.erase(pc_uncached);

    // Prime RAM (both mirrors read same bytes).
    *(u16*)(&mem_b[ram_off]) = 0x7777;

    // Block at 0x8C mirror writes r3 = 0x44444444.
    shil_opcode op = mkOp(shop_mov32, mkReg(reg_r3), mkImm(0x44444444));
    RuntimeBlockInfo* blk = makeBlockAtPC(pc_cached, op);
    if (!compileAndRegisterDispatch(blk)) {
        memcpy(&mem_b[ram_off], &saved_bytes, 2);
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' compile'); }, tname);
        teardownBlock(blk);
        return 1;
    }

    // Attempt to dispatch the UNCACHED mirror. Must miss — full PC differs.
    seedContext();
    Sh4cntx.pc = pc_uncached;
    Sh4cntx.r[3] = 0xCAFEBABE;  // marker
    u32 result; int blocks;
    runDispatch(1, &result, &blocks);

    bool pass = (result == 1)
             && (blocks == 0)
             && (Sh4cntx.r[3] == 0xCAFEBABE)
             && (g_dispatch_miss_pc == pc_uncached);

    if (!pass) {
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' result=' + $1 + ' blocks=' + $2 +
            ' r3=0x' + ($3>>>0).toString(16) +
            ' miss_pc=0x' + ($4>>>0).toString(16)); },
            tname, result, blocks, Sh4cntx.r[3], g_dispatch_miss_pc);
    } else {
        DISP_LOG_PASS(tname);
    }

    memcpy(&mem_b[ram_off], &saved_bytes, 2);
    teardownBlock(blk);
    // Also clear the uncached-mirror slot in case c_dispatch_loop wrote to it.
    clearDispatchSlot(pc_uncached);
    blockByVaddr.erase(pc_uncached);
    return pass ? 0 : 1;
}

// --- Test 5: stale_entry_after_smc -------------------------------
// After a successful SMC miss + clear, re-dispatching the same PC must
// miss again (entry stays cleared until re-compile). Guards against a
// regression where SMC clears the hash but leaves a stale table pointer.
static int test_stale_entry_after_smc() {
    const char* tname = "stale_entry_after_smc";
    const u32 pc = 0x0CFE0300;
    const u32 phys = pc & 0x1FFFFFFF;
    const u32 ram_off = phys & RAM_MASK;

    u16 saved_bytes;
    memcpy(&saved_bytes, &mem_b[ram_off], 2);

    clearDispatchSlot(pc);
    blockByVaddr.erase(pc);

    *(u16*)(&mem_b[ram_off]) = 0xAABB;

    shil_opcode op = mkOp(shop_mov32, mkReg(reg_r4), mkImm(0x55555555));
    RuntimeBlockInfo* blk = makeBlockAtPC(pc, op);
    if (!compileAndRegisterDispatch(blk)) {
        memcpy(&mem_b[ram_off], &saved_bytes, 2);
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' compile'); }, tname);
        teardownBlock(blk);
        return 1;
    }

    // Trigger SMC by mutating first opcode.
    *(u16*)(&mem_b[ram_off]) = 0xCCDD;

    seedContext();
    Sh4cntx.pc = pc;
    u32 result; int blocks;
    runDispatch(1, &result, &blocks);  // triggers SMC clear

    if (result != 1) {
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' setup SMC did not fire: result=' + $1); }, tname, result);
        memcpy(&mem_b[ram_off], &saved_bytes, 2);
        clearDispatchSlot(pc);
        blockByVaddr.erase(pc);
        delete blk;
        return 1;
    }

    // Re-dispatch same PC — must miss again (no stale block runs).
    seedContext();
    Sh4cntx.pc = pc;
    Sh4cntx.r[4] = 0xDEADFACE;  // marker
    runDispatch(1, &result, &blocks);

    u32 key = (pc >> 1) & JIT_TABLE_MASK;
    bool pass = (result == 1)
             && (blocks == 0)
             && (Sh4cntx.r[4] == 0xDEADFACE)
             && (jit_dispatch_table[key] == 0)
             && (jit_dispatch_pc[key] == 0);

    if (!pass) {
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' re-dispatch: result=' + $1 + ' blocks=' + $2 +
            ' r4=0x' + ($3>>>0).toString(16) +
            ' tbl=' + $4 + ' pc_slot=0x' + ($5>>>0).toString(16)); },
            tname, result, blocks, Sh4cntx.r[4],
            jit_dispatch_table[key], jit_dispatch_pc[key]);
    } else {
        DISP_LOG_PASS(tname);
    }

    memcpy(&mem_b[ram_off], &saved_bytes, 2);
    clearDispatchSlot(pc);
    blockByVaddr.erase(pc);
    delete blk;
    return pass ? 0 : 1;
}

// --- Test 6: chain_smc_interior ----------------------------------
// Multi-block chain: block A (entry) → block B (interior). Interior
// SMC guard in buildMultiBlockModule previously checked only the first
// opcode; inner-byte mutation of block B went undetected and stale
// chained code ran. This test validates the widened guard (full-block
// hash) on the interior block.
static int test_chain_smc_interior() {
    const char* tname = "chain_smc_interior";
    const u32 pc_a = 0x0CFE0400;
    const u32 pc_b = 0x0CFE0500;
    const u32 phys_a = pc_a & 0x1FFFFFFF;
    const u32 phys_b = pc_b & 0x1FFFFFFF;
    const u32 ram_off_a = phys_a & RAM_MASK;
    const u32 ram_off_b = phys_b & RAM_MASK;

    u32 saved_a, saved_b;
    memcpy(&saved_a, &mem_b[ram_off_a], 4);
    memcpy(&saved_b, &mem_b[ram_off_b], 4);

    clearDispatchSlot(pc_a);
    clearDispatchSlot(pc_b);
    blockByVaddr.erase(pc_a);
    blockByVaddr.erase(pc_b);

    // Prime RAM at both PCs. Block B spans 2 SH4 instructions (4 bytes)
    // so we have inner bytes to mutate.
    *(u16*)(&mem_b[ram_off_a + 0]) = 0x1111;
    *(u16*)(&mem_b[ram_off_a + 2]) = 0x2222;
    *(u16*)(&mem_b[ram_off_b + 0]) = 0x3333;
    *(u16*)(&mem_b[ram_off_b + 2]) = 0x4444;

    // Block A: shop_mov32 r5 = 0x66666666, static jump to B.
    shil_opcode op_a = mkOp(shop_mov32, mkReg(reg_r5), mkImm(0x66666666));
    RuntimeBlockInfo* blk_a = makeBlockAtPC(pc_a, op_a, /*gc*/1, /*sh4_sz*/4);
    blk_a->BranchBlock = pc_b;                // chain to B
    blk_a->NextBlock = pc_b;

    // Block B: shop_mov32 r6 = 0x77777777, jumps outside chain to exit.
    const u32 pc_outside = 0x0CBADF00;        // area 3 but outside chain
    shil_opcode op_b = mkOp(shop_mov32, mkReg(reg_r6), mkImm(0x77777777));
    RuntimeBlockInfo* blk_b = makeBlockAtPC(pc_b, op_b, /*gc*/1, /*sh4_sz*/4);
    blk_b->BranchBlock = pc_outside;          // exits chain
    blk_b->NextBlock = pc_outside;

    // Build multi-block module directly.
    WasmModuleBuilder builder;
    std::vector<RuntimeBlockInfo*> chain = { blk_a, blk_b };
    if (!buildMultiBlockModule(builder, chain)) {
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' buildMultiBlockModule'); }, tname);
        memcpy(&mem_b[ram_off_a], &saved_a, 4);
        memcpy(&mem_b[ram_off_b], &saved_b, 4);
        teardownBlock(blk_a);
        teardownBlock(blk_b);
        return 1;
    }

    // Compile the module. Register entry block (A) in dispatch table.
    const std::vector<u8>& bytes = builder.getBytes();
    int table_idx = wasm_compile_block(bytes.data(), (u32)bytes.size(), pc_a);
    if (table_idx <= 0) {
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' wasm_compile_block'); }, tname);
        memcpy(&mem_b[ram_off_a], &saved_a, 4);
        memcpy(&mem_b[ram_off_b], &saved_b, 4);
        teardownBlock(blk_a);
        teardownBlock(blk_b);
        return 1;
    }
    primeDispatchEntry(pc_a, blk_a->sh4_code_size, (u32)table_idx);
    // B is chain-interior — no dispatch-table entry.

    // Pre-check: both A and B run, side effects visible.
    seedContext();
    Sh4cntx.pc = pc_a;
    Sh4cntx.r[5] = 0;
    Sh4cntx.r[6] = 0;
    u32 result; int blocks;
    runDispatch(2, &result, &blocks);       // budget for both A and B

    bool pre_ok = (Sh4cntx.r[5] == 0x66666666)
               && (Sh4cntx.r[6] == 0x77777777);
    if (!pre_ok) {
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' pre-check: r5=0x' + ($1>>>0).toString(16) +
            ' r6=0x' + ($2>>>0).toString(16) +
            ' (expected r5=0x66666666 r6=0x77777777)'); },
            tname, Sh4cntx.r[5], Sh4cntx.r[6]);
        memcpy(&mem_b[ram_off_a], &saved_a, 4);
        memcpy(&mem_b[ram_off_b], &saved_b, 4);
        teardownBlock(blk_a);
        teardownBlock(blk_b);
        return 1;
    }

    // Mutate INNER byte of block B (PC_B + 2). First opcode at PC_B unchanged.
    *(u16*)(&mem_b[ram_off_b + 2]) = 0xBEEF;

    // Re-dispatch: interior guard for B must detect mutation, exit chain
    // before B runs. r5 is set (A always runs); r6 must remain unchanged.
    seedContext();
    Sh4cntx.pc = pc_a;
    Sh4cntx.r[5] = 0;
    Sh4cntx.r[6] = 0xCAFEBABE;              // marker: B must NOT run
    runDispatch(2, &result, &blocks);

    bool guard_fired = (Sh4cntx.r[5] == 0x66666666)
                    && (Sh4cntx.r[6] == 0xCAFEBABE);
    bool stale_ran   = (Sh4cntx.r[5] == 0x66666666)
                    && (Sh4cntx.r[6] == 0x77777777);

    if (guard_fired) {
        DISP_LOG_PASS(tname);
    } else if (stale_ran) {
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' — interior guard missed inner-byte SMC of block B. ' +
            'Block B ran stale (r6=0x77777777 despite PC_B+2 rewrite). ' +
            'Widen buildMultiBlockModule interior guard to full-block hash.'); },
            tname);
    } else {
        EM_ASM({ console.error('[DISP-TEST] FAIL ' + UTF8ToString($0) +
            ' unexpected: r5=0x' + ($1>>>0).toString(16) +
            ' r6=0x' + ($2>>>0).toString(16)); },
            tname, Sh4cntx.r[5], Sh4cntx.r[6]);
    }

    // Cleanup.
    memcpy(&mem_b[ram_off_a], &saved_a, 4);
    memcpy(&mem_b[ram_off_b], &saved_b, 4);
    teardownBlock(blk_a);
    teardownBlock(blk_b);
    return guard_fired ? 0 : 1;
}

// ============================================================
// Main driver
// ============================================================

static int dispatch_test_harness() {
    // Save emulator context + enough of mem_b to cover all test RAM offsets.
    Sh4Context saved_ctx;
    memcpy(&saved_ctx, &Sh4cntx, sizeof(Sh4Context));

    EM_ASM({ console.log('[DISP-TEST] Starting Layer 1b dispatch-layer tests (chain-guard v3)'); });

    int fail = 0;
    fail += test_collision_guard();
    fail += test_smc_first_opcode();
    fail += test_smc_inner_byte();
    fail += test_mirror_0c_8c();
    fail += test_stale_entry_after_smc();
    fail += test_chain_smc_interior();

    // Restore emulator context so game resumption is clean.
    memcpy(&Sh4cntx, &saved_ctx, sizeof(Sh4Context));

    EM_ASM({
        console.log('[DISP-TEST] ============================');
        console.log('[DISP-TEST] TOTAL: 6  PASS: ' + (6-$0) + '  FAIL: ' + $0);
        console.log('[DISP-TEST] ============================');
    }, fail);

    return fail;
}
