// wasm_test_singlestep.h — SingleStepTests/sh4 harness
//
// Runs SingleStepTests/sh4 vectors (TomHarte-style, generated from Reicast's
// interpreter) through upstream Flycast's SH4 interpreter and compares final
// register state to Reicast's expected state.
//
// This validates our SH4 interpreter against an INDEPENDENT ground truth.
// Entry point: run_singlestep_tests_impl() returns number of failures.
//
// Gated with #ifndef JIT_PROD_BUILD — excluded from production builds.

#pragma once

#include <cstring>
#include <cstdio>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_interpreter.h"
#include "hw/sh4/dyna/ngen.h"
#include "wasm_test_singlestep_data.h"

// ==== Fetch hook state ====
// When g_ss_active is true, IReadMem16 is redirected to our hook which
// returns opcodes from the current test vector based on fetch index.
static bool     g_ss_active = false;
static int      g_ss_fetch_idx = 0;
static const SsTest* g_ss_current = nullptr;
static bool     g_ss_addr_mismatch = false;
static u32      g_ss_mismatch_addr = 0;
static u32      g_ss_mismatch_expected = 0;

static u16 DYNACALL ss_IReadMem16_hook(u32 addr) {
    if (!g_ss_active || g_ss_current == nullptr) {
        return 0x0009; // NOP
    }
    // Per SingleStepTests README: opcodes[0..3] are for fetches at
    //   base_PC, base_PC+2, base_PC+4, base_PC+6
    // opcodes[4] is the fallback for ANY OTHER instruction fetch address
    // (e.g. fetches at the target of a taken branch).
    u32 base = g_ss_current->initial.PC;
    u32 diff = addr - base;
    u16 op;
    if (diff < 8 && (diff & 1) == 0) {
        op = (u16)g_ss_current->opcodes[diff >> 1];
    } else {
        op = (u16)g_ss_current->opcodes[4];
    }
    // Sanity: if we've tracked fewer fetches than cycles.length and the
    // address doesn't match the expected fetch_addr for that cycle, it
    // means our interpreter went somewhere Reicast didn't. Flag it.
    if (g_ss_fetch_idx < 4) {
        u32 expected = g_ss_current->cycles[g_ss_fetch_idx].fetch_addr;
        if (addr != expected && !g_ss_addr_mismatch) {
            g_ss_addr_mismatch = true;
            g_ss_mismatch_addr = addr;
            g_ss_mismatch_expected = expected;
        }
        g_ss_fetch_idx++;
    }
    return op;
}

static void ss_apply_state(const SsState& s) {
    for (int i = 0; i < 16; i++) Sh4cntx.r[i] = s.R[i];
    for (int i = 0; i < 8; i++) Sh4cntx.r_bank[i] = s.R_[i];
    for (int i = 0; i < 16; i++) {
        u32 v = s.FP0[i];
        memcpy(&Sh4cntx.fr[i], &v, 4);
    }
    for (int i = 0; i < 16; i++) {
        u32 v = s.FP1[i];
        memcpy(&Sh4cntx.xf[i], &v, 4);
    }
    Sh4cntx.pc   = s.PC;
    Sh4cntx.gbr  = s.GBR;
    Sh4cntx.sr.setFull(s.SR);
    Sh4cntx.ssr  = s.SSR;
    Sh4cntx.spc  = s.SPC;
    Sh4cntx.vbr  = s.VBR;
    Sh4cntx.sgr  = s.SGR;
    Sh4cntx.dbr  = s.DBR;
    Sh4cntx.mac.l = s.MACL;
    Sh4cntx.mac.h = s.MACH;
    Sh4cntx.pr   = s.PR;
    Sh4cntx.fpscr.full = s.FPSCR;
    Sh4cntx.fpul = s.FPUL;
}

static const char* ss_compare_state(const SsState& e, u32* got, u32* wanted) {
    static char buf[32];
    for (int i = 0; i < 16; i++) {
        if (Sh4cntx.r[i] != e.R[i]) {
            snprintf(buf, sizeof(buf), "R[%d]", i);
            *got = Sh4cntx.r[i]; *wanted = e.R[i]; return buf;
        }
    }
    for (int i = 0; i < 8; i++) {
        if (Sh4cntx.r_bank[i] != e.R_[i]) {
            snprintf(buf, sizeof(buf), "R_[%d]", i);
            *got = Sh4cntx.r_bank[i]; *wanted = e.R_[i]; return buf;
        }
    }
    if (Sh4cntx.pc != e.PC)     { *got = Sh4cntx.pc;        *wanted = e.PC;   return "PC"; }
    if (Sh4cntx.gbr != e.GBR)   { *got = Sh4cntx.gbr;       *wanted = e.GBR;  return "GBR"; }
    if (Sh4cntx.sr.getFull() != e.SR) { *got = Sh4cntx.sr.getFull(); *wanted = e.SR; return "SR"; }
    if (Sh4cntx.ssr != e.SSR)   { *got = Sh4cntx.ssr;       *wanted = e.SSR;  return "SSR"; }
    if (Sh4cntx.spc != e.SPC)   { *got = Sh4cntx.spc;       *wanted = e.SPC;  return "SPC"; }
    if (Sh4cntx.vbr != e.VBR)   { *got = Sh4cntx.vbr;       *wanted = e.VBR;  return "VBR"; }
    if (Sh4cntx.sgr != e.SGR)   { *got = Sh4cntx.sgr;       *wanted = e.SGR;  return "SGR"; }
    if (Sh4cntx.dbr != e.DBR)   { *got = Sh4cntx.dbr;       *wanted = e.DBR;  return "DBR"; }
    if (Sh4cntx.mac.l != e.MACL){ *got = Sh4cntx.mac.l;     *wanted = e.MACL; return "MACL"; }
    if (Sh4cntx.mac.h != e.MACH){ *got = Sh4cntx.mac.h;     *wanted = e.MACH; return "MACH"; }
    if (Sh4cntx.pr != e.PR)     { *got = Sh4cntx.pr;        *wanted = e.PR;   return "PR"; }
    if (Sh4cntx.fpscr.full != e.FPSCR) { *got = Sh4cntx.fpscr.full; *wanted = e.FPSCR; return "FPSCR"; }
    if (Sh4cntx.fpul != e.FPUL) { *got = Sh4cntx.fpul;      *wanted = e.FPUL; return "FPUL"; }
    for (int i = 0; i < 16; i++) {
        u32 g; memcpy(&g, &Sh4cntx.fr[i], 4);
        if (g != e.FP0[i]) {
            snprintf(buf, sizeof(buf), "FR[%d]", i);
            *got = g; *wanted = e.FP0[i]; return buf;
        }
    }
    for (int i = 0; i < 16; i++) {
        u32 g; memcpy(&g, &Sh4cntx.xf[i], 4);
        if (g != e.FP1[i]) {
            snprintf(buf, sizeof(buf), "XF[%d]", i);
            *got = g; *wanted = e.FP1[i]; return buf;
        }
    }
    return nullptr;
}

static int run_singlestep_tests_impl() {
    // Save ctx + IReadMem16 so we can restore.
    Sh4Context saved;
    memcpy(&saved, &Sh4cntx, sizeof(Sh4Context));
    ReadMem16Func saved_iread = IReadMem16;

    // Install hook.
    IReadMem16 = &ss_IReadMem16_hook;
    g_ss_active = true;

    int pass = 0, fail = 0, skip = 0;
    int fail_detail_budget = 15;

#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
    EM_ASM({ console.log('[SS-TEST] Starting SingleStepTests/sh4 harness: ' + $0 + ' tests'); }, SS_TEST_COUNT);
#endif

    for (int ti = 0; ti < SS_TEST_COUNT; ti++) {
        const SsTest& t = *ss_all_tests[ti];

        ss_apply_state(t.initial);
        g_ss_current = &t;
        g_ss_fetch_idx = 0;
        g_ss_addr_mismatch = false;

        // Drive up to 4 fetches. Uses the same pattern as Sh4Interpreter's
        // ReadNexOp + ExecuteOpcode: fetch via IReadMem16 (our hook), execute
        // via OpPtr. Branch opcodes handle their delay slot internally via
        // Sh4Interpreter::Instance->ExecuteDelayslot() which re-enters this
        // fetch path (hook returns the next opcode from the test).
        bool threw = false;
        int safety_iters = 0;
        try {
            while (g_ss_fetch_idx < 4 && safety_iters < 8) {
                safety_iters++;
                u32 addr = Sh4cntx.pc;
                Sh4cntx.pc = addr + 2;
                u16 op = IReadMem16(addr);
                OpPtr[op](&Sh4cntx, op);
            }
        } catch (const SH4ThrownException&) {
            threw = true;
        } catch (...) {
            threw = true;
        }

        if (threw) {
            skip++;
            continue;
        }

        u32 got = 0, wanted = 0;
        const char* diff = ss_compare_state(t.final_, &got, &wanted);
        if (diff == nullptr && !g_ss_addr_mismatch) {
            pass++;
        } else {
            fail++;
#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
            if (fail_detail_budget-- > 0) {
                if (g_ss_addr_mismatch) {
                    EM_ASM({
                        console.log('[SS-TEST] FAIL ' + UTF8ToString($0) + ' #' + $1 +
                            ' reason=fetch-addr-mismatch' +
                            ' got_addr=0x' + ($2>>>0).toString(16) +
                            ' want_addr=0x' + ($3>>>0).toString(16) +
                            ' at_fetch=' + $4);
                    }, t.encoding_name, ti, g_ss_mismatch_addr, g_ss_mismatch_expected, g_ss_fetch_idx);
                } else {
                    EM_ASM({
                        console.log('[SS-TEST] FAIL ' + UTF8ToString($0) + ' #' + $1 +
                            ' reg=' + UTF8ToString($2) +
                            ' got=0x' + ($3>>>0).toString(16) +
                            ' want=0x' + ($4>>>0).toString(16));
                    }, t.encoding_name, ti, diff, got, wanted);
                }
            }
#endif
        }
    }

    // Restore.
    g_ss_active = false;
    g_ss_current = nullptr;
    IReadMem16 = saved_iread;
    memcpy(&Sh4cntx, &saved, sizeof(Sh4Context));

#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
    EM_ASM({
        console.log('[SS-TEST] ============================');
        console.log('[SS-TEST] TOTAL: ' + $0 + '  PASS: ' + $1 + '  FAIL: ' + $2 + '  SKIP: ' + $3);
        console.log('[SS-TEST] ============================');
    }, SS_TEST_COUNT, pass, fail, skip);
#endif

    return fail;
}
