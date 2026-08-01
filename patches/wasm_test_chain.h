// wasm_test_chain.h — Layer 2 multi-block chain differential harness
//
// This is the harness the methodology has been pointing at since 2026-04-18:
// multi-block chaining (buildMultiBlockModule) is the dominant freeze class and
// is currently DISABLED (rec_wasm.cpp `#if EXECUTOR_MODE == 7 || 1`), costing
// ~50% perf. Every prior harness (208 Layer-1 + 320 SingleStepTests) validates
// SINGLE-block / per-instruction behavior. NONE exercises cross-block state flow
// inside a compiled chain. That is the gap this harness closes.
//
// Method — differential dual-path:
//   Path A (under test): compile the connected blocks as ONE module via
//     buildMultiBlockModule (the production chain path), run via
//     wasm_execute_block. Registers stay live in shared WASM locals across
//     block boundaries; routing is by internal dispatch index.
//   Path B (reference): compile each block SINGLE-block via buildBlockModule,
//     prime the dispatch table, run via c_dispatch_loop. Every block boundary
//     round-trips ALL registers through ctx memory (the proven-correct path).
//
//   Seed ctx → run Path A → snapshot → restore seed → run Path B → snapshot →
//   diff the two Sh4Context snapshots. ANY architectural-state divergence names
//   a chain bug. (Same dual-path pattern as wasm_test_shil_ops.h, but the unit
//   under test is the chain, not a single op.)
//
// Suspects this targets (see memory project_chain_code_audit.md):
//   #2  COND both-targets-in-chain routing (discoverChain's "safe" comment is
//       provably false — the variant IS reachable).
//   #1  (coherency half) register / sr.T flow across a chain boundary when a
//       mid-chain fallback (shil_fb) reads/writes ctx memory directly.
//   #3  hand-computed br depths in the 5 COND routing variants.
//
// Phase 1 (this file): non-throwing tests. If any diverge, the bug is found
// without the fragile exception-throw path. Phase 2 (added only if Phase 1 is
// clean) will add a mid-chain shop_illegal that throws SH4ThrownException,
// replicating the mainloop's try/catch + Do_Exception flow for both paths.
//
// Included after wasm_test_shil_ops.h / wasm_test_dispatch.h, so it reuses
// mkOp/mkReg/mkImm, seedContext, clearDispatchSlot, teardownBlock,
// primeDispatchEntry, discoverChain, buildMultiBlockModule, buildBlockModule.
//
// Gated #ifndef JIT_PROD_BUILD. Entry point: chain_test_harness() → #failures.

#pragma once

// ============================================================
// Chain test helpers
// ============================================================

// A vaddr region in P2 area 5 (phys area != 3) so the dispatch SMC check and
// the interior-block SMC guard are both skipped — keeps these tests focused on
// chain control/data flow, not SMC (already covered by wasm_test_dispatch.h).
static constexpr u32 CHAIN_BASE = 0xBED00000;
// A "park" PC with no registered block. A terminal dynamic jump lands here;
// Path A exits the module, Path B misses in c_dispatch_loop. Both stop with
// ctx.pc == CHAIN_PARK.
static constexpr u32 CHAIN_PARK = 0xBEC00000;

// Build a block at an explicit vaddr with explicit end-type + targets + ops.
// (makeBlockAtPC forces a self-loop StaticJump; chains need arbitrary links.)
static RuntimeBlockInfo* makeChainBlock(u32 vaddr, BlockEndType blockType,
                                        u32 branchBlock, u32 nextBlock,
                                        const std::vector<shil_opcode>& ops,
                                        bool has_jcond = false) {
    RuntimeBlockInfo* block = new RuntimeBlockInfo();
    block->vaddr = vaddr;
    block->addr = vaddr;
    block->BlockType = blockType;
    block->BranchBlock = branchBlock;
    block->NextBlock = nextBlock;
    block->guest_cycles = 1;
    block->guest_opcodes = (u32)ops.size();
    block->host_opcodes = 0;
    block->host_code_size = 0;
    block->sh4_code_size = 2;
    block->has_jcond = has_jcond;
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
    for (const auto& op : ops) block->oplist.push_back(op);
    blockByVaddr[vaddr] = block;
    return block;
}

// Terminal dynamic-jump ops: load CHAIN_PARK into a scratch GPR, jdyn = it.
// Mirrors the decoder's `jmp @Rn` output. Exits both chain and single paths.
static std::vector<shil_opcode> chainTerminalOps() {
    return {
        mkOp(shop_mov32, mkReg(reg_r14), mkImm(CHAIN_PARK)),
        mkOp(shop_jdyn,  mkReg(reg_pc_dyn), mkReg(reg_r14)),
    };
}

// Path A setup: compile `chain` as ONE multi-block module, prime ONLY the entry
// vaddr in the dispatch table. Execution is driven by runDispatchExc via
// c_dispatch_loop's call_indirect — the SAME invocation path production uses
// (NOT wasm_execute_block, whose JS frame would not propagate a C++
// SH4ThrownException as its original type). Returns false on compile failure.
static bool setupChainModule(const std::vector<RuntimeBlockInfo*>& chain) {
#ifdef __EMSCRIPTEN__
    WasmModuleBuilder b;
    if (!buildMultiBlockModule(b, chain)) {
        EM_ASM({ console.error('[CHAIN-TEST]   setupChainModule: ' +
            'buildMultiBlockModule false (len=' + $0 + ')'); }, (u32)chain.size());
        return false;
    }
    const std::vector<u8>& bytes = b.getBytes();
    int idx = wasm_compile_block(bytes.data(), (u32)bytes.size(), chain[0]->vaddr);
    if (idx <= 0) {
        EM_ASM({ console.error('[CHAIN-TEST]   setupChainModule: wasm_compile_block=' +
            $0 + ' (bytes=' + $1 + ', len=' + $2 + ', entry=0x' +
            ($3>>>0).toString(16) + ')'); },
            idx, (u32)bytes.size(), (u32)chain.size(), chain[0]->vaddr);
        return false;
    }
    primeDispatchEntry(chain[0]->vaddr, chain[0]->sh4_code_size, (u32)idx);
    return true;
#else
    (void)chain; return false;
#endif
}

// Path B setup: compile every block SINGLE-block, prime each in the dispatch
// table. c_dispatch_loop walks the chain block-by-block (round-tripping all regs
// through ctx memory at each boundary) — the proven-correct reference.
static bool setupSingleBlockModules(const std::vector<RuntimeBlockInfo*>& blocks) {
#ifdef __EMSCRIPTEN__
    for (auto* blk : blocks) {
        WasmModuleBuilder b;
        if (!buildBlockModule(b, blk)) return false;
        const std::vector<u8>& bytes = b.getBytes();
        int idx = wasm_compile_block(bytes.data(), (u32)bytes.size(), blk->vaddr);
        if (idx <= 0) return false;
        primeDispatchEntry(blk->vaddr, blk->sh4_code_size, (u32)idx);
    }
    return true;
#else
    (void)blocks; return false;
#endif
}

// Drive c_dispatch_loop from entry_pc with the SAME exception/trap handling the
// mainloop uses (rec_wasm.cpp:3554). Returns: 0 = normal exit, 1 = SH4
// exception delivered (Do_Exception), 2 = WASM trap. Both paths use this, so a
// difference in outcome OR resulting ctx is a real chain divergence.
static int runDispatchExc(u32 entry_pc) {
#ifdef __EMSCRIPTEN__
    Sh4cntx.pc = entry_pc;
    Sh4cntx.cycle_counter = 1000;
    g_dispatch_result = 0xDEADBEEF;
    g_dispatch_miss_pc = 0;
    u32 ctx_ptr = (u32)(uintptr_t)&Sh4cntx;
    u32 ram_base = (u32)(uintptr_t)&mem_b[0];
    try {
        c_dispatch_loop(ctx_ptr, ram_base);
        return 0;
    } catch (const SH4ThrownException& ex) {
        Do_Exception(ex.epc, ex.expEvn);
        Sh4cntx.cycle_counter += 5;
        return 1;
    } catch (...) {
        return 2;
    }
#else
    (void)entry_pc; return -1;
#endif
}

// Compare architectural state of two Sh4Context snapshots. Logs each diverging
// field. Returns number of diverging fields (0 = chain matches single-block).
static int diffChainCtx(const Sh4Context& a, const Sh4Context& b,
                        const char* tname) {
    int diffs = 0;
    auto rep = [&](const char* field, u32 va, u32 vb) {
        diffs++;
        EM_ASM({ console.error('[CHAIN-TEST]   DIVERGE ' + UTF8ToString($0) +
            '.' + UTF8ToString($1) +
            ' chain=0x' + ($2>>>0).toString(16) +
            ' single=0x' + ($3>>>0).toString(16)); },
            tname, field, va, vb);
    };
#define CHK(f) do { if ((u32)(a.f) != (u32)(b.f)) rep(#f, (u32)a.f, (u32)b.f); } while (0)
    CHK(pc); CHK(pr); CHK(jdyn); CHK(spc); CHK(ssr); CHK(sgr);
    CHK(gbr); CHK(vbr); CHK(dbr); CHK(fpul); CHK(cycle_counter);
    CHK(sr.status); CHK(sr.T); CHK(fpscr.full); CHK(interrupt_pend);
#undef CHK
    if ((u64)a.mac.full != (u64)b.mac.full)
        rep("mac.full", (u32)a.mac.full, (u32)b.mac.full);
    for (int i = 0; i < 16; i++)
        if (a.r[i] != b.r[i]) {
            diffs++;
            EM_ASM({ console.error('[CHAIN-TEST]   DIVERGE ' + UTF8ToString($0) +
                '.r[' + $1 + '] chain=0x' + ($2>>>0).toString(16) +
                ' single=0x' + ($3>>>0).toString(16)); },
                tname, i, a.r[i], b.r[i]);
        }
    for (int i = 0; i < 8; i++)
        if (a.r_bank[i] != b.r_bank[i]) {
            diffs++;
            EM_ASM({ console.error('[CHAIN-TEST]   DIVERGE ' + UTF8ToString($0) +
                '.r_bank[' + $1 + '] chain=0x' + ($2>>>0).toString(16) +
                ' single=0x' + ($3>>>0).toString(16)); },
                tname, i, a.r_bank[i], b.r_bank[i]);
        }
    if (memcmp(a.fr, b.fr, sizeof(a.fr)) != 0 ||
        memcmp(a.xf, b.xf, sizeof(a.xf)) != 0) {
        diffs++;
        EM_ASM({ console.error('[CHAIN-TEST]   DIVERGE ' + UTF8ToString($0) +
            '.fr/xf (float bank mismatch)'); }, tname);
    }
    return diffs;
}

// Run one differential test: build blocks via `build`, run both paths from the
// same seed, diff. `build` populates `blocks` (execution order) and returns the
// entry vaddr; the chain is rediscovered via discoverChain to match production.
// `seed` applies test-specific ctx state AFTER seedContext().
typedef u32 (*ChainBuildFn)(std::vector<RuntimeBlockInfo*>& blocks);
typedef void (*ChainSeedFn)();

static int runChainDifferential(const char* tname, ChainBuildFn build,
                                ChainSeedFn seed) {
    std::vector<RuntimeBlockInfo*> blocks;
    u32 entry = build(blocks);

    // Validate discoverChain sees the chain we intended (entry must chain).
    std::vector<RuntimeBlockInfo*> chain = discoverChain(blocks[0]);

    // --- Path A: chain ---
    seedContext();
    seed();
    Sh4Context seedSnap;
    memcpy(&seedSnap, &Sh4cntx, sizeof(Sh4Context));

    if (!setupChainModule(chain)) {
        EM_ASM({ console.error('[CHAIN-TEST] FAIL ' + UTF8ToString($0) +
            ' (chain compile)'); }, tname);
        for (auto* blk : blocks) teardownBlock(blk);
        return 1;
    }
    int chainOutcome = runDispatchExc(entry);
    Sh4Context chainSnap;
    memcpy(&chainSnap, &Sh4cntx, sizeof(Sh4Context));

    // Remove chain module from JS cache before single-block recompiles the
    // entry vaddr (same key) as a single-block module.
    for (auto* blk : blocks) {
#ifdef __EMSCRIPTEN__
        wasm_remove_block(blk->vaddr);
#endif
        clearDispatchSlot(blk->vaddr);
    }

    // --- Path B: single-block reference ---
    memcpy(&Sh4cntx, &seedSnap, sizeof(Sh4Context));
    if (!setupSingleBlockModules(blocks)) {
        EM_ASM({ console.error('[CHAIN-TEST] FAIL ' + UTF8ToString($0) +
            ' (single-block compile)'); }, tname);
        for (auto* blk : blocks) teardownBlock(blk);
        return 1;
    }
    int singleOutcome = runDispatchExc(entry);
    Sh4Context singleSnap;
    memcpy(&singleSnap, &Sh4cntx, sizeof(Sh4Context));

    int diffs = diffChainCtx(chainSnap, singleSnap, tname);
    if (chainOutcome != singleOutcome) {
        diffs++;
        EM_ASM({ console.error('[CHAIN-TEST]   DIVERGE ' + UTF8ToString($0) +
            ' OUTCOME chain=' + $1 + ' single=' + $2 +
            ' (0=normal,1=SH4exc,2=trap)'); }, tname, chainOutcome, singleOutcome);
    }

    if (diffs == 0) {
        EM_ASM({ console.log('[CHAIN-TEST] PASS ' + UTF8ToString($0) +
            ' (chain_len=' + $1 + ')'); }, tname, (u32)chain.size());
    } else {
        EM_ASM({ console.error('[CHAIN-TEST] FAIL ' + UTF8ToString($0) +
            ' — ' + $1 + ' field(s) diverge (chain_len=' + $2 + ')'); },
            tname, diffs, (u32)chain.size());
    }

    for (auto* blk : blocks) teardownBlock(blk);
    return diffs == 0 ? 0 : 1;
}

// ============================================================
// Test 1: regflow_3block — register dataflow across a 3-block static chain
// block0: r0 = 5            (StaticJump → block1)
// block1: r1 = r0 + r0      (reads r0 written by block0; StaticJump → block2)
// block2: r2 = r1 + r1; terminal dynamic jump → PARK
// Chain keeps r0/r1 in shared locals; single-block round-trips via memory.
// Both must agree on r0=5, r1=10, r2=20, pc=PARK.
// ============================================================
static u32 build_regflow_3block(std::vector<RuntimeBlockInfo*>& blocks) {
    const u32 A = CHAIN_BASE + 0x000;
    const u32 B = CHAIN_BASE + 0x010;
    const u32 C = CHAIN_BASE + 0x020;
    clearDispatchSlot(A); clearDispatchSlot(B); clearDispatchSlot(C);
    blockByVaddr.erase(A); blockByVaddr.erase(B); blockByVaddr.erase(C);

    blocks.push_back(makeChainBlock(A, BET_StaticJump, B, B,
        { mkOp(shop_mov32, mkReg(reg_r0), mkImm(5)) }));
    blocks.push_back(makeChainBlock(B, BET_StaticJump, C, C,
        { mkOp(shop_add, mkReg(reg_r1), mkReg(reg_r0), mkReg(reg_r0)) }));
    std::vector<shil_opcode> c_ops =
        { mkOp(shop_add, mkReg(reg_r2), mkReg(reg_r1), mkReg(reg_r1)) };
    for (auto& op : chainTerminalOps()) c_ops.push_back(op);
    blocks.push_back(makeChainBlock(C, BET_DynamicJump, 0, 0, c_ops));
    return A;
}
static void seed_noop() {}

// ============================================================
// Test 2a/2b: cond_taken_target_in_chain — COND with taken target chained,
// fall-through exits. Exercises the "only branch target in chain" routing
// variant (rec_wasm.cpp:2887) for both T=1 (taken) and T=0 (fall-through).
// block0: set r3=0xAA; COND (sr.T), BranchBlock=B (chained), NextBlock=PARK
// block1: r4 = r3 + r3; terminal dynamic → PARK
// ============================================================
static u32 build_cond_taken(std::vector<RuntimeBlockInfo*>& blocks) {
    const u32 A = CHAIN_BASE + 0x100;
    const u32 B = CHAIN_BASE + 0x110;
    clearDispatchSlot(A); clearDispatchSlot(B);
    blockByVaddr.erase(A); blockByVaddr.erase(B);

    blocks.push_back(makeChainBlock(A, BET_Cond_1, B, CHAIN_PARK,
        { mkOp(shop_mov32, mkReg(reg_r3), mkImm(0xAA)) }, /*has_jcond*/false));
    std::vector<shil_opcode> b_ops =
        { mkOp(shop_add, mkReg(reg_r4), mkReg(reg_r3), mkReg(reg_r3)) };
    for (auto& op : chainTerminalOps()) b_ops.push_back(op);
    blocks.push_back(makeChainBlock(B, BET_DynamicJump, 0, 0, b_ops));
    return A;
}
static void seed_T1() { Sh4cntx.sr.T = 1; }
static void seed_T0() { Sh4cntx.sr.T = 0; }

// ============================================================
// Test 3a/3b: cond_both_targets_in_chain — the suspect #2 path. A COND whose
// taken AND fall-through targets both resolve in the chain, activating the
// "both targets in chain" routing variant (rec_wasm.cpp:2873) whose else-path
// has NO pc-equality guard.
// block0 @A: COND(sr.T) BranchBlock=B, NextBlock=C
// block1 @B: COND(sr.T) BranchBlock=C, NextBlock=PARK  (pulls C into chain)
// block2 @C: terminal dynamic → PARK
// discoverChain(A) = [A,B,C]; A sees both B and C in pcToIdx.
//   T=1: A→B→C→PARK ; T=0: A→C→PARK (B skipped)
// ============================================================
static u32 build_cond_both(std::vector<RuntimeBlockInfo*>& blocks) {
    const u32 A = CHAIN_BASE + 0x200;
    const u32 B = CHAIN_BASE + 0x210;
    const u32 C = CHAIN_BASE + 0x220;
    clearDispatchSlot(A); clearDispatchSlot(B); clearDispatchSlot(C);
    blockByVaddr.erase(A); blockByVaddr.erase(B); blockByVaddr.erase(C);

    blocks.push_back(makeChainBlock(A, BET_Cond_1, B, C,
        { mkOp(shop_mov32, mkReg(reg_r5), mkImm(0x50)) }, false));
    blocks.push_back(makeChainBlock(B, BET_Cond_1, C, CHAIN_PARK,
        { mkOp(shop_mov32, mkReg(reg_r6), mkImm(0x60)) }, false));
    std::vector<shil_opcode> c_ops =
        { mkOp(shop_mov32, mkReg(reg_r7), mkImm(0x70)) };
    for (auto& op : chainTerminalOps()) c_ops.push_back(op);
    blocks.push_back(makeChainBlock(C, BET_DynamicJump, 0, 0, c_ops));
    return A;
}

// ============================================================
// Test 4: srt_across_boundary — sr.T written by block0, consumed by block1's
// COND exit. sr.T is cached in a shared local across the boundary in the chain
// path; in single-block it round-trips through ctx.sr.T memory. Targets the
// "sr.T cached but sr.status not" coherency concern (suspect #1).
// block0 @A: sr.T = 1 ; StaticJump → B
// block1 @B: COND(sr.T) BranchBlock=C (chained), NextBlock=PARK
// block2 @C: terminal dynamic → PARK
// Correct: T=1 → A→B→C→PARK in both paths.
// ============================================================
static u32 build_srt_across(std::vector<RuntimeBlockInfo*>& blocks) {
    const u32 A = CHAIN_BASE + 0x300;
    const u32 B = CHAIN_BASE + 0x310;
    const u32 C = CHAIN_BASE + 0x320;
    clearDispatchSlot(A); clearDispatchSlot(B); clearDispatchSlot(C);
    blockByVaddr.erase(A); blockByVaddr.erase(B); blockByVaddr.erase(C);

    blocks.push_back(makeChainBlock(A, BET_StaticJump, B, B,
        { mkOp(shop_mov32, mkReg(reg_sr_T), mkImm(1)) }));
    blocks.push_back(makeChainBlock(B, BET_Cond_1, C, CHAIN_PARK,
        { mkOp(shop_mov32, mkReg(reg_r8), mkImm(0x88)) }, false));
    blocks.push_back(makeChainBlock(C, BET_DynamicJump, 0, 0, chainTerminalOps()));
    return A;
}

// ============================================================
// Test 5: fallback_midchain_noexc — interior block contains shop_sync_sr, which
// forces a full flush → shil_fb → reload (wasm_emit.h:1463) MID-CHAIN. Verifies
// the shared register cache stays coherent with the C++ fallback's direct
// ctx-memory view across a boundary, WITHOUT the fragile exception-throw path.
// This is the non-throwing proxy for suspect #1.
// block0 @A: r9 = 0xABC ; StaticJump → B
// block1 @B: shop_sync_sr (fallback flush/reload) ; StaticJump → C
// block2 @C: r10 = r9 + r9 (must still see r9) ; terminal dynamic → PARK
// Seed sr so UpdateSR is a no-op bank-wise (no RB change, no pending int).
// ============================================================
static u32 build_fallback_midchain(std::vector<RuntimeBlockInfo*>& blocks) {
    const u32 A = CHAIN_BASE + 0x400;
    const u32 B = CHAIN_BASE + 0x410;
    const u32 C = CHAIN_BASE + 0x420;
    clearDispatchSlot(A); clearDispatchSlot(B); clearDispatchSlot(C);
    blockByVaddr.erase(A); blockByVaddr.erase(B); blockByVaddr.erase(C);

    blocks.push_back(makeChainBlock(A, BET_StaticJump, B, B,
        { mkOp(shop_mov32, mkReg(reg_r9), mkImm(0xABC)) }));
    blocks.push_back(makeChainBlock(B, BET_StaticJump, C, C,
        { mkOp(shop_sync_sr) }));
    std::vector<shil_opcode> c_ops =
        { mkOp(shop_add, mkReg(reg_r10), mkReg(reg_r9), mkReg(reg_r9)) };
    for (auto& op : chainTerminalOps()) c_ops.push_back(op);
    blocks.push_back(makeChainBlock(C, BET_DynamicJump, 0, 0, c_ops));
    return A;
}
static void seed_clean_sr() {
    // MD=1, RB=0, BL=0, IMASK=0 — UpdateSR won't swap banks or deliver an int.
    Sh4cntx.sr.status = 0x40000000;
    Sh4cntx.old_sr.status = Sh4cntx.sr.status;
    Sh4cntx.sr.T = 0;
    Sh4cntx.interrupt_pend = 0;
}

// ============================================================
// PHASE 2 — the paths Phase 1 deliberately skipped
// ============================================================

// Test 6: exc_midchain — PRIME SUSPECT. An interior block raises an SH4
// illegal-instruction exception (shop_illegal → shil_fb throws
// SH4ThrownException). Both paths catch it and call Do_Exception exactly as the
// mainloop does. If the chain leaves ctx (SPC/SSR/SR/PC/regs) different from
// single-block after delivery, that is the DOA2/VT "RTE to garbage" bug.
// block0 @A: r11 = 0x111 ; StaticJump → B
// block1 @B: shop_illegal(epc=B) ; (throws before reaching its exit)
// block2 @C: r12 = 0x222 ; terminal dynamic → PARK  (never reached)
static u32 build_exc_midchain(std::vector<RuntimeBlockInfo*>& blocks) {
    const u32 A = CHAIN_BASE + 0x500;
    const u32 B = CHAIN_BASE + 0x510;
    const u32 C = CHAIN_BASE + 0x520;
    clearDispatchSlot(A); clearDispatchSlot(B); clearDispatchSlot(C);
    blockByVaddr.erase(A); blockByVaddr.erase(B); blockByVaddr.erase(C);

    blocks.push_back(makeChainBlock(A, BET_StaticJump, B, B,
        { mkOp(shop_mov32, mkReg(reg_r11), mkImm(0x111)) }));
    // shop_illegal: rs1 = epc, rs2 = delaySlot flag (see rec_wasm.cpp:763)
    blocks.push_back(makeChainBlock(B, BET_StaticJump, C, C,
        { mkOp(shop_illegal, shil_param(), mkImm(B), mkImm(0)) }));
    std::vector<shil_opcode> c_ops =
        { mkOp(shop_mov32, mkReg(reg_r12), mkImm(0x222)) };
    for (auto& op : chainTerminalOps()) c_ops.push_back(op);
    blocks.push_back(makeChainBlock(C, BET_DynamicJump, 0, 0, c_ops));
    return A;
}
static void seed_exc() {
    Sh4cntx.sr.status = 0x40000000;   // MD=1, BL=0 — exception can be delivered
    Sh4cntx.old_sr.status = Sh4cntx.sr.status;
    Sh4cntx.sr.T = 0;
    Sh4cntx.vbr = 0x8C001000;         // exception vector base
    Sh4cntx.interrupt_pend = 0;
}

// Test 7: chain_ram_smc — chain in area-3 RAM, which activates the interior SMC
// guard (rec_wasm.cpp:2773, skipped by the area-5 Phase-1 tests). RAM is not
// modified, so the guard's runtime re-hash must match the compile-time hash and
// the chain must run to completion identically to single-block. High RAM
// addresses chosen to avoid colliding with live game code.
static u32 build_ram_smc(std::vector<RuntimeBlockInfo*>& blocks) {
    const u32 A = 0x0CF00000;  // area-3 RAM (phys>>26 == 3)
    const u32 B = 0x0CF00010;
    const u32 C = 0x0CF00020;
    clearDispatchSlot(A); clearDispatchSlot(B); clearDispatchSlot(C);
    blockByVaddr.erase(A); blockByVaddr.erase(B); blockByVaddr.erase(C);

    blocks.push_back(makeChainBlock(A, BET_StaticJump, B, B,
        { mkOp(shop_mov32, mkReg(reg_r0), mkImm(0xAAA)) }));
    blocks.push_back(makeChainBlock(B, BET_StaticJump, C, C,
        { mkOp(shop_add, mkReg(reg_r1), mkReg(reg_r0), mkReg(reg_r0)) }));
    std::vector<shil_opcode> c_ops =
        { mkOp(shop_mov32, mkReg(reg_r2), mkImm(0xCCC)) };
    for (auto& op : chainTerminalOps()) c_ops.push_back(op);
    blocks.push_back(makeChainBlock(C, BET_DynamicJump, 0, 0, c_ops));
    return A;
}

// Test 8: chain_long6 — a 6-block static chain (within MULTIBLOCK_MAX=8), each
// block writing a distinct register, with a final cross-block read (r8 = r0 +
// r4). Stresses deep register-cache propagation and the dispatch-index loop.
static u32 build_long6(std::vector<RuntimeBlockInfo*>& blocks) {
    const u32 base = CHAIN_BASE + 0x600;
    u32 v[6];
    for (int i = 0; i < 6; i++) {
        v[i] = base + (u32)i * 0x10;
        clearDispatchSlot(v[i]);
        blockByVaddr.erase(v[i]);
    }
    for (int i = 0; i < 5; i++)
        blocks.push_back(makeChainBlock(v[i], BET_StaticJump, v[i + 1], v[i + 1],
            { mkOp(shop_mov32, mkReg((Sh4RegType)(reg_r0 + i)), mkImm(0x10 + i)) }));
    std::vector<shil_opcode> last =
        { mkOp(shop_add, mkReg(reg_r8), mkReg(reg_r0), mkReg(reg_r4)) };
    for (auto& op : chainTerminalOps()) last.push_back(op);
    blocks.push_back(makeChainBlock(v[5], BET_DynamicJump, 0, 0, last));
    return v[0];
}

// Test 9: chain_hotloop — THE representative chain case (all prior tests are
// acyclic; chaining exists FOR hot loops, per discoverChain's comment). A
// 2-block loop A⇄B that decrements a counter and exits via COND when it hits 0.
// The multi-block module loops internally (B's fall-through routes back to A's
// dispatch index); single-block round-trips the counter through ctx memory each
// iteration. Both must agree on the final register state + exit PC.
// block A @A: r0 -= 1                         ; StaticJump → B
// block B @B: sr.T = (r0 == 0) ; COND_1: T=1 → PARK (exit), T=0 → A (loop)
static u32 build_hotloop(std::vector<RuntimeBlockInfo*>& blocks) {
    const u32 A = CHAIN_BASE + 0x700;
    const u32 B = CHAIN_BASE + 0x710;
    clearDispatchSlot(A); clearDispatchSlot(B);
    blockByVaddr.erase(A); blockByVaddr.erase(B);

    blocks.push_back(makeChainBlock(A, BET_StaticJump, B, B,
        { mkOp(shop_sub, mkReg(reg_r0), mkReg(reg_r0), mkImm(1)) }));
    // BET_Cond_1: branch (→PARK, exit) when sr.T==1; fall-through (→A, loop) when T==0
    blocks.push_back(makeChainBlock(B, BET_Cond_1, CHAIN_PARK, A,
        { mkOp(shop_seteq, mkReg(reg_sr_T), mkReg(reg_r0), mkImm(0)) }, /*has_jcond*/false));
    return A;
}
static void seed_loop_count() { Sh4cntx.r[0] = 5; }  // 5 iterations then exit

// ============================================================
// Main driver
// ============================================================
static int chain_test_harness() {
    Sh4Context saved_ctx;
    memcpy(&saved_ctx, &Sh4cntx, sizeof(Sh4Context));

    EM_ASM({ console.log('[CHAIN-TEST] Starting Layer 2 multi-block chain ' +
        'differential tests'); });

    int fail = 0;
    int total = 0;

    total++; fail += runChainDifferential("regflow_3block",
        build_regflow_3block, seed_noop);

    total++; fail += runChainDifferential("cond_taken_T1",
        build_cond_taken, seed_T1);
    total++; fail += runChainDifferential("cond_taken_T0",
        build_cond_taken, seed_T0);

    total++; fail += runChainDifferential("cond_both_targets_T1",
        build_cond_both, seed_T1);
    total++; fail += runChainDifferential("cond_both_targets_T0",
        build_cond_both, seed_T0);

    total++; fail += runChainDifferential("srt_across_boundary",
        build_srt_across, seed_noop);

    total++; fail += runChainDifferential("fallback_midchain_noexc",
        build_fallback_midchain, seed_clean_sr);

    // Phase 2 — exception, RAM/SMC guard, deep chain
    total++; fail += runChainDifferential("exc_midchain",
        build_exc_midchain, seed_exc);
    total++; fail += runChainDifferential("chain_ram_smc",
        build_ram_smc, seed_noop);
    total++; fail += runChainDifferential("chain_long6",
        build_long6, seed_noop);
    total++; fail += runChainDifferential("chain_hotloop",
        build_hotloop, seed_loop_count);

    memcpy(&Sh4cntx, &saved_ctx, sizeof(Sh4Context));

    EM_ASM({
        console.log('[CHAIN-TEST] ============================');
        console.log('[CHAIN-TEST] TOTAL: ' + $0 + '  PASS: ' + ($0-$1) +
            '  FAIL: ' + $1);
        console.log('[CHAIN-TEST] ============================');
    }, total, fail);

    return fail;
}
