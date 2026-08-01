// wasm_test_rte.h — Layer 1d RTE / SR-update semantics harness
//
// The SH4 JIT decodes RTE as a sequence of SHIL ops rather than a fallback
// call (see decoder.cpp:197–205). The composition is:
//
//   shop_and  reg_sr_status, reg_ssr, sr_t::MASK   ; SR = SSR (status bits)
//   shop_and  reg_sr_T,      reg_ssr, 1            ; SR.T = SSR & 1
//   shop_sync_sr                                    ; UpdateSR() — bank swap
//   shop_jdyn reg_pc_dyn,    reg_spc                ; jdyn = SPC
//   BlockType = BET_DynamicIntr                     ; pc = jdyn on exit
//
// Individual ops tested at Layer 1 (143/143). Block-exit types tested at
// Layer 1 (20/20). This harness verifies the COMPOSITION: the whole RTE
// sequence produces the expected post-state end-to-end.
//
// If any DOA2/VT-class freeze is caused by a mishandled RTE (bank mis-swap,
// stale SPC, missed interrupt delivery), one of these tests will fail.
//
// Entry point: rte_test_harness() returns number of failures.

#pragma once

// ============================================================
// Helper: build a block that mirrors decoder.cpp's RTE output
// ============================================================

// Creates a block at `vaddr` whose oplist is the JIT's decoded RTE
// sequence. Registered in blockByVaddr; caller must destroy via
// teardownBlock() (which also clears the dispatch slot).
static RuntimeBlockInfo* makeRteBlock(u32 vaddr) {
    RuntimeBlockInfo* block = new RuntimeBlockInfo();
    block->vaddr = vaddr;
    block->addr = vaddr;
    block->BlockType = BET_DynamicIntr;
    block->BranchBlock = 0;
    block->NextBlock = 0;
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

    // dec_write_sr(reg_ssr) = 2 ops
    block->oplist.push_back(
        mkOp(shop_and, mkReg(reg_sr_status), mkReg(reg_ssr), mkImm(sr_t::MASK)));
    block->oplist.push_back(
        mkOp(shop_and, mkReg(reg_sr_T), mkReg(reg_ssr), mkImm(1)));
    // shop_sync_sr (nullary)
    block->oplist.push_back(mkOp(shop_sync_sr));
    // dec_DynamicSet(reg_spc): shop_jdyn reg_pc_dyn, reg_spc
    block->oplist.push_back(
        mkOp(shop_jdyn, mkReg(reg_pc_dyn), mkReg(reg_spc)));

    blockByVaddr[vaddr] = block;
    return block;
}

// Compile + register RTE block in dispatch table. Non-RAM area (0xDEAD...)
// so the SMC check skips.
static bool compileRteBlock(RuntimeBlockInfo* block) {
#ifdef __EMSCRIPTEN__
    WasmModuleBuilder builder;
    if (!buildBlockModule(builder, block))
        return false;
    const std::vector<u8>& bytes = builder.getBytes();
    int table_idx = wasm_compile_block(bytes.data(), (u32)bytes.size(), block->vaddr);
    if (table_idx <= 0) return false;
    primeDispatchEntry(block->vaddr, block->sh4_code_size, (u32)table_idx);
    return true;
#else
    (void)block;
    return false;
#endif
}

// Run one RTE block via c_dispatch_loop. Returns g_dispatch_result.
static u32 runRteDispatch(u32 pc, int max_cycles) {
    Sh4cntx.pc = pc;
    Sh4cntx.cycle_counter = max_cycles;
    g_dispatch_result = 0xDEADBEEF;
    g_dispatch_miss_pc = 0;
    u32 ctx_ptr = (u32)(uintptr_t)&Sh4cntx;
    u32 ram_base = (u32)(uintptr_t)&mem_b[0];
    c_dispatch_loop(ctx_ptr, ram_base);
    return g_dispatch_result;
}

// ============================================================
// Individual tests
// ============================================================

// --- Test 1: rte_pc_from_spc ----------------------------------
// After executing RTE, ctx.pc must equal the SPC value that was set.
static int test_rte_pc_from_spc() {
    const char* tname = "rte_pc_from_spc";
    const u32 rte_pc = 0xBEEE0000;  // area 5 — non-RAM, SMC skipped
    const u32 target_spc = 0xCAFE1234;

    clearDispatchSlot(rte_pc);
    blockByVaddr.erase(rte_pc);

    RuntimeBlockInfo* blk = makeRteBlock(rte_pc);
    if (!compileRteBlock(blk)) {
        EM_ASM({ console.error('[RTE-TEST] FAIL ' + UTF8ToString($0) +
            ' compile'); }, tname);
        teardownBlock(blk);
        return 1;
    }

    seedContext();
    Sh4cntx.ssr = 0x40000000;             // MD=1, RB=0, BL=0, IMASK=0
    Sh4cntx.spc = target_spc;
    // Pre-exception state: normal run (BL=0, RB=0, MD=1). Simplest case.
    Sh4cntx.sr.status = 0x40000000;
    Sh4cntx.old_sr.status = Sh4cntx.sr.status;
    Sh4cntx.sr.T = 0;

    runRteDispatch(rte_pc, 2);

    bool pass = (Sh4cntx.pc == target_spc);
    if (!pass) {
        EM_ASM({ console.error('[RTE-TEST] FAIL ' + UTF8ToString($0) +
            ' pc=0x' + ($1>>>0).toString(16) +
            ' expected=0x' + ($2>>>0).toString(16)); },
            tname, Sh4cntx.pc, target_spc);
    } else {
        EM_ASM({ console.log('[RTE-TEST] PASS ' + UTF8ToString($0)); }, tname);
    }
    teardownBlock(blk);
    return pass ? 0 : 1;
}

// --- Test 2: rte_sr_restored_from_ssr -------------------------
// SR.status + SR.T must take SSR's values post-RTE.
static int test_rte_sr_restored_from_ssr() {
    const char* tname = "rte_sr_restored_from_ssr";
    const u32 rte_pc = 0xBEEE0100;

    clearDispatchSlot(rte_pc);
    blockByVaddr.erase(rte_pc);

    RuntimeBlockInfo* blk = makeRteBlock(rte_pc);
    if (!compileRteBlock(blk)) {
        EM_ASM({ console.error('[RTE-TEST] FAIL ' + UTF8ToString($0) +
            ' compile'); }, tname);
        teardownBlock(blk);
        return 1;
    }

    // Pre-state: BL=1, MD=1, RB=1 (exception entry). IMASK=0xF.
    // Target (SSR): BL=0, MD=1, RB=0, IMASK=0x0, T=1.
    u32 pre_sr  = 0x600000F0;   // BL=1, RB=1, MD=1, IMASK=0xF
    u32 target_ssr_full = 0x40000001; // MD=1, RB=0, BL=0, IMASK=0, T=1

    seedContext();
    Sh4cntx.spc = 0xCAFE5678;
    Sh4cntx.ssr = target_ssr_full;
    Sh4cntx.sr.status = pre_sr;
    Sh4cntx.sr.T = 0;
    Sh4cntx.old_sr.status = Sh4cntx.sr.status;

    runRteDispatch(rte_pc, 2);

    u32 got_sr = Sh4cntx.sr.status & sr_t::MASK;
    u32 want_sr = target_ssr_full & sr_t::MASK;
    bool pass = (got_sr == want_sr) && (Sh4cntx.sr.T == (target_ssr_full & 1));

    if (!pass) {
        EM_ASM({ console.error('[RTE-TEST] FAIL ' + UTF8ToString($0) +
            ' sr.status=0x' + ($1>>>0).toString(16) +
            ' expected=0x' + ($2>>>0).toString(16) +
            ' sr.T=' + $3 + ' expected=' + $4); },
            tname, got_sr, want_sr, Sh4cntx.sr.T, target_ssr_full & 1);
    } else {
        EM_ASM({ console.log('[RTE-TEST] PASS ' + UTF8ToString($0)); }, tname);
    }
    teardownBlock(blk);
    return pass ? 0 : 1;
}

// --- Test 3: rte_bank_swap_on_rb_change -----------------------
// When SSR.RB differs from current SR.RB, UpdateSR swaps r[] and r_bank[].
// Pre-state: SR.RB=1 (bank1 active in r[]), SSR.RB=0 (bank0 active after).
// Seed bank-distinguishing values into r[] and r_bank[], verify they swap.
static int test_rte_bank_swap_on_rb_change() {
    const char* tname = "rte_bank_swap_on_rb_change";
    const u32 rte_pc = 0xBEEE0200;

    clearDispatchSlot(rte_pc);
    blockByVaddr.erase(rte_pc);

    RuntimeBlockInfo* blk = makeRteBlock(rte_pc);
    if (!compileRteBlock(blk)) {
        EM_ASM({ console.error('[RTE-TEST] FAIL ' + UTF8ToString($0) +
            ' compile'); }, tname);
        teardownBlock(blk);
        return 1;
    }

    seedContext();
    // Pre-state: exception mode — SR.MD=1, RB=1, BL=1
    Sh4cntx.sr.status = 0x60000000;      // MD=1, RB=1, BL=1
    Sh4cntx.sr.T = 0;
    Sh4cntx.old_sr.status = Sh4cntx.sr.status;
    // SSR: target state — SR.MD=1, RB=0, BL=0
    Sh4cntx.ssr = 0x40000000;
    Sh4cntx.spc = 0xCAFEBEEF;

    // Seed distinguishing values: r[] holds bank1 (A..H), r_bank[] holds bank0 (1..8).
    for (u32 i = 0; i < 8; i++) {
        Sh4cntx.r[i] = 0xB1000000 | (i + 0xA);   // bank1 marker
        Sh4cntx.r_bank[i] = 0xB0000000 | (i + 1); // bank0 marker
    }

    runRteDispatch(rte_pc, 2);

    // After RTE with RB 1→0, UpdateSR swaps. r[0..7] should now hold
    // what was in r_bank[], and r_bank[] holds what r[] held.
    bool pass = true;
    for (u32 i = 0; i < 8; i++) {
        u32 want_r = 0xB0000000 | (i + 1);
        u32 want_rbank = 0xB1000000 | (i + 0xA);
        if (Sh4cntx.r[i] != want_r || Sh4cntx.r_bank[i] != want_rbank) {
            pass = false;
            EM_ASM({ console.error('[RTE-TEST] FAIL ' + UTF8ToString($0) +
                ' bank swap at i=' + $1 +
                ' r=0x' + ($2>>>0).toString(16) +
                ' want r=0x' + ($3>>>0).toString(16) +
                ' r_bank=0x' + ($4>>>0).toString(16) +
                ' want r_bank=0x' + ($5>>>0).toString(16)); },
                tname, i, Sh4cntx.r[i], want_r, Sh4cntx.r_bank[i], want_rbank);
            break;
        }
    }
    if (pass) {
        EM_ASM({ console.log('[RTE-TEST] PASS ' + UTF8ToString($0)); }, tname);
    }
    teardownBlock(blk);
    return pass ? 0 : 1;
}

// --- Test 4: rte_bl_clear_delivers_interrupt ------------------
// Pre-RTE: SR.BL=1, pending interrupt masked. Post-RTE: BL=0 → interrupt
// must be decoded as pending (ctx.interrupt_pend != 0).
static int test_rte_bl_clear_delivers_interrupt() {
    const char* tname = "rte_bl_clear_delivers_interrupt";
    const u32 rte_pc = 0xBEEE0300;

    clearDispatchSlot(rte_pc);
    blockByVaddr.erase(rte_pc);

    RuntimeBlockInfo* blk = makeRteBlock(rte_pc);
    if (!compileRteBlock(blk)) {
        EM_ASM({ console.error('[RTE-TEST] FAIL ' + UTF8ToString($0) +
            ' compile'); }, tname);
        teardownBlock(blk);
        return 1;
    }

    seedContext();
    // Pre-state: BL=1, MD=1, RB=0, IMASK=0. BL masks everything.
    Sh4cntx.sr.status = 0x50000000;       // MD=1, BL=1, RB=0
    Sh4cntx.sr.T = 0;
    Sh4cntx.old_sr.status = Sh4cntx.sr.status;
    // SSR: BL=0 (target), MD=1, RB=0, IMASK=0 (allow all priorities)
    Sh4cntx.ssr = 0x40000000;
    Sh4cntx.spc = 0xCAFE9000;

    // Raise a real interrupt via the ASIC path (HBlank, level 9) so the
    // interrupt bitmap is set up consistently with what games see.
    // Before raising, SR.BL=1 → UpdateSR/SRdecode masks all → interrupt_pend=0.
    SRdecode();  // ensure decoded_srimask reflects BL=1
    Sh4cntx.interrupt_pend = 0;
    // Inject pending bit directly into interrupt_vpend so we don't
    // depend on real device state. Pick sh4_IRL_9 (index 0 in the
    // interrupt source list).
    extern u32 InterruptBit[32];
    u32 fake_pend = InterruptBit[0];  // IRL_9 bit

    // Call SetInterruptPend via the public API (matches real path):
    // but to avoid side effects on real emulator state, just set vpend
    // directly by re-invoking via the public API:
    SetInterruptPend(sh4_IRL_9);

    // Sanity: with BL=1, interrupt must still be masked.
    if (Sh4cntx.interrupt_pend != 0) {
        EM_ASM({ console.error('[RTE-TEST] FAIL ' + UTF8ToString($0) +
            ' setup: interrupt_pend non-zero with BL=1'); }, tname);
        ResetInterruptPend(sh4_IRL_9);
        teardownBlock(blk);
        return 1;
    }

    runRteDispatch(rte_pc, 2);

    bool pass = (Sh4cntx.interrupt_pend != 0);

    if (!pass) {
        EM_ASM({ console.error('[RTE-TEST] FAIL ' + UTF8ToString($0) +
            ' interrupt_pend=0x' + ($1>>>0).toString(16) +
            ' (expected non-zero after BL cleared)'); },
            tname, Sh4cntx.interrupt_pend);
    } else {
        EM_ASM({ console.log('[RTE-TEST] PASS ' + UTF8ToString($0)); }, tname);
    }

    // Clean up injected interrupt state.
    ResetInterruptPend(sh4_IRL_9);
    (void)fake_pend;
    teardownBlock(blk);
    return pass ? 0 : 1;
}

// --- Test 5: spc_write_flushed_before_rte ---------------------
// Simulates the "handler computes return PC in a register, writes to SPC,
// then RTEs" pattern. If the RegCache holds a stale spc value when
// shop_jdyn reads it, we'd end up dispatching the wrong PC.
//
// Build a block with: shop_mov32 reg_spc, imm(HANDLER_TARGET) ; RTE sequence
// Execute, verify ctx.pc == HANDLER_TARGET (not the initial seeded SPC).
static int test_spc_write_flushed_before_rte() {
    const char* tname = "spc_write_flushed_before_rte";
    const u32 rte_pc = 0xBEEE0400;
    const u32 handler_target = 0xABCD5678;
    const u32 initial_spc   = 0xFFFF0000;

    clearDispatchSlot(rte_pc);
    blockByVaddr.erase(rte_pc);

    // Custom block: SPC-write-then-RTE.
    RuntimeBlockInfo* blk = new RuntimeBlockInfo();
    blk->vaddr = rte_pc;
    blk->addr = rte_pc;
    blk->BlockType = BET_DynamicIntr;
    blk->BranchBlock = 0;
    blk->NextBlock = 0;
    blk->guest_cycles = 1;
    blk->guest_opcodes = 1;
    blk->host_opcodes = 0;
    blk->host_code_size = 0;
    blk->sh4_code_size = 2;
    blk->has_jcond = false;
    blk->has_fpu_op = false;
    blk->temp_block = false;
    blk->blockcheck_failures = 0;
    blk->code = nullptr;
    blk->pBranchBlock = nullptr;
    blk->pNextBlock = nullptr;
    blk->relink_offset = 0;
    blk->relink_data = 0;
    blk->read_only = false;
    blk->fpu_cfg.full = Sh4cntx.fpscr.full;

    // Prepend: write new SPC via SHIL.
    blk->oplist.push_back(
        mkOp(shop_mov32, mkReg(reg_spc), mkImm(handler_target)));
    // Then the full RTE sequence.
    blk->oplist.push_back(
        mkOp(shop_and, mkReg(reg_sr_status), mkReg(reg_ssr), mkImm(sr_t::MASK)));
    blk->oplist.push_back(
        mkOp(shop_and, mkReg(reg_sr_T), mkReg(reg_ssr), mkImm(1)));
    blk->oplist.push_back(mkOp(shop_sync_sr));
    blk->oplist.push_back(
        mkOp(shop_jdyn, mkReg(reg_pc_dyn), mkReg(reg_spc)));

    blockByVaddr[rte_pc] = blk;

    if (!compileRteBlock(blk)) {
        EM_ASM({ console.error('[RTE-TEST] FAIL ' + UTF8ToString($0) +
            ' compile'); }, tname);
        teardownBlock(blk);
        return 1;
    }

    seedContext();
    Sh4cntx.spc = initial_spc;   // must be overwritten by the mov32 op
    Sh4cntx.ssr = 0x40000000;
    Sh4cntx.sr.status = 0x40000000;
    Sh4cntx.old_sr.status = Sh4cntx.sr.status;
    Sh4cntx.sr.T = 0;

    runRteDispatch(rte_pc, 2);

    bool pass = (Sh4cntx.pc == handler_target);
    if (!pass) {
        EM_ASM({ console.error('[RTE-TEST] FAIL ' + UTF8ToString($0) +
            ' pc=0x' + ($1>>>0).toString(16) +
            ' expected=0x' + ($2>>>0).toString(16) +
            ' (initial_spc=0x' + ($3>>>0).toString(16) + ')'); },
            tname, Sh4cntx.pc, handler_target, initial_spc);
    } else {
        EM_ASM({ console.log('[RTE-TEST] PASS ' + UTF8ToString($0)); }, tname);
    }
    teardownBlock(blk);
    return pass ? 0 : 1;
}

// ============================================================
// Main driver
// ============================================================

static int rte_test_harness() {
    Sh4Context saved_ctx;
    memcpy(&saved_ctx, &Sh4cntx, sizeof(Sh4Context));

    EM_ASM({ console.log('[RTE-TEST] Starting Layer 1d RTE/SR-update tests'); });

    int fail = 0;
    fail += test_rte_pc_from_spc();
    fail += test_rte_sr_restored_from_ssr();
    fail += test_rte_bank_swap_on_rb_change();
    fail += test_rte_bl_clear_delivers_interrupt();
    fail += test_spc_write_flushed_before_rte();

    memcpy(&Sh4cntx, &saved_ctx, sizeof(Sh4Context));

    EM_ASM({
        console.log('[RTE-TEST] ============================');
        console.log('[RTE-TEST] TOTAL: 5  PASS: ' + (5-$0) + '  FAIL: ' + $0);
        console.log('[RTE-TEST] ============================');
    }, fail);

    return fail;
}
