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

// FLY_RELEASE_BUILD: public-release artifact switch (build-baked via
// -DFLY_RELEASE_BUILD=1). Silences all boot/play console output and
// internal telemetry. Default 0 = dev/prod behavior unchanged.
#ifndef FLY_RELEASE_BUILD
#define FLY_RELEASE_BUILD 0
#endif

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

// ============================================================
// BUILD MODE — single switch (prevents the flag-drift that shipped a
// black-screening shadow-mode build, regression 4d0065c, 2026-06-11).
// ============================================================
//   VALIDATOR_BUILD 1 → JIT-vs-interpreter differential validator:
//       EXECUTOR_MODE 7 + FORCE_CPP_DISPATCH 1 + WASM_VAL_LOG_WRITES 1.
//       Runs both paths per block, compares regs + RAM writes, halts on the
//       first divergence with a full dump. Single-block (no chaining).
//   VALIDATOR_BUILD 0 → production WASM JIT execution (EXECUTOR_MODE 6).
// EXECUTOR_MODE / FORCE_CPP_DISPATCH below derive from this — never set them
// independently.
#define VALIDATOR_BUILD 0

// HYBRID_DISPATCH_BUILD: ISOLATION EXPERIMENT (2026-06-11). Runs the real WASM
// JIT blocks (wasm_execute_block) but dispatches them through the PROVEN-GOOD
// cpp dispatch inner loop (blockByVaddr.find + per-block interrupt check) used
// by the working INTERP_ONLY build — completely bypassing c_dispatch_loop
// (hash table, SMC hashing, collision detection). The per-block validator has
// proven WASM compute bit-exact vs the interpreter across ~1M blocks incl. MMIO
// reads + MMIO-write reg outputs, so compute is NOT the bug. This isolates the
// remaining suspect: if Skies boots cleanly here, the bug is in c_dispatch_loop;
// if it still boot-loops, it's WASM block exit/PC handling or guest_cycles
// accounting (cpp dispatch here uses the WASM block's own guest_cycles charge).
// Mutually exclusive with VALIDATOR_BUILD / INTERP_ONLY. → EXECUTOR_MODE 8.
#define HYBRID_DISPATCH_BUILD 0

// INTERP_ONLY: run the pure upstream SH4 interpreter (OpPtr) — JIT disabled —
// in our WASM environment. Isolates CPU/JIT bugs from subsystem (PVR/AICA/
// scheduler/memory) bugs: if games run correctly here (just slow), the JIT is
// the culprit; if they break the same way, the WASM subsystem port is. Renders
// at full correctness, slowly. Mutually exclusive with VALIDATOR_BUILD.
#define INTERP_ONLY 0

// LOCKSTEP_DIFF_BUILD: timeslice-level differential between the PRODUCTION
// dispatch (c_dispatch_loop + mainloop miss handler, EXECUTOR_MODE 6) and the
// glitch-free clean_dispatch_loop — the objective instrument for the JGR
// graphical-glitch hunt (2026-06-11). Each timeslice: snapshot Sh4Context, run
// the slice through PRODUCTION with every guest write logged, undo the RAM
// writes, restore the snapshot, re-run the same slice through CLEAN, then
// binary-compare registers + the full write streams (RAM, VRAM, MMIO — addr/
// size/value/order). First divergence → [LOCKSTEP] dump + halt latch. Slices
// that delivered an interrupt are compared but never halt (global INTC state
// can't be restored for the re-run, so a diff there is untrustworthy).
// Headless-friendly: no eyes, no toggle — guest state is the ground truth.
// Production execution stays call_indirect (FLY_DISPATCH_VIA_CACHE must be 0).
#define LOCKSTEP_DIFF_BUILD 0

// FLY_WRITE_WATCH: write-provenance diagnostic (2026-06-12, JGR zero-hole
// kill shot). Forces every native write through imports (WASM_VAL_LOG_WRITES)
// and logs every write to the watched staging-hole addresses with frame +
// value + writer class. Clean writes the watched bytes every frame; whatever
// production does differently at them names the failing writer.
#define FLY_WRITE_WATCH 0

// FLY_BRIDGE_SHADOW (2026-07-17): decisive differential for the async-defer
// SHIL bridge divergence (v5/v6 both broke JGR graphics; interrupt-delay and
// cycle-charge suspects individually addressed/measured without cure). Each
// deferred block is compiled synchronously but left UNPRIMED; on its first
// found-miss dispatch, the block is executed BOTH ways from the same
// Sh4Context snapshot — compiled WASM first (writes logged + undone), then
// the SHIL bridge (which stays the live result) — and the two end states are
// binary-compared. First mismatch names the diverging register/field + block.
// Headless-valid: compares state, not timing.
// RESULT (2026-07-17): found + fixed TWO real bugs — readF32 dropping float
// immediates (fb computed with 0.0), and fmac double-rounding divergence
// (native emitter vs canonical fused; both paths now unified on f64).
// Final farm run: 10K+ blocks shadowed, ZERO diffs. OFF for production.
#define FLY_BRIDGE_SHADOW 0

// FLY_CHAIN_SHADOW (2026-07-17, chaining campaign): differential for
// multi-block chain modules. Chains are discovered+compiled but heads stay
// UNPRIMED; on a head's first found-miss dispatch, the chain module and a
// sequential single-block reference execute from the same snapshot and are
// binary-compared (MMIO-touching runs skipped, validator policy). The
// REFERENCE result stays live. Farm to zero diffs before enabling chains
// for real. Implies WASM_VAL_LOG_WRITES (write log + undo).
// RESULT (2026-07-17): farm CLEAN — 5376 chains, ZERO diffs, all skips
// classified (mmio 359 / infra 149 / dev 3 / STALE 1212). Bugs found+fixed:
// COND both-targets routing (unverified else-arm), idle-loop links (missing
// soft-fast-forward), pref inline (single-block-only contract, now fb in
// chains), and the crown jewel: STALE MEMBER REFERENCES — chains had no
// invalidation channel for replaced members (collision recompiles, fpscr-
// differing redecodes) → baked fb indices/exits/gc against the wrong oplist
// = the historical chain freeze mechanism (1212 would-have-corrupted chains
// per 300s run).
#define FLY_CHAIN_SHADOW 0

// FLY_CHAINS_LIVE (2026-07-17): production chaining. The sweep driver
// discovers + compiles chains OFF the hot path (bounded per frame), heads
// prime directly to the chain module in the dispatch table, and
// fly_chain_invalidate() kills every owning chain the moment a member block
// is replaced or evicted — the invariant the shadow farm proved necessary.
#define FLY_CHAINS_LIVE 1
                            // member slots before they can miss into the
                            // region differential (starved it to ~0.5/window)
// Region compilation flags (design v0): LIVE primes hot-range region modules
// into the dispatch table; SHADOW registers them unprimed and runs the
// region-vs-singles differential on member misses (farm builds).
#define FLY_REGIONS_LIVE 0
#define FLY_REGION_SHADOW 0
#define FLY_CHAINS_ANY (FLY_CHAIN_SHADOW || FLY_CHAINS_LIVE || FLY_REGION_SHADOW || FLY_REGIONS_LIVE)

// FLY_WRITE_PARITY (2026-07-28, page-gen option 1 step 2, decision
// the design record): the differential that finally exercises the PROD-SHAPE
// write path. Every prior farm mode forces writes through imports
// (WASM_VAL_LOG_WRITES) — the inline store fast paths, live since February,
// were never differentially certified. This mode inverts bridge-shadow's
// order: the SHIL bridge (reference) runs FIRST with its writes logged via
// the WriteMem hook, gets undone, then the compiled WASM runs with writes
// INLINE exactly as prod ships — and stays live. Compared: full ctx, the
// reference's write set replayed as a byte-map against final memory
// (missing/wrong-value inline writes), and — the campaign's certification
// target — every written RAM page's g_fly_page_gen cell must have CHANGED
// across the JIT run (the emitted gen bump for inline 1/2/4 stores, the
// C-side fly_ram_written bump for import size-8 stores). Spurious-extra-
// write bugs remain the force-imports farm's job; the two modes are
// complementary. Deliberately NOT in the WASM_VAL_LOG_WRITES implication
// list below — prod-shape writes are the whole point.
// FARM RUN 4 (2026-08-01): n=20,480 / diffs=0 / gen_miss=0 with the size-8
// inline write path live. The full option-1 package is certified.
// CERTIFIED 2026-08-01: n=20,480 / diffs=0 / gen_miss=0 (write-parity-farm-3;
// farm-2's 94 diffs were a harness bug — expectation list lacked
// last-writer-wins). The emitted gen bump is exact; the insurance tick is
// formally redundant for CPU stores.
#define FLY_WRITE_PARITY 0

// ★ INSURANCE TICK RETIREMENT (2026-08-01, page-gen option 1 step 3,
// the design decision): with every write source now bumping page gens
// — emitted inline 1/2/4 bumps (write-parity-certified exact), C-side
// import bumps (size-8, MMIO-fallback), and the block writers (DMA/SQ) —
// gen-change detection is complete and the 1/64 forced-hash insurance is
// pure overhead: ~28-38K hashed halfwords/frame (the "insurance premium",
// audit-verified as ≈ dispatches/64 × block size) + a per-dispatch u8 RMW
// + a per-chain-guard cell RMW. 1 = legacy belt-and-braces (pre-bump
// behavior); 0 = exact contract, tick retired.
#define FLY_SMC_INSURANCE_TICK 0

#if VALIDATOR_BUILD || LOCKSTEP_DIFF_BUILD || FLY_WRITE_WATCH || FLY_BRIDGE_SHADOW || FLY_CHAIN_SHADOW || FLY_REGION_SHADOW
#define WASM_VAL_LOG_WRITES 1
#else
#define WASM_VAL_LOG_WRITES 0
#endif

#include "wasm_module_builder.h"
#include "wasm_emit.h"

#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <cstring>

#include "hw/sh4/sh4_rom.h"  // sin_table for FSCA
#include "hw/pvr/pvr_regs.h"  // PVR register macros (FB_R_CTRL etc.)

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// Unified instrumentation ring buffer. This TU owns the implementation
// (FLY_INSTRUMENT_IMPL); other includers of fly_instrument.h get decls only.
#define FLY_INSTRUMENT_IMPL
#include "fly_instrument.h"

// Set to 1 to force C++ SHIL interpreter dispatch (diagnostic, bypasses
// WASM blocks). Required for EXECUTOR_MODE 0-5 and 7 to do anything —
// otherwise the mainloop takes the WASM JIT dispatch path and
// cpp_execute_block is never called. Set to 0 for normal production.
//
// DIAGNOSTIC MODES:
//   EXECUTOR_MODE 5 + FORCE_CPP_DISPATCH 1 → SHIL-vs-ref shadow.
//       Logs [SHADOW] MISMATCH for any SHIL interpreter bug.
//   EXECUTOR_MODE 7 + FORCE_CPP_DISPATCH 1 → JIT-vs-ref shadow.
//       Runs each block through the native WASM JIT AND the reference
//       interpreter, compares ctx, logs [SHADOW-JIT] MISMATCH with
//       block PC + SHIL op dump for the first 10 hits. This is the
//       primary tool for finding native-emit bugs at the subsystem
//       level — the actual methodology the project switched to.
// Derived from build switches. Validator and interpreter-only both use the
// cpp dispatch path (no WASM JIT execution); production uses the JIT.
#if VALIDATOR_BUILD || INTERP_ONLY || HYBRID_DISPATCH_BUILD
#define FORCE_CPP_DISPATCH 1
#else
#define FORCE_CPP_DISPATCH 0
#endif

// ISOLATION TEST toggle (2026-06-11): when 1, c_dispatch_loop (mode 6) executes
// each block via wasm_execute_block(pc) [_wasmBlockCache, by exact PC — same as
// the clean mode-8 hybrid] instead of call_indirect via jit_dispatch_table[key].
// Decisive test of whether the table_idx/call_indirect HIT path is the boot-loop
// bug. Set to 0 for normal production (call_indirect, faster).
#define FLY_DISPATCH_VIA_CACHE 0

// ISOLATION TEST toggle (2026-06-11): pad each mainloop frame with a wall-clock
// busy-spin to N ms (~9fps at 110), keeping EMULATED work identical. Replicates
// the slow-motion condition under which the clean (mode-8) dispatch showed NO
// graphical glitches — to decide whether the glitches are SPEED/presentation/
// timing (vanish when slow) or DISPATCH (persist when slow). 0 = no throttle.
#define FLY_THROTTLE_FRAME_MS 0

// BISECTION VARIANT A (2026-06-15): production dispatch, but misses and
// interrupts are handled INLINE inside c_dispatch_loop — no mid-timeslice
// returns to the mainloop. Isolates structural difference #3 between
// c_dispatch_loop (glitches) and clean_dispatch_loop (no glitches): the
// mainloop round-trip on every miss/interrupt. The inline miss handler is a
// faithful replica of the mainloop's (same SMC check, same execute+recompile
// order, same saved_pc dance) — ONLY the control-flow location changes.
// Verdict by user eyes on JGR: glitches gone → #3 is the mechanism;
// unchanged → bisect #1 (lookup) / #2 (SMC granularity) next.
#define FLY_INLINE_DISPATCH 0   // Variant A REFUTED by eyes 2026-06-15: inline handling, same glitches

// BISECTION VARIANT B (2026-06-15): the CLEAN loop (user-confirmed glitch-free
// baseline) becomes the production dispatcher, with ONE change — its block
// lookup + SMC fingerprint source use the hash-table mechanism
// (jit_dispatch_table/pc/hash/sz slots) instead of blockByVaddr/g_block_smc
// maps. Everything else stays clean-loop. Eyes verdict: glitches appear →
// the table/slot mechanism is the culprit; still clean → the loops are
// functionally identical and the difference hunt moves to a line-by-line diff.
#define FLY_VARIANT_B 0   // Variant B verdict (eyes 2026-06-15): CLEAN — table lookup/SMC oracle exonerated

// BISECTION VARIANT C (2026-06-15): canonical production dispatch, but ALL
// in-loop diagnostic probes compiled out (PR-ZERO, LOWPC, pc-trace ring,
// NEWPC, dbg_watched, MEM-WATCH, PR-PROBE). After A and B, the complete
// remaining delta between glitchy and clean configs is: {in-loop probes,
// reset-pending-before-block, wasm_has_block guard, stuck-bail}. This tests
// the probes — the largest textual mass. Eyes verdict: glitches gone → a
// probe is guilty (binary-search them); unchanged → probes exonerated, test
// reset-before next.
#define FLY_NO_LOOP_PROBES 0

// Naomi serial EEPROM diagnostic counters (defined in naomi.cpp)
extern u32 g_naomi_board_write_count;
extern u32 g_naomi_board_read_count;

// Verify Sh4Context offsets used in wasm_emit.h
static_assert(offsetof(Sh4Context, pc) == 0x148, "PC offset mismatch");
static_assert(offsetof(Sh4Context, jdyn) == 0x14C, "jdyn offset mismatch");
static_assert(offsetof(Sh4Context, sr.T) == 0x154, "sr.T offset mismatch");
static_assert(offsetof(Sh4Context, cycle_counter) == 0x174, "cycle_counter offset mismatch");
static_assert(offsetof(Sh4Context, interrupt_pend) == 0x16C, "interrupt_pend offset mismatch (chain module int check)");

// Forward declarations from driver.cpp
DynarecCodeEntryPtr DYNACALL rdv_FailedToFindBlock(u32 pc);

// Forward declarations for EM_JS functions (defined later, after extern "C" block)
#ifdef __EMSCRIPTEN__
extern "C" {
int wasm_compile_block(const u8* bytesPtr, u32 len, u32 block_pc);
int wasm_compile_block_batch(const u8* bytesPtr, u32 len, const u32* pcsPtr, u32 count, u32* outIdxPtr);
int wasm_execute_block(u32 block_pc, u32 ctx_ptr, u32 ram_base);
int wasm_has_block(u32 block_pc);
void wasm_clear_cache();
void wasm_remove_block(u32 block_pc);
int wasm_compile_chain(const u8* bytesPtr, u32 len, u32 head_pc);
int wasm_execute_chain(u32 head_pc, u32 ctx_ptr, u32 ram_base);
int wasm_has_chain(u32 head_pc);
void wasm_clear_chains();
void wasm_remove_chain(u32 head_pc);
void wasm_compile_chain_async(const u8* bytesPtr, u32 len, u32 head_pc);
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
static std::unordered_map<u32, u32> blockExecCount;   // PC → execution count (per mainloop)

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
static bool g_shil_log_writes = false;  // log writes to g_shil_writes AND apply them

// ============================================================
// Differential validator (EXECUTOR_MODE 7) — memory-write capture
// ============================================================
// When g_val_logging is on, BOTH execution paths record every guest write here:
//   - the JIT path via the wasm_mem_write* imports (fast path disabled by
//     WASM_VAL_LOG_WRITES so all writes route through the import), and
//   - the interpreter path via shop_writem in wasm_exec_shil_fb.
// For RAM (area 3) writes we also capture the OLD value so the validator can
// UNDO the JIT's writes and run the reference on clean memory. MMIO writes are
// recorded (is_ram=false) but not undone/compared — their side effects aren't
// safely repeatable (matches the pre-existing mode-7 behavior).
struct ValWrite {
	u32 addr;
	u32 size;       // 1/2/4 (8-byte writes are split into two 4-byte entries)
	u32 old_val;    // pre-write RAM value (valid only when is_ram)
	u32 new_val;
	bool is_ram;    // physical area 3 (system RAM) — undoable + comparable
};
static std::vector<ValWrite> g_val_writes;
static bool g_val_logging = false;
static bool g_val_halt = false;   // set on first divergence — freezes execution
// Set when a block reads or writes NON-RAM (MMIO/area!=3) while logging. MMIO
// read-modify-write can't be isolated (the reference's MMIO reads would see the
// JIT's un-undoable MMIO writes), so the validator skips the halt for such
// blocks and only reports clean, isolated RAM/compute divergences.
static bool g_val_mmio_touched = false;
// Split out MMIO WRITES specifically: only MMIO writes have unrepeatable side
// effects that make a block uncomparable. MMIO reads (status registers) are fine
// to compare — and a mis-read MMIO value landing in a register is a prime way
// r15/pr get corrupted, so we now DO validate MMIO-read-only blocks.
static bool g_val_mmio_wrote = false;

// Record one write into g_val_writes (RAM old-value captured directly from
// mem_b to avoid MMIO read side effects). Called from the write wrappers and
// the shop_writem interpreter path when g_val_logging is set.
static inline void valLogWrite(u32 addr, u32 size, u32 new_val) {
	u32 phys = addr & 0x1FFFFFFF;
	bool is_ram = ((phys >> 26) == 3);
	if (!is_ram) { g_val_mmio_touched = true; g_val_mmio_wrote = true; }  // MMIO write — block not isolatable
	ValWrite e;
	e.addr = addr;
	e.size = size;
	e.is_ram = is_ram;
	e.new_val = new_val;
	e.old_val = is_ram ? *(u32*)&mem_b[phys & RAM_MASK] : 0;
	g_val_writes.push_back(e);
}

// Note a guest READ during validation — flag MMIO (non-RAM) reads so the
// validator skips blocks whose reference run would see polluted MMIO state.
static inline void valNoteRead(u32 addr) {
	if (((addr & 0x1FFFFFFF) >> 26) != 3) g_val_mmio_touched = true;
}

#if VALIDATOR_BUILD || LOCKSTEP_DIFF_BUILD || FLY_BRIDGE_SHADOW || FLY_CHAIN_SHADOW || FLY_REGION_SHADOW || FLY_WRITE_PARITY
// WriteMem logging wrappers. WriteMem8/16/32 are function pointers (→
// addrspace::write*). Both the OpPtr reference AND the JIT (via wasm_mem_write*)
// ultimately call through them, so wrapping here captures ALL guest writes
// uniformly when g_val_logging is set — the single comparison sink. Installed
// once, lazily, on first validated block.
static WriteMem8Func  g_orig_wm8  = nullptr;
static WriteMem16Func g_orig_wm16 = nullptr;
static WriteMem32Func g_orig_wm32 = nullptr;
static WriteMem64Func g_orig_wm64 = nullptr;
static void DYNACALL val_wm8 (u32 a, u8  d) { if (g_val_logging) valLogWrite(a, 1, d); g_orig_wm8(a, d); }
static void DYNACALL val_wm16(u32 a, u16 d) { if (g_val_logging) valLogWrite(a, 2, d); g_orig_wm16(a, d); }
static void DYNACALL val_wm32(u32 a, u32 d) { if (g_val_logging) valLogWrite(a, 4, d); g_orig_wm32(a, d); }
// 64-bit writes (FMOV.D etc.) — the JIT emits these as two WriteMem32 calls, so
// log this as two 32-bit entries to compare apples-to-apples.
static void DYNACALL val_wm64(u32 a, u64 d) {
	if (g_val_logging) { valLogWrite(a, 4, (u32)d); valLogWrite(a + 4, 4, (u32)(d >> 32)); }
	g_orig_wm64(a, d);
}
static void valInstallWriteHook() {
	if (g_orig_wm32) return;  // already installed
	g_orig_wm8 = WriteMem8;   g_orig_wm16 = WriteMem16;
	g_orig_wm32 = WriteMem32; g_orig_wm64 = WriteMem64;
	WriteMem8  = val_wm8;      WriteMem16  = val_wm16;
	WriteMem32 = val_wm32;     WriteMem64  = val_wm64;
}
#endif

// ============================================================
// Dispatch PC trace — JIT-vs-interpreter dispatch diff
// ============================================================
// Records the FIRST N block-entry PCs from reset. Hooked in BOTH dispatch
// paths: c_dispatch_loop (mode 6 = JIT, uses c_dispatch_loop) and the
// interpreter mainloop (mode 0). Run each mode on the same ROM from reset (no
// input — deterministic boot), read the buffer via the getters, diff offline:
// the first index where the JIT dispatch selects a different block than the
// interpreter is the dispatch-layer bug (the per-block compute is already proven
// clean vs OpPtr, so any path divergence is in dispatch/timing/interrupts).
#ifndef JIT_PROD_BUILD
#define DISP_TRACE_MAX 300000
static u32 g_disp_pc[DISP_TRACE_MAX];
static u32 g_disp_n = 0;
// FIRST-VISIT trace: record each block PC only the first time it's dispatched
// (bitmap-deduped). Loops/waits record their blocks once, so the trace = the
// set+order of blocks REACHED. If a build is stuck in a wait loop, its count
// plateaus; if it progresses, new blocks keep appearing. Reveals exactly where
// one build stops reaching new code that the other reaches.
static u8 g_disp_seen_bm[0x80000];  // 4M-bit bitmap over (pc>>1)&0x3FFFFF
static inline void dispTrace(u32 pc) {
	if (g_disp_n >= DISP_TRACE_MAX) return;
	u32 b = (pc >> 1) & 0x3FFFFF;
	if (g_disp_seen_bm[b >> 3] & (1u << (b & 7))) return;
	g_disp_seen_bm[b >> 3] |= (1u << (b & 7));
	g_disp_pc[g_disp_n++] = pc;
}
extern "C" u32 EMSCRIPTEN_KEEPALIVE fly_disp_count() { return g_disp_n; }
extern "C" u32 EMSCRIPTEN_KEEPALIVE fly_disp_ptr()  { return (u32)(uintptr_t)g_disp_pc; }

// Memory snapshot exports (2026-06-12) — for the boot-corruption VRAM/RAM diff.
// User-confirmed: booting under clean dispatch → glitch-free menu; booting under
// production → persistent glitches at the menu REGARDLESS of which dispatcher
// runs afterward. So production corrupts persistent memory once during the
// boot→menu load window. These getters let the headless capture dump VRAM+RAM
// at an identical guest progress point (FrameCount) in both modes; the binary
// diff exposes exactly which bytes production corrupts.
// (function-local externs: this TU already declares `vram` with C linkage
// inside wasm_mem_* — a file-scope C++ declaration conflicts.)
extern "C" u32 EMSCRIPTEN_KEEPALIVE fly_vram_ptr()    { extern RamRegion vram; return (u32)(uintptr_t)&vram[0]; }
extern "C" u32 EMSCRIPTEN_KEEPALIVE fly_vram_size()   { return VRAM_SIZE; }
extern "C" u32 EMSCRIPTEN_KEEPALIVE fly_ram_ptr()     { return (u32)(uintptr_t)&mem_b[0]; }
extern "C" u32 EMSCRIPTEN_KEEPALIVE fly_ram_size()    { return RAM_SIZE; }
extern "C" u32 EMSCRIPTEN_KEEPALIVE fly_frame_count() { extern u32 FrameCount; return FrameCount; }
// Live Sh4Context pointer — lets the freeze autopsy read registers (esp. r15,
// the stack pointer holding the wedged token-scanner's cursor) at dump time.
extern "C" u32 EMSCRIPTEN_KEEPALIVE fly_ctx_ptr()  { return (u32)(uintptr_t)&Sh4cntx; }
extern "C" u32 EMSCRIPTEN_KEEPALIVE fly_ctx_size() { return (u32)sizeof(Sh4Context); }
// AICA sound RAM — the SH4↔ARM7 sound-driver shared state (command queues,
// sequence data). The JGR freeze scanner is sound-driver-shaped; if sound RAM
// diverges between prod/clean while main RAM matches, the divergence lives in
// the AICA/ARM7 subsystem's interaction with the dispatch loop.
namespace aica { extern RamRegion aica_ram; }
extern "C" u32 EMSCRIPTEN_KEEPALIVE fly_aica_ptr()  { return (u32)(uintptr_t)&aica::aica_ram[0]; }
extern "C" u32 EMSCRIPTEN_KEEPALIVE fly_aica_size() { return ARAM_SIZE; }
#else
static inline void dispTrace(u32) {}
#endif

// Write/read counters for diagnostic modes
static u32 g_shil_write_count = 0;
static u32 g_shil_read_count = 0;
static u32 g_shil_pvr_write_count = 0;
static u32 g_shil_sq_write_count = 0;

// ============================================================
// Profiling counters (stripped in prod build)
// ============================================================
#ifndef JIT_PROD_BUILD
static u32 prof_native_ops_compiled = 0;   // SHIL ops compiled to native WASM
static u32 prof_fallback_ops_compiled = 0; // SHIL ops compiled as C++ fallback
static u32 prof_idle_loops_detected = 0;   // blocks detected as idle spin-wait loops
static u32 prof_multiblock_modules = 0;   // multi-block super-block modules compiled
static u32 prof_multiblock_total_blocks = 0; // total blocks in super-block modules
static u32 prof_fb_by_op[128] = {};        // runtime fallback calls by op type
static double prof_emulation_ms = 0;       // time in inner block dispatch loop
static double prof_system_ms = 0;          // time in UpdateSystem_INTC
static double prof_wall_ms = 0;            // accumulated wall time across all frames in report window
static double prof_compile_ms_acc = 0;     // accumulated compile time
static u32 prof_block_execs_acc = 0;       // accumulated block executions
static u32 prof_timeslices_acc = 0;        // accumulated timeslices
static u32 prof_fb_calls_acc = 0;          // accumulated fallback calls
static u32 prof_interp_acc = 0;            // accumulated interpreted instructions
static int prof_report_frames = 0;         // mainloop calls since last report
#endif

// ============================================================
// C dispatch table: PC hash → indirect function table index
// ============================================================
// Each compiled WASM block is registered in Emscripten's
// __indirect_function_table. The dispatch table maps PC hashes
// to table indices. c_dispatch_loop uses call_indirect to call
// blocks entirely within WASM — no JS in the hot path.
#define JIT_TABLE_SIZE (1 << 20)  // 1M entries (~4MB)
#define JIT_TABLE_MASK (JIT_TABLE_SIZE - 1)
// ★ PAGE-GENERATION SMC (2026-07-17): per-4KB-RAM-page write counters.
// Every RAM write bumps its page's generation (JIT store imports + C-side
// writers: SQ flush, GD-DMA). The dispatch hit path re-hashes a block ONLY
// when a covered page's generation changed since last verification — the
// measured ~4M hashed halfwords/vblank (≈40% of DOA2's frame cost) drop to
// ~zero at steady state with EXACT detection semantics. Insurance: a forced
// hash every 64th dispatch bounds staleness if any C-side writer is missed
// (new code is additionally always caught by the miss-path full hash on its
// first dispatch).
#define FLY_PAGE_GEN_COUNT (RAM_SIZE_MAX >> 12)
u32 g_fly_page_gen[FLY_PAGE_GEN_COUNT];
// ★ REGION WRITE-WATCH (2026-07-19, guard-cost experiment): per-region
// "current gen-sum" cells maintained HERE on the write path, so the region
// module's per-iteration guard is 2 loads + ne instead of 8 loads + 7 adds
// + ne. Exit semantics identical to the certified v0 range gen-sum: the cur
// cell bumps exactly when a page gen in [lo,hi] bumps. Registry rebuilt by
// fly_region_watch_rebuild() on region register/invalidate/reset; empty
// (g_rwatch_n=0) for non-region builds — one predictable-untaken loop test.
#define FLY_RWATCH_MAX 8
static u32  g_rwatch_n;
static u32  g_rwatch_lo[FLY_RWATCH_MAX], g_rwatch_hi[FLY_RWATCH_MAX];
static u32* g_rwatch_cell[FLY_RWATCH_MAX];
static inline void fly_ram_written(u32 addr, u32 len) {
	u32 phys = addr & 0x1FFFFFFF;
	if ((phys >> 26) != 3) return;
	u32 p0 = (phys & RAM_MASK) >> 12;
	u32 p1 = ((phys + (len ? len - 1 : 0)) & RAM_MASK) >> 12;
	g_fly_page_gen[p0]++;
	if (p1 != p0) g_fly_page_gen[p1]++;
	for (u32 i = 0; i < g_rwatch_n; i++)
		if ((p0 >= g_rwatch_lo[i] && p0 <= g_rwatch_hi[i])
		    || (p1 >= g_rwatch_lo[i] && p1 <= g_rwatch_hi[i]))
			(*g_rwatch_cell[i])++;
}

static u32 jit_dispatch_table[JIT_TABLE_SIZE];  // PC hash → table index (0 = miss)
static u32 jit_dispatch_pc[JIT_TABLE_SIZE];    // PC hash → actual PC (collision guard)
static u32 jit_dispatch_hash[JIT_TABLE_SIZE];  // PC hash → SMC fingerprint (full block hash for RAM, first op for others)
static u16 jit_dispatch_sz[JIT_TABLE_SIZE];    // PC hash → block size in u16 halfwords (0 = skip SMC check)
static u32 jit_dispatch_pgen[JIT_TABLE_SIZE];  // PC hash → page-generation sum at last SMC verify
static u8  jit_dispatch_tick[JIT_TABLE_SIZE];  // PC hash → forced-hash insurance counter
static u16 jit_dispatch_arg[JIT_TABLE_SIZE];   // region entry index (dense) for slots primed to a region module; 0 otherwise
extern u32 g_region_entry_idx;                 // defined with the region emitter

// Per-PC SMC fingerprint (parallels blockByVaddr). The slot-keyed arrays above
// are overwritten on hash COLLISIONS, so they can't be trusted to SMC-check a
// block reached via the collision/miss path — c_dispatch_loop only SMC-checks
// table HITS. This exact pc→(hash,sz) map lets the miss handler detect a stale
// (self-modified) block reached via collision and recompile it fresh, matching
// the per-block SMC the clean (mode-8) dispatch does. Cleared in reset().
struct BlockSmc { u32 hash; u16 sz; };
static std::unordered_map<u32, BlockSmc> g_block_smc;

// Dispatch loop exit status
static int g_dispatch_result = 0;    // 0=timeslice, 1=miss, 3=interrupt
static u32 g_dispatch_miss_pc = 0;

// Live A/B toggle: when true, production uses clean_dispatch_loop (mode-8 path)
// instead of c_dispatch_loop. Set per-frame from Module._useCleanDispatch.
static bool g_use_clean_dispatch = false;

// ============================================================
// SMC fingerprint — hash over full block bytes
// ============================================================
// Reads n_u16 halfwords from RAM starting at vaddr's physical offset,
// returns a 32-bit rolling-XOR hash. Used by both the compile path
// (to prime jit_dispatch_hash) and the dispatch SMC check.
//
// Rationale: the original SMC fingerprint was just IReadMem16(vaddr) —
// only PC+0/1. Any game that rewrote byte 2+ inside a compiled block
// silently ran stale compiled code. Layer 1b test smc_inner_byte
// confirmed this hazard; widening the hash to cover the full block
// closes it.
static inline u32 hashRamBlock(u32 vaddr, u32 n_u16) {
	u32 phys = vaddr & 0x1FFFFFFF;
	u32 h = 0;
	for (u32 i = 0; i < n_u16; i++) {
		u16 w = *(u16*)(&mem_b[(phys + i * 2) & RAM_MASK]);
		h = ((h << 5) | (h >> 27)) ^ (u32)w;  // rotl 5, xor
	}
	return h;
}

// Populate dispatch-table metadata for a compiled block. For area-3 RAM
// blocks, stores the full-block hash + halfword count; dispatch SMC check
// uses both. For non-RAM blocks (BIOS/ROM), stores zero size so the
// dispatch SMC check is skipped (ROM can't self-modify).
static inline void primeDispatchEntry(u32 vaddr, u32 sh4_code_size, u32 table_idx,
                                      u32 entry_arg = 0) {
	u32 key = (vaddr >> 1) & JIT_TABLE_MASK;
	jit_dispatch_table[key] = table_idx;
	jit_dispatch_pc[key] = vaddr;
	jit_dispatch_arg[key] = (u16)entry_arg;
	u32 phys = vaddr & 0x1FFFFFFF;
	if ((phys >> 26) == 3) {
		u32 nw = sh4_code_size / 2;
		if (nw == 0) nw = 1;            // minimum: hash the opcode at PC
		if (nw > 0xFFFF) nw = 0xFFFF;   // clamp to u16
		u32 h = hashRamBlock(vaddr, nw);
		jit_dispatch_hash[key] = h;
		jit_dispatch_sz[key] = (u16)nw;
		{
			u32 p0 = (phys & RAM_MASK) >> 12;
			u32 p1 = ((phys + nw * 2 - 1) & RAM_MASK) >> 12;
			jit_dispatch_pgen[key] = g_fly_page_gen[p0] + (p1 != p0 ? g_fly_page_gen[p1] : 0);
			jit_dispatch_tick[key] = 0;
		}
		// Per-PC copy (collision-proof) for the miss-path SMC check.
		g_block_smc[vaddr] = { h, (u16)nw };
	} else {
		jit_dispatch_hash[key] = 0;
		jit_dispatch_sz[key] = 0;       // 0 = skip SMC check
	}
}

// ============================================================
// Per-frame compile budget (2026-07-16)
// ============================================================
// Synchronous WebAssembly.compile storms at scene transitions caused
// 100-850ms frame spikes (measured: logs/perf-baseline.log — steady-state
// compile cost is only ~1.2%, the spikes are the entire stutter problem).
// Cold-miss compiles beyond the budget are DEFERRED: the module bytes are
// queued here and the block executes via the SHIL fallback — charging
// block->guest_cycles, NOT the old 1-cycle-per-instruction interp fallback
// whose timing warp caused the pre-06/12 glitches — until the queue drains
// at mainloop frame starts (same budget per frame). Module bytes are built
// from the already-decoded oplist, so entry-fpscr correctness is unaffected
// by when the JS compile finally runs.
// ASYNC COMPILE (2026-07-16 v3): compilation storms at scene loads caused
// 100-850ms frame stalls ([SPIKE] instrument: e.g. 430ms compile in a 588ms
// frame). Inline synchronous compiles are allowed until the frame reaches
// FLY_FRAME_BUDGET_MS wall-clock age; past that, the module bytes go to
// WebAssembly.instantiate() — the browser compiles OFF-THREAD and the block
// executes via the SHIL fallback (charging block->guest_cycles, so guest
// timing is unaffected) for the frame or two until the promise resolves and
// drainCompileQueue() promotes it into the dispatch table.
// History: v1 (flat 4ms sync drain) starved promotion → load phases crawled
// on SHIL for minutes; v2 (sync drain on frame leftovers) starved harder
// because emulation alone exceeds any sane frame budget. Async promotion has
// no drain-rate cap — resolution latency is 1-2 frames regardless of storm
// size, so the SHIL bridge stays short.
// BUILD-BAKED VARIANT (2026-07-17, user directive: no runtime toggles —
// each iteration is a committed build so the change is guaranteed active).
// v6 = async deferral ON + corrected SHIL bridge (cycle-normalized) +
// per-block interrupt check after bridged execution (v5 delivered interrupts
// raised in bridged blocks one block late — systematic timing skew, prime
// suspect for its soak-time graphics divergence) + [BRIDGE-CHARGE] telemetry.
// Verdict history: v4 (trap-mediated bridge) broke JGR immediately;
// v5 (no int-check) broke JGR after soak. Each build gets user-eyes verdict.
#define FLY_DEFER_ON 1
static const bool g_defer_runtime = FLY_DEFER_ON;
#define FLY_FRAME_BUDGET_MS 24.0
static double g_frame_start_ms = 0;   // stamped at mainloop entry
#ifndef JIT_PROD_BUILD
// Spike-hunt round 2 (2026-07-23): per-frame bracket snapshots so [SPIKE2]
// can attribute a slow frame to render brackets + guest-time advanced.
static double g_fly_rp_snap[10];
static u64 g_fly_guest_snap = 0;
#endif
static inline bool fly_frame_over_budget() {
	return emscripten_get_now() - g_frame_start_ms > FLY_FRAME_BUDGET_MS;
}
static bool g_compile_defer = false;   // set around over-budget rdv calls
static u32 g_defer_count = 0, g_drain_count = 0, g_defer_drop_count = 0;
// HOT-DEFER PROMOTION: SHIL-bridging a hot loop for even one frame costs
// hundreds of ms ([SPIKE] frames with compile fully deferred: other=834ms).
// Count per-frame dispatches of each still-deferred block; past the
// threshold, sync-compile it immediately — ~1ms beats more SHIL spins.
#define FLY_HOT_DEFER_THRESHOLD 8
static std::unordered_map<u32, u32> g_defer_execs;   // cleared each frame
// ★ STORM DETECTION (2026-07-23 spike hunt): bridged-block executions this
// frame. Steady state is ~0-150; a transition storm (cold working set inside
// a multi-vblank no-render frame) is 10K-2M+. Past the threshold, the frame
// is already doomed to grind — spend budget on compiling instead of refusing.
static u32 g_bridged_this_frame = 0;
#define FLY_STORM_BRIDGED_THRESHOLD 2000
// Mid-frame batch flush depth: normal frames queue <10 blocks; a storm
// queues hundreds. Flushing at 64 turns "bridge the whole 3-vblank frame"
// into "compile the working set a few ms in and dispatch natively".
#define FLY_STORM_FLUSH_DEPTH 64
// BATCHED HOT-MISS SYNC COMPILE (2026-07-18): ~95% of a small module's sync
// compile cost is per-MODULE overhead, so a transition working set is promoted
// as ONE multi-function Module per frame (flushBlockBatch) instead of ~1ms per
// block. Queued blocks bridge until the flush. (pc, ptr) pairs so validation
// never dereferences a possibly-freed block.
#define FLY_SYNC_COMPILE_BUDGET_MS 2.0
static bool g_compile_batch = false;   // set around rdv calls that should queue
static std::vector<std::pair<u32, RuntimeBlockInfo*>> g_batch_blocks;
static std::unordered_set<u32> g_batch_pending;
#ifndef JIT_PROD_BUILD
static double g_fly_shil_ms = 0;   // per-frame SHIL-bridge time ([SPIKE])
static u32 g_fly_shil_count = 0;
#endif

#ifndef JIT_PROD_BUILD
// Spike-composition accumulators — defined in rend/gles/gltex.cpp.
extern "C" {
extern double g_fly_tex_check_ms;
extern double g_fly_tex_update_ms;
extern u32 g_fly_tex_update_count;
}
#endif

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

// ★ PROD-ACTIVE region-pipeline data (hoisted out of the dev gate,
// 2026-07-21): the page-exec histogram and the discovery function feed
// fly_region_pipeline_tick(), which prod builds run when regions are
// enabled. Everything else in the dev block below stays dev-only.
u32 g_fly_page_execs[4096];   // dispatch count per 4KB RAM page (region scoping)
// Region discovery (design v0): seed at the hottest page, extend to contiguous
// neighbors holding >=5% of the seed's window count, cap 8 pages. Returns
// [first,last] page or first=0xFFFFFFFF if nothing hot enough.
struct FlyRegionCandidate {
	u32 first_page = 0xFFFFFFFF;
	u32 last_page = 0;
	u64 covered = 0;   // window dispatches inside range
	u64 total = 0;     // window dispatches everywhere
};
static FlyRegionCandidate fly_discover_region(const u32* window_execs);

#ifndef JIT_PROD_BUILD
// Memory-import density counters (2026-07-17): every guest load/store still
// crosses this import boundary. DOA2-vs-JGR density + RAM-eligible share
// decides the inline fast-path design (measure before building).
u32 g_fly_mr = 0, g_fly_mr_ram = 0, g_fly_mw = 0, g_fly_mw_ram = 0;
// Size-8 (float-pair) op executions, bumped from EMITTED code in the size==8
// readm/writem arms (fastmem Phase 0). Each size-8 op makes 2 import calls,
// so 2*g_fly_mr8 ~= g_fly_mr confirms the import-read wall is float pairs.
u32 g_fly_mr8 = 0, g_fly_mw8 = 0;
u32 g_fly_mw_sq = 0;   // write-import calls landing in the SQ region (0xE0-0xE3)
u32 g_fly_hash_words = 0;   // halfwords hashed by per-dispatch SMC checks
u32 g_fly_chain_hash_words = 0;   // halfwords hashed by chain-interior guards (WASM-side)
u32 g_fly_chain_enters = 0;  // chain-module entries (each = 1 call_indirect)
u32 g_fly_chain_links = 0;   // member-block executions inside chain modules
u32 g_fly_chains_built = 0;  // running chain-compile count (churn tracking)
// Overlay-return measurement (2026-07-18): what fraction of compiles carry a
// (pc, code-hash) pair we've compiled before? High = DOA2-style overlay
// cycling → content-keyed module retention pays.
// Inter-page edge tracking (region SHAPE): consecutive dispatch pairs that
// stay in-page vs cross pages. Cross pairs accumulate in a map (cumulative,
// dev-only). Caveat: consecutive dispatches aren't strictly control-flow
// successors (timeslice/interrupt interleave) — good enough for shaping.
u32 g_fly_edge_intra = 0, g_fly_edge_cross = 0;
static u32 g_fly_last_page = 0xFFFFFFFF;
static std::unordered_map<u32, u32> g_fly_edge_pairs;
#endif  // JIT_PROD_BUILD (dev counters)

static FlyRegionCandidate fly_discover_region(const u32* window_execs)
{
	FlyRegionCandidate rc;
	u32 seed = 0; u32 seedv = 0;
	for (u32 pi = 0; pi < 4096; pi++) {
		rc.total += window_execs[pi];
		if (window_execs[pi] > seedv) { seedv = window_execs[pi]; seed = pi; }
	}
	if (seedv == 0)
		return rc;
	// Hot pages need not be contiguous (DOA2: 12b/12d/12f with cold gaps).
	// Greedily absorb the nearest page >=5% of seed on either side while the
	// total SPAN stays within the 8-page cap — cold gap pages ride along
	// (one extra gen-load in the guard, zero member blocks).
	u32 thresh = seedv / 20;
	u32 lo = seed, hi = seed;
	for (;;) {
		u32 nl = lo, nh = hi;
		while (nl > 0 && window_execs[nl - 1] < thresh && lo - (nl - 1) + 0 < 8 && hi - (nl - 1) + 1 <= 8) nl--;
		bool canL = (nl > 0) && (hi - (nl - 1) + 1 <= 8) && window_execs[nl - 1] >= thresh;
		while (nh < 4095 && window_execs[nh + 1] < thresh && (nh + 1) - lo + 1 <= 8) nh++;
		bool canR = (nh < 4095) && ((nh + 1) - lo + 1 <= 8) && window_execs[nh + 1] >= thresh;
		if (canL && (!canR || window_execs[nl - 1] >= window_execs[nh + 1]))
			lo = nl - 1;
		else if (canR)
			hi = nh + 1;
		else
			break;
	}
	rc.first_page = lo;
	rc.last_page = hi;
	for (u32 pi = lo; pi <= hi; pi++) rc.covered += window_execs[pi];
	return rc;
}

#ifndef JIT_PROD_BUILD
void fly_track_edge(u32 page) {
	g_fly_page_execs[page]++;
	if (g_fly_last_page != 0xFFFFFFFF) {
		if (page == g_fly_last_page) {
			g_fly_edge_intra++;
		} else {
			g_fly_edge_cross++;
			g_fly_edge_pairs[(g_fly_last_page << 16) | page]++;
		}
	}
	g_fly_last_page = page;
}
static std::unordered_map<u64, u8> g_fly_seen_code;
u32 g_fly_code_compiles = 0, g_fly_code_returns = 0;
static inline void fly_track_code(u32 vaddr, u32 hash) {
	u64 k = ((u64)vaddr << 32) | hash;
	if (!g_fly_seen_code.insert({ k, 1 }).second)
		g_fly_code_returns++;
	g_fly_code_compiles++;
}
static inline void fly_mem_count_r(u32 addr) {
	g_fly_mr++;
	if (((addr & 0x1FFFFFFF) >> 26) == 3) g_fly_mr_ram++;
}
static inline void fly_mem_count_w(u32 addr) {
	g_fly_mw++;
	if (((addr & 0x1FFFFFFF) >> 26) == 3) g_fly_mw_ram++;
	// SQ-region stores (0xE0-0xE3FFFFFF, unmasked): candidate for full
	// inlining — the C handler is a plain mapBlock onto ctx.sq_buffer
	// (mask 63), zero side effects (TA-write campaign Phase 0).
	if ((addr >> 26) == 0x38) g_fly_mw_sq++;
}
#else
#define fly_mem_count_r(a) ((void)0)
#define fly_mem_count_w(a) ((void)0)
#endif

extern "C" {

u32 EMSCRIPTEN_KEEPALIVE wasm_mem_read8(u32 addr) {
	fly_mem_count_r(addr);
	addMemReadPenalty(addr, 1);
	if (g_val_logging) valNoteRead(addr);
	return (u32)(s32)(s8)ReadMem8(addr);  // sign-extend (matches SHIL convention)
}

u32 EMSCRIPTEN_KEEPALIVE wasm_mem_read16(u32 addr) {
	fly_mem_count_r(addr);
	addMemReadPenalty(addr, 2);
	if (g_val_logging) valNoteRead(addr);
	return (u32)(s32)(s16)ReadMem16(addr);  // sign-extend (matches SHIL convention)
}

u32 EMSCRIPTEN_KEEPALIVE wasm_mem_read32(u32 addr) {
	fly_mem_count_r(addr);
	addMemReadPenalty(addr, 4);
	if (g_val_logging) valNoteRead(addr);
	return ReadMem32(addr);
}

#if FLY_WRITE_WATCH
// Write-provenance watch: known JGR staging-hole addresses (RAM offsets).
// Stable per scene across runs; from the geometry probes. Logs every write
// (any size) touching a watched 32-byte param slot, with frame + writer tag.
// DYNAMIC watch targets: the ch2-DMA scan (dmac.cpp) sets these to the actual
// hole offsets of the latest transfer, so next frame's writes to those exact
// slots get logged — write-vs-transfer interleaving, no cross-run guessing.
extern "C" u32 g_fly_watch_off[2] = { 0x6ECF00, 0x6F34E0 };
extern "C" u32 g_fly_watch_hits = 0;
static inline void fly_watch_write(u32 addr, u32 val, u32 sz, int writer /*0=jit-import 1=sq-flush 2=block-dma*/) {
	u32 phys = addr & 0x1FFFFFFF;
	if ((phys >> 26) != 3) return;
	u32 off = phys & RAM_MASK;
	for (int i = 0; i < 2; i++) {
		if (off >= g_fly_watch_off[i] && off < g_fly_watch_off[i] + 0x20) {
			g_fly_watch_hits++;
			if (g_fly_watch_hits <= 400) {
				extern u32 FrameCount;
				EM_ASM({
					if (!window._flyLog) window._flyLog = [];
					window._flyLog.push('[WATCH-WR] frame=' + $0 + ' off=0x' + ($1>>>0).toString(16)
						+ ' val=0x' + ($2>>>0).toString(16) + ' sz=' + $3 + ' writer=' + $4
						+ ' pc=0x' + ($5>>>0).toString(16));
				}, FrameCount, off, val, sz, writer, Sh4cntx.pc);
			}
			return;
		}
	}
}
#endif

void EMSCRIPTEN_KEEPALIVE wasm_mem_write8(u32 addr, u32 val) {
	fly_ram_written(addr, 1);
	fly_mem_count_w(addr);
	addMemWritePenalty(addr, 1);
#if FLY_WRITE_WATCH
	fly_watch_write(addr, val, 1, 0);
#endif
	WriteMem8(addr, (u8)val);  // validator logs at the WriteMem* wrapper
}

void EMSCRIPTEN_KEEPALIVE wasm_mem_write16(u32 addr, u32 val) {
	fly_ram_written(addr, 2);
	fly_mem_count_w(addr);
	addMemWritePenalty(addr, 2);
#if FLY_WRITE_WATCH
	fly_watch_write(addr, val, 2, 0);
#endif
	WriteMem16(addr, (u16)val);
}

void EMSCRIPTEN_KEEPALIVE wasm_mem_write32(u32 addr, u32 val) {
	fly_ram_written(addr, 4);
	fly_mem_count_w(addr);
	addMemWritePenalty(addr, 4);
#if FLY_WRITE_WATCH
	fly_watch_write(addr, val, 4, 0);
#endif
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
static u32 g_shil_fb_oob_count = 0;
// Lost-fallback-op counters, readable by the capture autopsy.
extern "C" u32 EMSCRIPTEN_KEEPALIVE fly_fb_calls()  { return g_shil_fb_call_count; }
extern "C" u32 EMSCRIPTEN_KEEPALIVE fly_fb_misses() { return g_shil_fb_miss_count; }
extern "C" u32 EMSCRIPTEN_KEEPALIVE fly_fb_oob()    { return g_shil_fb_oob_count; }

// ★ THIN JIT IMPORTS (2026-07-17): dedicated entry points for ops that must
// call into C but need NONE of the shil_fb machinery (no blockByVaddr
// lookup, no operand re-decode, no full register flush/reload). Measured:
// shop_pref alone was 90% of ALL fallback calls (DOA2 ~8.5K/frame).
#include "hw/sh4/sh4_rom.h"
u32 fly_sin_table_addr() { return (u32)(uintptr_t)&sin_table[0]; }

void EMSCRIPTEN_KEEPALIVE wasm_sq_pref(u32 addr) {
	// doSqWrite reads only ctx->sq_buffer (memory-coherent, never register-
	// cached) and writes device/RAM — no GPR interaction at all.
	if (Sh4cntx.doSqWrite)
		Sh4cntx.doSqWrite(addr, &Sh4cntx);
}
u64 EMSCRIPTEN_KEEPALIVE wasm_div32u(u32 r1, u32 r2, u32 r3) {
	u64 dividend = ((u64)r3 << 32) | r1;
	u32 quo = r2 ? (u32)(dividend / r2) : 0;
	u32 rem = r2 ? (u32)(dividend % r2) : (u32)dividend;
	return ((u64)rem << 32) | quo;   // rd = low, rd2 = high
}
u64 EMSCRIPTEN_KEEPALIVE wasm_div32s(u32 r1u, u32 r2u, u32 r3u) {
	s32 r2 = (s32)r2u, r3 = (s32)r3u;
	s64 dividend = ((s64)r3 << 32) | r1u;
	if (dividend < 0)
		dividend++;
	s32 quo = (s32)(r2 ? dividend / r2 : 0);
	s32 rem = (s32)(dividend - (s64)quo * r2);
	u32 negative = (r3u ^ r2u) & 0x80000000;
	if (negative)
		quo--;
	else if (r3 < 0)
		rem--;
	return ((u64)(u32)rem << 32) | (u32)quo;
}
u64 EMSCRIPTEN_KEEPALIVE wasm_div1(u32 a, u32 bU, u32 T) {
	// Ported verbatim from the shil fallback (matches shil_canonical.h).
	Sh4Context& ctx = Sh4cntx;
	s32 b = (s32)bU;
	bool qxm = ctx.sr.Q ^ ctx.sr.M;
	ctx.sr.Q = (int)a < 0;
	a = (a << 1) | T;
	u32 oldA = a;
	a += (qxm ? 1 : -1) * b;
	ctx.sr.Q ^= ctx.sr.M ^ (qxm ? a < oldA : a > oldA);
	T = !(ctx.sr.Q ^ ctx.sr.M);
	return ((u64)T << 32) | a;   // rd = a (low), rd2 = T (high)
}


void EMSCRIPTEN_KEEPALIVE wasm_exec_shil_fb(u32 block_vaddr, u32 op_index) {
	g_shil_fb_call_count++;
	// Skip remaining ops after a deferred exception (block should abort)
	if (g_ifb_exception_pending) return;

	auto it = blockByVaddr.find(block_vaddr);
	if (it == blockByVaddr.end()) {
		g_shil_fb_miss_count++;
#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
		// LOST-OP HAZARD #1 (prime suspect for the JGR zero-tail display lists,
		// 2026-06-12): the compiled WASM block delegates fallback ops (incl. the
		// SQ-flushing pref!) here by (vaddr, opIndex) — if the block was EVICTED
		// while its code still executes, this silently NO-OPS the op. A no-op'd
		// pref = a lost 32-byte store-queue burst = a hole in game-built data.
		if (g_shil_fb_miss_count <= 20 || (g_shil_fb_miss_count & 0xFF) == 0) {
			extern u32 FrameCount;
			EM_ASM({ console.log('[SHIL-FB-MISS] #' + $0 +
				' vaddr=0x' + ($1>>>0).toString(16) +
				' op_idx=' + $2 + ' frame=' + $3); },
				g_shil_fb_miss_count, block_vaddr, op_index, FrameCount);
		}
#endif
		return;
	}
	RuntimeBlockInfo* block = it->second;
	if (op_index >= block->oplist.size()) {
		// LOST-OP HAZARD #2: block was REPLACED (recompiled, possibly different
		// boundaries after SMC) — the new oplist is shorter than the executing
		// code expects. Silent no-op. (In-range indexes into a replaced oplist
		// would execute the WRONG op — not detectable here; counted via probe
		// runs by correlation.)
		g_shil_fb_oob_count++;
#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
		if (g_shil_fb_oob_count <= 20 || (g_shil_fb_oob_count & 0xFF) == 0) {
			extern u32 FrameCount;
			EM_ASM({ console.log('[SHIL-FB-OOB] #' + $0 +
				' vaddr=0x' + ($1>>>0).toString(16) +
				' op_idx=' + $2 + ' oplist_n=' + $3 + ' frame=' + $4); },
				g_shil_fb_oob_count, block_vaddr, op_index,
				(u32)block->oplist.size(), FrameCount);
		}
#endif
		return;
	}

	shil_opcode& op = block->oplist[op_index];
	Sh4Context& ctx = Sh4cntx;

#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
	// Fallback-op histogram (2026-07-17): which ops actually eat the ~9.4K
	// fb calls/frame (DOA2)? Emission priority comes from this, not op-count.
	{
		static u32 fb_hist[128];
		static u32 fb_total;
		if ((u32)op.op < 128) fb_hist[(u32)op.op]++;
		if ((++fb_total & 0xFFFFF) == 0) {   // every ~1M calls
			// top-6 by count
			for (int rank = 0; rank < 6; rank++) {
				u32 best = 0, besti = 0;
				for (u32 i = 0; i < 128; i++)
					if (fb_hist[i] > best) { best = fb_hist[i]; besti = i; }
				if (!best) break;
				EM_ASM({ console.log('[FB-HIST] rank' + $0 + ' shop=' + $1 +
					' count=' + $2); }, rank, besti, best);
				fb_hist[besti] = 0;   // consume for ranking (resets histogram)
			}
			EM_ASM({ console.log('[FB-HIST] total=' + $0); }, fb_total);
		}
	}
#endif

	// Helper lambdas to read/write params
	auto readI32 = [&](const shil_param& p) -> u32 {
		if (p.is_imm()) return p._imm;
		if (p.is_reg()) return *(u32*)((u8*)&ctx + p.reg_offset());
		return 0;
	};
	auto readF32 = [&](const shil_param& p) -> float {
		if (p.is_reg()) return *(float*)((u8*)&ctx + p.reg_offset());
		if (p.is_imm()) {
			// ★ BRIDGE-SHADOW FIND (2026-07-17): float IMMEDIATES were
			// silently read as 0.0f here — every FP op with an imm operand
			// (fadd fr,fr,#1.0f etc.) computed garbage on the fallback path.
			// The imm bits are the raw IEEE-754 pattern; reinterpret them.
			u32 bits = p._imm;
			float f;
			memcpy(&f, &bits, sizeof(f));
			return f;
		}
		return 0.0f;
	};
	auto writeI32 = [&](const shil_param& p, u32 val) {
		if (p.is_reg()) *(u32*)((u8*)&ctx + p.reg_offset()) = val;
	};
	auto writeF32 = [&](const shil_param& p, float val) {
		if (p.is_reg()) *(float*)((u8*)&ctx + p.reg_offset()) = val;
	};

	// Per-op trace for diverging block #2360476 at pc=0x8c00b8e4
#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
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
#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
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
		// Must mirror the native emitter EXACTLY (f64 promote/mul/add/demote
		// — see wasm_emit.h shop_fmac). std::fma differed in double-rounding
		// tie cases; bridge-shadow requires bit-identical paths.
		float fn = readF32(op.rs1), f0 = readF32(op.rs2), fm = readF32(op.rs3);
		writeF32(op.rd, (float)((double)fn + (double)f0 * (double)fm));
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
		// 4x4 matrix * 4-element vector with double accumulation
		// Copy input vector to temp to handle rd == rs1 aliasing (FTRV is in-place)
		u32 voff = op.rs1.reg_offset(), moff = op.rs2.reg_offset();
		u32 doff = op.rd.reg_offset();
		float vin[4];
		for (int j = 0; j < 4; j++)
			vin[j] = *(float*)((u8*)&ctx + voff + j * 4);
		for (int i = 0; i < 4; i++) {
			double sum = 0;
			for (int j = 0; j < 4; j++) {
				float m = *(float*)((u8*)&ctx + moff + (j * 4 + i) * 4);
				sum += (double)m * (double)vin[j];
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
		// Save sr.T into jdyn for delayed conditional branches (BT/S, BF/S).
		// The condition is evaluated BEFORE the delay slot but the branch
		// happens AFTER, so we stash the condition in jdyn.
		writeI32(op.rd, readI32(op.rs1));
		break;
	case shop_readm: {
		u32 addr = readI32(op.rs1);
		if (!op.rs3.is_null()) addr += readI32(op.rs3);
		addMemReadPenalty(addr, op.size);
		if (g_val_logging) valNoteRead(addr);
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

#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
		// FALLBACK-path writer probe: log writes to known-corrupt addresses
		// observed in DOA2 freezes. This catches writes that c_dispatch_loop's
		// memwatch misses because the SHIL fallback path runs outside of it.
		{
			u32 masked = addr & ~3;
			if (masked == 0x8c0d9324 || masked == 0x8c1b89b0 ||
			    masked == 0x8c1c47c8 || masked == (0x8c1b89b2 & ~3)) {
				static u32 fbwrite_count = 0;
				if (fbwrite_count < 30) {
					fbwrite_count++;
					u32 val_to_write =
						(op.size == 8) ? *(u32*)((u8*)&ctx + op.rs2.reg_offset())
						               : readI32(op.rs2);
					EM_ASM({
						if (!window._flyLog) window._flyLog = [];
						window._flyLog.push('[FB-WRITE] #' + $0
							+ ' block_vaddr=0x' + ($1>>>0).toString(16)
							+ ' op_idx=' + $2
							+ ' addr=0x' + ($3>>>0).toString(16)
							+ ' val=0x' + ($4>>>0).toString(16)
							+ ' size=' + $5
							+ ' rs1.t=' + $6 + ',off=0x' + ($7>>>0).toString(16)
							+ ' rs3.t=' + $8 + ',off=0x' + ($9>>>0).toString(16));
					}, fbwrite_count, block_vaddr, op_index,
					   addr, val_to_write, op.size,
					   (int)op.rs1.type, op.rs1.is_reg() ? op.rs1.reg_offset() : op.rs1._imm,
					   (int)op.rs3.type, op.rs3.is_reg() ? op.rs3.reg_offset() : op.rs3._imm);
				}
			}
		}
#endif
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
#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
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
			// Log writes for mode 7 comparison if enabled
			if (g_shil_log_writes) {
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
			}
			// (validator logs writes at the WriteMem* wrapper, uniformly)
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
	case shop_illegal: {
		// Raise the SH4 illegal-instruction exception. Previously fell through
		// to the `default` case which was a silent no-op — the SH4 never saw
		// the exception, reg_nextpc stayed stale, and the same illegal PC was
		// dispatched again every iteration (measured: 4 M no-op spirals/sec on
		// Virtua Tennis). Mirrors the canonical shop_illegal defined in
		// shil_canonical.h. The throw is caught by the mainloop's catch block
		// which calls Do_Exception and advances past the faulting instruction.
		u32 epc       = op.rs1._imm;
		u32 delaySlot = op.rs2._imm;
		if (delaySlot == 1)
			throw SH4ThrownException(epc - 2, Sh4Ex_SlotIllegalInstr);
		else
			throw SH4ThrownException(epc, Sh4Ex_IllegalInstr);
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
		// Unknown op — emit a health-critical event (M1: observation only).
		// M4: should halt the mainloop cleanly instead of silently continuing.
		// Any occurrence of this event sets the session's health verdict to RED
		// and inhibits perf interpretation in the report generator.
		FLY_EVT(FLY_EVT_UNHANDLED_OP, (u32)op.op, block_vaddr, op_index, 0);
#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
		static int unhandledCount = 0;
		unhandledCount++;
		if (unhandledCount <= 20) {
			EM_ASM({ console.warn('[rec_wasm] unhandled SHIL fallback op=' + $0 + ' at block 0x' + ($1>>>0).toString(16)); },
				(int)op.op, block_vaddr);
		}
#endif
		break;
	}

#ifndef JIT_PROD_BUILD
	// Profiling: count runtime fallback calls by op type
	if ((int)op.op >= 0 && (int)op.op < 128) prof_fb_by_op[(int)op.op]++;
#endif

#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
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
// EXECUTOR_MODE:
//   6 = pure WASM JIT (normal production)
//   5 = SHIL-vs-ref shadow (finds SHIL interpreter bugs)
//   7 = JIT-vs-ref shadow (finds WASM native-emit bugs) — primary
//       diagnostic tool for the systematic methodology. Logs
//       [SHADOW-JIT] MISMATCH lines with exact divergence info.
//
// Modes 0-5 and 7 all require FORCE_CPP_DISPATCH=1.
// Derived from VALIDATOR_BUILD (see top of file): validator → 7 (JIT-vs-ref
// differential), production → 6 (WASM JIT execution). Set via VALIDATOR_BUILD,
// never independently — independent flag drift shipped the black-screen
// regression 4d0065c.
#if VALIDATOR_BUILD
#define EXECUTOR_MODE 7
#elif INTERP_ONLY
#define EXECUTOR_MODE 0
#elif HYBRID_DISPATCH_BUILD
#define EXECUTOR_MODE 8   // WASM exec via cpp dispatch (c_dispatch_loop bypassed)
#else
#define EXECUTOR_MODE 6
#endif
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
#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
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
	case BET_CLS_COND: {
		// For delayed conditional branches (BT/S, BF/S), the condition was
		// saved in jdyn by shop_jcond before the delay slot executed.
		u32 cond_val = block->has_jcond ? ctx.jdyn : ctx.sr.T;
		if (block->BlockType == BET_Cond_1)
			ctx.pc = cond_val ? block->BranchBlock : block->NextBlock;
		else // BET_Cond_0
			ctx.pc = cond_val ? block->NextBlock : block->BranchBlock;
		break;
	}
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
#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
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
#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
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
#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
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
#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
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
#elif EXECUTOR_MODE == 7
	// JIT-vs-REF SHADOW COMPARISON.
	// Runs each block through BOTH the native WASM JIT and the reference
	// interpreter, compares Sh4Context byte-by-byte, logs the first
	// divergence per block with PC + SHIL op list. Used to pinpoint
	// buggy native-emit code at subsystem level.
	//
	// Execution continues with REF's state (known-correct) so later
	// blocks start from a clean baseline — this keeps the game
	// progressing past already-found bugs instead of cascading every
	// divergence into garbage.
	//
	// Requires FORCE_CPP_DISPATCH=1 to actually run.
	{
		// Halt latch: once a divergence is found, freeze — stop executing so the
		// root-cause state is preserved and nothing cascades on top of it.
		if (g_val_halt) return;

		valInstallWriteHook();  // route WriteMem* through the logging wrapper (once)

		// 1. Save pre-block state
		alignas(16) static u8 pre_buf[sizeof(Sh4Context)];
		memcpy(pre_buf, &ctx, sizeof(Sh4Context));

		// 2. Run the native JIT with write-logging ON. All guest writes route
		//    through wasm_mem_write* (direct-RAM fast path disabled by
		//    WASM_VAL_LOG_WRITES) and are captured in g_val_writes.
		u32 ctx_ptr = (u32)(uintptr_t)&ctx;
		u32 ram_ptr = (u32)(uintptr_t)&mem_b[0];
		g_val_writes.clear();
		g_val_mmio_touched = false;
		g_val_mmio_wrote = false;
		g_val_logging = true;
		ctx.cycle_counter -= block->guest_cycles;
		g_ifb_exception_pending = false;
		int trap = wasm_execute_block(block->vaddr, ctx_ptr, ram_ptr);
		bool jit_ran = !trap;
		bool jit_exc = g_ifb_exception_pending;
		// Deliver the JIT's deferred exception (if any) so its exit pc matches
		// the reference's post-exception pc — symmetric comparison.
		if (jit_exc) {
			Do_Exception(g_ifb_exception_epc, g_ifb_exception_expEvn);
			g_ifb_exception_pending = false;
		}
		if (trap) {
			static u32 jit_skip_count = 0;
			jit_skip_count++;
			if (jit_skip_count <= 5 || (jit_skip_count & 0x3FF) == 0) {
				EM_ASM({ console.log('[SHADOW-JIT-SKIP] #' + $0 + ' blk=' + $1 +
					' pc=0x' + ($2>>>0).toString(16)); },
					jit_skip_count, g_wasm_block_count, block->vaddr);
			}
		}

		// 3. Snapshot JIT result + its write log.
		alignas(16) static u8 jit_buf[sizeof(Sh4Context)];
		memcpy(jit_buf, &ctx, sizeof(Sh4Context));
		std::vector<ValWrite> jit_writes = g_val_writes;

		// 4. UNDO the JIT's RAM writes (reverse order, size bytes) so the
		//    reference runs on clean pre-block memory. MMIO writes can't be
		//    undone — their side effects persist (as in the prior mode 7).
		for (size_t _wi = jit_writes.size(); _wi-- > 0; ) {
			const ValWrite& w = jit_writes[_wi];
			if (!w.is_ram) continue;
			u8* p = &mem_b[(w.addr & 0x1FFFFFFF) & RAM_MASK];
			if (w.size >= 4)      *(u32*)p = w.old_val;
			else if (w.size == 2) *(u16*)p = (u16)w.old_val;
			else                  *p = (u8)w.old_val;
		}

		// 5. Restore pre-state and run the reference: OpPtr — the UNMODIFIED
		//    upstream SH4 interpreter (decodes SH4 directly, no SHIL). This is
		//    the TRUE ground truth: it catches bugs in the SHIL DECODER and in
		//    shared SHIL op semantics that a SHIL-interpreter reference would
		//    miss (both share the decoder). OpPtr sets ctx.pc itself via its
		//    branch handlers, so we do NOT call applyBlockExitCpp. Its writes go
		//    through WriteMem* → the logging wrapper, captured like the JIT's.
		memcpy(&ctx, pre_buf, sizeof(Sh4Context));
		g_val_writes.clear();
		ctx.cycle_counter -= block->guest_cycles;
		g_ifb_exception_pending = false;
		bool ref_exc = false;
		try {
			ref_execute_block(block);
		} catch (const SH4ThrownException& ex) {
			Do_Exception(ex.epc, ex.expEvn);
			ref_exc = true;
		}
		if (g_ifb_exception_pending) {  // FpuDisabled path inside ref_execute_block
			ref_exc = true;
			g_ifb_exception_pending = false;
		}
		std::vector<ValWrite> ref_writes = g_val_writes;
		g_val_logging = false;
		// Memory now reflects the reference's (correct) writes — continue from here.

		// 6. Compare JIT vs reference: registers, RAM writes, exception flag.
		//    On the FIRST divergence: dump everything and HALT (freeze) so the
		//    root cause is captured cleanly before any cascade.
		if (jit_ran && !g_val_halt) {
			static u32 match_count = 0;
			if ((match_count & 0x3FFF) == 0)
				EM_ASM({ console.log('[VAL] alive — ' + $0 + ' blocks verified clean'); }, match_count);

			const Sh4Context& jit_ctx = *(const Sh4Context*)jit_buf;
			const Sh4Context& ref_ctx = ctx;
			bool match = true;
			int diff_idx = -1;
			const char* diff_name = "";
			u32 jit_v = 0, ref_v = 0;
#define JIT_CMP(field, name) \
	if (match && jit_ctx.field != ref_ctx.field) { \
		match = false; diff_name = name; \
		jit_v = (u32)jit_ctx.field; ref_v = (u32)ref_ctx.field; \
	}
#define JIT_CMP_ARR(arr, count, name) \
	if (match) for (int _i = 0; _i < (count); _i++) { \
		if (*(const u32*)&jit_ctx.arr[_i] != *(const u32*)&ref_ctx.arr[_i]) { \
			match = false; diff_name = name; diff_idx = _i; \
			jit_v = *(const u32*)&jit_ctx.arr[_i]; \
			ref_v = *(const u32*)&ref_ctx.arr[_i]; \
			break; \
		} \
	}
			JIT_CMP(pc, "pc")
			JIT_CMP_ARR(r, 16, "r")
			JIT_CMP(sr.T, "sr.T")
			JIT_CMP(sr.status, "sr.status")
			JIT_CMP_ARR(fr, 16, "fr")
			JIT_CMP_ARR(xf, 16, "xf")
			JIT_CMP(mac.l, "mac.l")
			JIT_CMP(mac.h, "mac.h")
			JIT_CMP(pr, "pr")
			JIT_CMP(fpscr.full, "fpscr")
			JIT_CMP(gbr, "gbr")
			JIT_CMP(fpul, "fpul")
			JIT_CMP_ARR(r_bank, 8, "r_bank")
			JIT_CMP(vbr, "vbr")
			JIT_CMP(ssr, "ssr")
			JIT_CMP(spc, "spc")
			JIT_CMP(sgr, "sgr")
			JIT_CMP(dbr, "dbr")
#undef JIT_CMP
#undef JIT_CMP_ARR
			bool reg_match = match;

			// RAM write comparison. Both paths execute the same SHIL ops in the
			// same order, so their RAM-write sequences must be identical.
			bool mem_match = true;
			int wdiff = -1;
			u32 jw_n = 0, rw_n = 0;
			{
				static std::vector<const ValWrite*> jw, rw;
				jw.clear(); rw.clear();
				for (auto& w : jit_writes) if (w.is_ram) jw.push_back(&w);
				for (auto& w : ref_writes) if (w.is_ram) rw.push_back(&w);
				jw_n = (u32)jw.size(); rw_n = (u32)rw.size();
				if (jw.size() != rw.size()) { mem_match = false; }
				else for (size_t i = 0; i < jw.size(); i++) {
					if (jw[i]->addr != rw[i]->addr || jw[i]->size != rw[i]->size ||
					    jw[i]->new_val != rw[i]->new_val) { mem_match = false; wdiff = (int)i; break; }
				}
			}

			bool exc_match = (jit_exc == ref_exc);

			if (reg_match && mem_match && exc_match) {
				match_count++;
			} else if (g_val_mmio_touched) {
				// Block READ or WROTE MMIO — its comparison is inherently unreliable
				// and must NOT halt:
				//  - VOLATILE READS (e.g. TMU TCNT0 @0xffd8000c, a free-running timer)
				//    return DIFFERENT values on the JIT-read vs the ref-read because
				//    the counter ticks between them → false off-by-N reg divergence.
				//    (Confirmed on JGR: a TCNT0 poll loop, r0 differed by exactly 1.)
				//  - WRITES can't be undone, so the ref's re-run sees polluted state.
				// Either way the block's NATIVE compute can't be trusted here. Skip,
				// log lightly, CONTINUE so the validator can reach real (non-MMIO)
				// divergences deeper. (This reverts the MMIO-read-halt experiment from
				// earlier this session — the register-corruption bug it was hunting
				// turned out to be the c_dispatch_loop double-execution, now fixed.)
				static u32 mmio_skip = 0;
				mmio_skip++;
				if (mmio_skip <= 20 || (mmio_skip & 0x3FF) == 0)
					EM_ASM({ console.log('[VAL] mmio-touch block divergence skipped #' + $0 +
						' pc=0x' + ($1>>>0).toString(16) + ' (regs=' + ($2?'OK':'DIFF') +
						' mem=' + ($3?'OK':'DIFF') + ')'); },
						mmio_skip, block->vaddr, reg_match, mem_match);
			} else {
				// ===== REAL divergence — dump + HALT =====
				// Either a pure RAM/compute block, OR an MMIO-write block whose
				// REGISTER or EXCEPTION output diverged (reg/exc comparison is valid
				// regardless of MMIO writes).
				g_val_halt = true;
				EM_ASM({ console.error('================ [VAL] DIVERGENCE — HALTING ================'); });
				EM_ASM({ console.error('[VAL] block_pc=0x' + ($0>>>0).toString(16) + ' nops=' + $1 +
					' guest_cycles=' + $2 + ' (after ' + $3 + ' clean blocks)'); },
					block->vaddr, (u32)block->oplist.size(), block->guest_cycles, match_count);
				EM_ASM({ console.error('[VAL] kind: regs=' + ($0 ? 'OK' : 'DIFF') +
					' mem=' + ($1 ? 'OK' : 'DIFF') + ' exc=' + ($2 ? 'OK' : 'DIFF') +
					' (jit_exc=' + $3 + ' ref_exc=' + $4 + ')'); },
					reg_match, mem_match, exc_match, jit_exc, ref_exc);
				if (!reg_match)
					EM_ASM({ console.error('[VAL] REG DIFF ' + UTF8ToString($0) +
						(($1 >= 0) ? ('[' + $1 + ']') : '') + ' jit=0x' + ($2>>>0).toString(16) +
						' ref=0x' + ($3>>>0).toString(16)); }, diff_name, diff_idx, jit_v, ref_v);
				if (!mem_match) {
					EM_ASM({ console.error('[VAL] MEM DIFF: jit_ramwrites=' + $0 +
						' ref_ramwrites=' + $1 + ' first_diff_idx=' + $2); }, jw_n, rw_n, wdiff);
					for (u32 i = 0; i < jit_writes.size() && i < 24; i++) { auto& w = jit_writes[i];
						if (!w.is_ram) continue;
						EM_ASM({ console.error('[VAL]   jitW addr=0x' + ($0>>>0).toString(16) +
							' sz=' + $1 + ' val=0x' + ($2>>>0).toString(16)); }, w.addr, w.size, w.new_val); }
					for (u32 i = 0; i < ref_writes.size() && i < 24; i++) { auto& w = ref_writes[i];
						if (!w.is_ram) continue;
						EM_ASM({ console.error('[VAL]   refW addr=0x' + ($0>>>0).toString(16) +
							' sz=' + $1 + ' val=0x' + ($2>>>0).toString(16)); }, w.addr, w.size, w.new_val); }
				}
				// Full register dump (jit vs ref)
				for (int i = 0; i < 16; i++)
					EM_ASM({ console.error('[VAL]   r[' + $0 + '] jit=0x' + ($1>>>0).toString(16) +
						' ref=0x' + ($2>>>0).toString(16)); }, i, jit_ctx.r[i], ref_ctx.r[i]);
				EM_ASM({ console.error('[VAL]   pc jit=0x' + ($0>>>0).toString(16) + ' ref=0x' + ($1>>>0).toString(16) +
					' | pr jit=0x' + ($2>>>0).toString(16) + ' ref=0x' + ($3>>>0).toString(16)); },
					jit_ctx.pc, ref_ctx.pc, jit_ctx.pr, ref_ctx.pr);
				EM_ASM({ console.error('[VAL]   sr.status jit=0x' + ($0>>>0).toString(16) + ' ref=0x' + ($1>>>0).toString(16) +
					' | sr.T jit=' + $2 + ' ref=' + $3); }, jit_ctx.sr.status, ref_ctx.sr.status, jit_ctx.sr.T, ref_ctx.sr.T);
				EM_ASM({ console.error('[VAL]   gbr j=0x' + ($0>>>0).toString(16) + ' r=0x' + ($1>>>0).toString(16) +
					' | vbr j=0x' + ($2>>>0).toString(16) + ' r=0x' + ($3>>>0).toString(16) +
					' | spc j=0x' + ($4>>>0).toString(16) + ' r=0x' + ($5>>>0).toString(16) +
					' | ssr j=0x' + ($6>>>0).toString(16) + ' r=0x' + ($7>>>0).toString(16)); },
					jit_ctx.gbr, ref_ctx.gbr, jit_ctx.vbr, ref_ctx.vbr,
					jit_ctx.spc, ref_ctx.spc, jit_ctx.ssr, ref_ctx.ssr);
				// Full SHIL oplist of the diverging block
				for (u32 i = 0; i < block->oplist.size() && i < 64; i++) { auto& sop = block->oplist[i];
					EM_ASM({ console.error('[VAL]   op[' + $0 + '] shop=' + $1 +
						' rd=(t' + $2 + ',0x' + ($3>>>0).toString(16) + ')' +
						' rs1=(t' + $4 + ',0x' + ($5>>>0).toString(16) + ')' +
						' rs2=(t' + $6 + ',0x' + ($7>>>0).toString(16) + ')' +
						' rs3=t' + $8 + ' sz=' + $9); },
						i, (int)sop.op,
						(int)sop.rd.type,  sop.rd.is_reg()  ? sop.rd.reg_offset()  : sop.rd._imm,
						(int)sop.rs1.type, sop.rs1.is_reg() ? sop.rs1.reg_offset() : sop.rs1._imm,
						(int)sop.rs2.type, sop.rs2.is_reg() ? sop.rs2.reg_offset() : sop.rs2._imm,
						(int)sop.rs3.type, sop.size);
				}
				EM_ASM({ console.error('================ [VAL] END DUMP — emulation frozen ================'); });
			}
		}
		// ctx is now the REF result (correct). Continue execution from here.
	}
#elif EXECUTOR_MODE == 8
	// HYBRID ISOLATION (2026-06-11): run the REAL WASM JIT block, but dispatched
	// through this proven-good cpp dispatch inner loop (blockByVaddr.find), with
	// c_dispatch_loop (hash table / SMC hashing / collision detection) entirely
	// bypassed. wasm_execute_block sets ctx.pc itself (verified bit-exact vs ref
	// in mode 7), so no applyBlockExitCpp here. Charges block->guest_cycles, same
	// as the production WASM prologue. Trap count is logged: a clean run means
	// near-zero traps. If Skies boots here, the bug lives in c_dispatch_loop.
	{
		u32 ctx_ptr = (u32)(uintptr_t)&ctx;
		u32 ram_ptr = (u32)(uintptr_t)&mem_b[0];
		// NOTE: do NOT subtract guest_cycles here — the emitted WASM block does it
		// in its own prologue (same as production c_dispatch_loop, which calls the
		// block fn directly with no separate charge). Subtracting here would
		// double-charge and distort timeslice/interrupt timing.
		g_ifb_exception_pending = false;
		int trap = wasm_execute_block(block->vaddr, ctx_ptr, ram_ptr);
		if (g_ifb_exception_pending) {
			Do_Exception(g_ifb_exception_epc, g_ifb_exception_expEvn);
			g_ifb_exception_pending = false;
		}
		if (trap) {
#if !FLY_RELEASE_BUILD
			static u32 hyb_trap = 0;
			hyb_trap++;
			if (hyb_trap <= 20 || (hyb_trap & 0xFFF) == 0)
				EM_ASM({ console.log('[HYBRID-TRAP] #' + $0 + ' blk=' + $1 +
					' pc=0x' + ($2>>>0).toString(16)); },
					hyb_trap, g_wasm_block_count, block->vaddr);
#endif
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

// Whole-block SHIL bridge for async-deferred blocks (v6, 2026-07-17).
// Executes the decoded oplist via wasm_exec_shil_fb, then normalizes the net
// cycle charge to guest_cycles and applies the block exit. The [BRIDGE-CHARGE]
// telemetry counts blocks whose fallback execution charged differently than
// guest_cycles — evidence for whether normalization matches the emitted
// path's charging (emitted blocks add dynamic memory-access penalties).
static void fly_bridge_execute(RuntimeBlockInfo* block)
{
	Sh4Context& ctx = Sh4cntx;
	int cc_pre = ctx.cycle_counter;
	ctx.cycle_counter -= block->guest_cycles;
	for (u32 i = 0; i < block->oplist.size(); i++)
		wasm_exec_shil_fb(block->vaddr, i);
#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
	{
		int fb_charge = cc_pre - ctx.cycle_counter;
		int extra = fb_charge - (int)block->guest_cycles;
		static u32 n = 0, nz = 0;
		static long long sum = 0;
		n++;
		if (extra != 0) { nz++; sum += extra; }
		if ((n & 0xFFF) == 0)
			EM_ASM({ console.log('[BRIDGE-CHARGE] blocks=' + $0 + ' extra_nonzero=' + $1 +
				' avg_extra=' + $2); },
				n, nz, nz ? (double)((double)sum / nz) : 0.0);
	}
#endif
	ctx.cycle_counter = cc_pre - (int)block->guest_cycles;
	applyBlockExitCpp(block);
}

#if FLY_BRIDGE_SHADOW
// Differential: execute `block` via compiled WASM from a snapshot (writes
// logged + undone), restore, execute via the SHIL bridge (stays live), and
// binary-compare the end states. MMIO-writing blocks are skipped (side
// effects unrepeatable — validator policy). cycle_counter is compared
// separately: compiled blocks charge dynamic memory penalties the bridge
// doesn't, so cc deltas are EXPECTED and reported as statistics, not diffs.
static u8 g_bs_pre[sizeof(Sh4Context)];
static u8 g_bs_wasm[sizeof(Sh4Context)];
static u32 g_bs_n = 0, g_bs_diff = 0, g_bs_skip_mmio = 0, g_bs_cc_nonzero = 0;
static long long g_bs_cc_sum = 0;
static std::unordered_set<u32> g_bs_done;   // shadow each pc once
static void fly_bridge_shadow(RuntimeBlockInfo* block, u32 ctx_ptr, u32 ram_ptr)
{
	Sh4Context& ctx = Sh4cntx;
	valInstallWriteHook();
#ifndef JIT_PROD_BUILD
	// SQ activity is unrepeatable device state invisible to WriteMem hooks
	// (chain-shadow lesson) — skip the differential when the WASM run
	// touched the store queues (pref thin-import path).
	extern u32 g_fly_sq[8];
	u32 fly_sq_pre_bs = g_fly_sq[0];
#endif

	// Run 1: compiled WASM from snapshot, writes logged.
	memcpy(g_bs_pre, &ctx, sizeof(Sh4Context));
	g_val_writes.clear();
	g_val_mmio_touched = false;
	g_val_mmio_wrote = false;
	g_val_logging = true;
	g_ifb_exception_pending = false;
	wasm_execute_block(block->vaddr, ctx_ptr, ram_ptr);
	g_val_logging = false;
	bool wasm_pending = g_ifb_exception_pending;
	memcpy(g_bs_wasm, &ctx, sizeof(Sh4Context));

	g_bs_n++;
	// ★ MMIO policy (hard-learned: first shadow build wedged BIOS boot in a
	// GD-ROM poll loop): a block that touched MMIO — read OR write — cannot
	// be re-run (status reads consume/clear on read, writes advance hardware
	// state machines). Keep the WASM run as the live result and skip the
	// differential for this block. Pure-RAM blocks are fully undoable.
	bool fly_bs_sq = false;
#ifndef JIT_PROD_BUILD
	fly_bs_sq = (g_fly_sq[0] != fly_sq_pre_bs);
#endif
	if (g_val_mmio_touched || fly_bs_sq) {
		g_bs_skip_mmio++;
		g_ifb_exception_pending = wasm_pending;   // call site delivers
		if ((g_bs_n & 0x7FF) == 0)
			EM_ASM({ console.log('[BRIDGE-SHADOW] n=' + $0 + ' diffs=' + $1 +
				' mmio_skipped=' + $2 + ' cc_nonzero=' + $3 + ' cc_avg=' + $4); },
				g_bs_n, g_bs_diff, g_bs_skip_mmio, g_bs_cc_nonzero,
				g_bs_cc_nonzero ? (double)((double)g_bs_cc_sum / g_bs_cc_nonzero) : 0.0);
		return;
	}

	// Undo the WASM run's RAM writes (reverse order).
	for (size_t i = g_val_writes.size(); i-- > 0; ) {
		const ValWrite& w = g_val_writes[i];
		if (!w.is_ram) continue;
		u8* p = &mem_b[(w.addr & 0x1FFFFFFF) & RAM_MASK];
		if (w.size >= 4)      *(u32*)p = w.old_val;
		else if (w.size == 2) *(u16*)p = (u16)w.old_val;
		else                  *p = (u8)w.old_val;
	}
	size_t wasm_write_count = g_val_writes.size();

	// Run 2: SHIL bridge from the same snapshot — this result stays LIVE
	// (g_ifb_exception_pending is left for the call site to deliver).
	memcpy(&ctx, g_bs_pre, sizeof(Sh4Context));
	g_val_writes.clear();
	g_ifb_exception_pending = false;
	g_val_logging = true;
	fly_bridge_execute(block);
	g_val_logging = false;
	bool bridge_pending = g_ifb_exception_pending;
	size_t bridge_write_count = g_val_writes.size();

	{
		const Sh4Context& wctx = *(const Sh4Context*)g_bs_wasm;
		bool match = true;
		const char* diff_name = ""; int diff_idx = -1; u32 wv = 0, bv = 0;
#define BS_CMP(field, name) \
		if (match && wctx.field != ctx.field) { match = false; diff_name = name; \
			wv = (u32)wctx.field; bv = (u32)ctx.field; }
#define BS_CMP_ARR(arr, count, name) \
		if (match) for (int _i = 0; _i < (count); _i++) { \
			if (*(const u32*)&wctx.arr[_i] != *(const u32*)&ctx.arr[_i]) { \
				match = false; diff_name = name; diff_idx = _i; \
				wv = *(const u32*)&wctx.arr[_i]; bv = *(const u32*)&ctx.arr[_i]; break; } }
		BS_CMP(pc, "pc")
		BS_CMP_ARR(r, 16, "r")
		BS_CMP(sr.T, "sr.T")
		BS_CMP(sr.status, "sr.status")
		BS_CMP_ARR(fr, 16, "fr")
		BS_CMP_ARR(xf, 16, "xf")
		BS_CMP(mac.l, "mac.l")
		BS_CMP(mac.h, "mac.h")
		BS_CMP(pr, "pr")
		BS_CMP(fpscr.full, "fpscr")
		BS_CMP(gbr, "gbr")
		BS_CMP(fpul, "fpul")
		BS_CMP_ARR(r_bank, 8, "r_bank")
		BS_CMP(vbr, "vbr")
		BS_CMP(ssr, "ssr")
		BS_CMP(spc, "spc")
		BS_CMP(sgr, "sgr")
#undef BS_CMP
#undef BS_CMP_ARR
		if (match && wasm_pending != bridge_pending) {
			match = false; diff_name = "exc_pending";
			wv = wasm_pending; bv = bridge_pending;
		}
		if (match && wasm_write_count != bridge_write_count) {
			match = false; diff_name = "write_count";
			wv = (u32)wasm_write_count; bv = (u32)bridge_write_count;
		}
		if (!match) {
			g_bs_diff++;
			if (g_bs_diff <= 20) {
				EM_ASM({ console.error('[BRIDGE-DIFF] #' + $0 + ' pc=0x' + ($1>>>0).toString(16) +
					' field=' + UTF8ToString($2) + ($3 >= 0 ? ('[' + $3 + ']') : '') +
					' wasm=0x' + ($4>>>0).toString(16) + ' bridge=0x' + ($5>>>0).toString(16) +
					' nops=' + $6); },
					g_bs_diff, block->vaddr, diff_name, diff_idx, wv, bv,
					(u32)block->oplist.size());
				for (u32 i = 0; i < block->oplist.size() && i < 24; i++) {
					auto& sop = block->oplist[i];
					EM_ASM({ console.error('[BRIDGE-DIFF]   op[' + $0 + '] shop=' + $1 +
						' rd=' + $2 + ':0x' + ($3>>>0).toString(16) +
						' rs1=' + $4 + ':0x' + ($5>>>0).toString(16) +
						' rs2=' + $6 + ':0x' + ($7>>>0).toString(16) + ' sz=' + $8); },
						i, (int)sop.op,
						(int)sop.rd.type, sop.rd._imm,
						(int)sop.rs1.type, sop.rs1._imm,
						(int)sop.rs2.type, sop.rs2._imm, sop.size);
				}
			}
		}
		// cycle_counter delta (expected: compiled path charges memory
		// penalties the bridge doesn't — this measures the skew exactly).
		int ccd = wctx.cycle_counter - ctx.cycle_counter;
		if (ccd != 0) { g_bs_cc_nonzero++; g_bs_cc_sum += ccd; }
	}
	if ((g_bs_n & 0x7FF) == 0)
		EM_ASM({ console.log('[BRIDGE-SHADOW] n=' + $0 + ' diffs=' + $1 +
			' mmio_skipped=' + $2 + ' cc_nonzero=' + $3 + ' cc_avg=' + $4); },
			g_bs_n, g_bs_diff, g_bs_skip_mmio, g_bs_cc_nonzero,
			g_bs_cc_nonzero ? (double)((double)g_bs_cc_sum / g_bs_cc_nonzero) : 0.0);
}
#endif  // FLY_BRIDGE_SHADOW

#if FLY_WRITE_PARITY
// Write-parity differential — see the flag comment at the top of the file.
// Reference (SHIL bridge) FIRST with logged writes, undone; compiled WASM
// SECOND with prod-shape inline writes, stays LIVE. Certifies inline write
// values AND the page-gen bump contract per written page.
static u32 g_wp_n = 0, g_wp_diff = 0, g_wp_gen_miss = 0;
static u32 g_wp_skip_mmio = 0, g_wp_skip_trap = 0, g_wp_skip_pages = 0;
static std::unordered_set<u32> g_wp_done;
alignas(16) static u8 g_wp_pre[sizeof(Sh4Context)];
alignas(16) static u8 g_wp_ref[sizeof(Sh4Context)];

static void fly_write_parity_shadow(RuntimeBlockInfo* block, u32 ctx_ptr, u32 ram_ptr)
{
	Sh4Context& ctx = Sh4cntx;
	valInstallWriteHook();
#ifndef JIT_PROD_BUILD
	extern u32 g_fly_sq[8];
	u32 fly_sq_pre_wp = g_fly_sq[0];
#endif

	// Run 1: SHIL bridge (reference) from snapshot, writes logged.
	memcpy(g_wp_pre, &ctx, sizeof(Sh4Context));
	g_val_writes.clear();
	g_val_mmio_touched = false;
	g_val_mmio_wrote = false;
	g_val_logging = true;
	g_ifb_exception_pending = false;
	fly_bridge_execute(block);
	g_val_logging = false;
	bool ref_pending = g_ifb_exception_pending;
	memcpy(g_wp_ref, &ctx, sizeof(Sh4Context));

	g_wp_n++;
	// MMIO/SQ policy (bridge-shadow lesson): such a block cannot be re-run.
	// The reference result stays live; the JIT gets primed at the call site
	// and runs natively from the NEXT dispatch.
	bool wp_sq = false;
#ifndef JIT_PROD_BUILD
	wp_sq = (g_fly_sq[0] != fly_sq_pre_wp);
#endif
	if (g_val_mmio_touched || wp_sq) {
		g_wp_skip_mmio++;
		g_ifb_exception_pending = ref_pending;   // call site delivers
		if ((g_wp_n & 0x7FF) == 0)
			EM_ASM({ console.log('[WRITE-PARITY] n=' + $0 + ' diffs=' + $1 +
				' gen_miss=' + $2 + ' mmio_skipped=' + $3); },
				g_wp_n, g_wp_diff, g_wp_gen_miss, g_wp_skip_mmio);
		return;
	}

	// Undo the reference's RAM writes (reverse order) and collect its write
	// set: a byte-granular expectation map (handles overlapping writes —
	// last writer wins identically in both paths) + the distinct 4KB pages
	// whose gen cells the JIT run must move.
	for (size_t i = g_val_writes.size(); i-- > 0; ) {
		const ValWrite& w = g_val_writes[i];
		if (!w.is_ram) continue;
		u8* p = &mem_b[(w.addr & 0x1FFFFFFF) & RAM_MASK];
		if (w.size >= 4)      *(u32*)p = w.old_val;
		else if (w.size == 2) *(u16*)p = (u16)w.old_val;
		else                  *p = (u8)w.old_val;
	}
	// Byte-granular expectation map with LAST-WRITER-WINS: a block may write
	// the same byte twice (flag toggles measured in farm run 1 — 94 false
	// diffs from comparing EARLY writes against final memory). unordered_map
	// keyed by RAM offset implements the dedup; iteration order is irrelevant
	// for the final compare.
	static std::unordered_map<u32, u8> wp_expect;
	wp_expect.clear();
	u32 wp_pages[16]; u32 wp_page_gen[16]; u32 wp_page_n = 0;
	bool wp_pages_ovf = false;
	for (const ValWrite& w : g_val_writes) {
		if (!w.is_ram) continue;
		u32 off = (w.addr & 0x1FFFFFFF) & RAM_MASK;
		for (u32 b = 0; b < w.size; b++)
			wp_expect[off + b] = (u8)(w.new_val >> (b * 8));
		u32 pg = off >> 12;
		bool seen = false;
		for (u32 i = 0; i < wp_page_n; i++) if (wp_pages[i] == pg) { seen = true; break; }
		if (!seen) {
			if (wp_page_n < 16) {
				wp_pages[wp_page_n] = pg;
				wp_page_gen[wp_page_n] = g_fly_page_gen[pg];
				wp_page_n++;
			} else
				wp_pages_ovf = true;   // >16 distinct pages in one block: don't gen-check
		}
	}
	if (wp_pages_ovf) g_wp_skip_pages++;

	// Run 2: compiled WASM from the same snapshot — writes INLINE, prod
	// shape, no logging. This result stays LIVE (pending left for call site).
	memcpy(&ctx, g_wp_pre, sizeof(Sh4Context));
	g_ifb_exception_pending = false;
	int trap = wasm_execute_block(block->vaddr, ctx_ptr, ram_ptr);
	bool jit_pending = g_ifb_exception_pending;
	if (trap) {
		g_wp_skip_trap++;
		return;
	}

	{
		const Sh4Context& rctx = *(const Sh4Context*)g_wp_ref;   // reference
		bool match = true;
		const char* diff_name = ""; int diff_idx = -1; u32 jv = 0, rv = 0;
#define WP_CMP(field, name) \
		if (match && ctx.field != rctx.field) { match = false; diff_name = name; \
			jv = (u32)ctx.field; rv = (u32)rctx.field; }
#define WP_CMP_ARR(arr, count, name) \
		if (match) for (int _i = 0; _i < (count); _i++) { \
			if (*(const u32*)&ctx.arr[_i] != *(const u32*)&rctx.arr[_i]) { \
				match = false; diff_name = name; diff_idx = _i; \
				jv = *(const u32*)&ctx.arr[_i]; rv = *(const u32*)&rctx.arr[_i]; break; } }
		WP_CMP(pc, "pc")
		WP_CMP_ARR(r, 16, "r")
		WP_CMP(sr.T, "sr.T")
		WP_CMP(sr.status, "sr.status")
		WP_CMP_ARR(fr, 16, "fr")
		WP_CMP_ARR(xf, 16, "xf")
		WP_CMP(mac.l, "mac.l")
		WP_CMP(mac.h, "mac.h")
		WP_CMP(pr, "pr")
		WP_CMP(fpscr.full, "fpscr")
		WP_CMP(gbr, "gbr")
		WP_CMP(fpul, "fpul")
		WP_CMP_ARR(r_bank, 8, "r_bank")
		WP_CMP(vbr, "vbr")
		WP_CMP(ssr, "ssr")
		WP_CMP(spc, "spc")
		WP_CMP(sgr, "sgr")
#undef WP_CMP
#undef WP_CMP_ARR
		if (match && jit_pending != ref_pending) {
			match = false; diff_name = "exc_pending";
			jv = jit_pending; rv = ref_pending;
		}
		// Write-value parity: final memory must hold the reference's bytes.
		u32 wb_diff_off = 0; u8 wb_got = 0, wb_want = 0;
		if (match) for (auto& e : wp_expect) {
			if (mem_b[e.first] != e.second) {
				match = false; diff_name = "write_bytes";
				wb_diff_off = e.first; wb_got = mem_b[e.first]; wb_want = e.second;
				jv = wb_got; rv = wb_want;
				break;
			}
		}
		if (!match) {
			g_wp_diff++;
			if (g_wp_diff <= 20)
				EM_ASM({ console.error('[WP-DIFF] #' + $0 + ' pc=0x' + ($1>>>0).toString(16) +
					' field=' + UTF8ToString($2) + ($3 >= 0 ? ('[' + $3 + ']') : '') +
					' jit=0x' + ($4>>>0).toString(16) + ' ref=0x' + ($5>>>0).toString(16) +
					' off=0x' + ($6>>>0).toString(16) + ' nops=' + $7); },
					g_wp_diff, block->vaddr, diff_name, diff_idx, jv, rv,
					wb_diff_off, (u32)block->oplist.size());
		}
		// GEN-BUMP CONTRACT: every written page's gen cell must have moved.
		if (!wp_pages_ovf) for (u32 i = 0; i < wp_page_n; i++) {
			if (g_fly_page_gen[wp_pages[i]] == wp_page_gen[i]) {
				g_wp_gen_miss++;
				if (g_wp_gen_miss <= 20)
					EM_ASM({ console.error('[WP-GEN-MISS] #' + $0 + ' pc=0x' + ($1>>>0).toString(16) +
						' page=0x' + ($2>>>0).toString(16) + ' gen unchanged (' + $3 + ')'); },
						g_wp_gen_miss, block->vaddr, wp_pages[i], wp_page_gen[i]);
			}
		}
	}
	if ((g_wp_n & 0x7FF) == 0)
		EM_ASM({ console.log('[WRITE-PARITY] n=' + $0 + ' diffs=' + $1 +
			' gen_miss=' + $2 + ' mmio_skipped=' + $3 + ' traps=' + $4 +
			' page_ovf=' + $5); },
			g_wp_n, g_wp_diff, g_wp_gen_miss, g_wp_skip_mmio, g_wp_skip_trap,
			g_wp_skip_pages);
}
#endif  // FLY_WRITE_PARITY

#if FLY_CHAINS_ANY
// ============================================================
// Chain infrastructure (shared by shadow farm and live chaining)
// ============================================================
// Executes a discovered chain BOTH ways from the same snapshot:
//   Run 1: the multi-block chain module (writes logged, undone)
//   Run 2: sequential single-block executions faithfully mirroring the
//          chain's loop-top checks and routing (stays LIVE — proven path)
// and binary-compares the end states + write counts. MMIO-touching runs are
// skipped (validator policy; the chain result stays live for those — it is
// a real execution, just not comparable).
// Per-member fingerprint taken at chain build: a chain is only valid while
// every member's CURRENT RuntimeBlockInfo is the exact object it was built
// from. Members get REPLACED after build (collision recompiles; fpscr-
// differing redecodes produce different oplists from identical RAM), and the
// chain's baked (vaddr, opIndex) fb calls + exit constants + gc charges then
// operate against the wrong oplist — the unified mechanism behind the
// remaining farm diffs (wrong ops, stale exits, gc mismatches) and the
// prime suspect for the historical chain freezes.
struct ChainMemberFp {
	u32 pc;
	RuntimeBlockInfo* blk;   // identity (ABA-guarded by fields below)
	u32 nops;
	u32 gcycles;
	u32 btype;
};
struct ChainInfo {
	std::vector<ChainMemberFp> members;
	u32 table_idx = 0;   // call_indirect idx of the chain module (live mode)
	u32* guard_cells = nullptr;   // 2 u32/member: [pgen baseline, tick] — page-gen
	                              // gating state for interior SMC guards (addresses
	                              // baked into the module); freed on invalidate
};
static std::unordered_map<u32, ChainInfo> g_chain_by_head;
// member pc → heads of chains containing it (invalidation channel)
static std::unordered_map<u32, std::vector<u32>> g_chain_member_index;
static u32 g_chain_invalidated = 0;

// Async chain pipeline (2026-07-18): sweep-discovered candidates are byte-built
// and handed to the browser's off-thread compiler (wasm_compile_chain_async);
// promotion happens in drainChainQueue() after fingerprint + decode-freshness
// re-validation — the async-window version of the staleness law. Pending
// entries own their guard_cells until promoted or dropped.
struct PendingChain {
	std::vector<ChainMemberFp> members;
	u32* guard_cells = nullptr;
	u32 head_size = 0;
};
static std::unordered_map<u32, PendingChain> g_chain_pending;
static void fly_reap_pending_chains()
{
	for (auto& [h, p] : g_chain_pending)
		free(p.guard_cells);
	g_chain_pending.clear();
}
static u32 g_fly_chq_prom = 0, g_fly_chq_drop = 0;

// ============================================================
// Region registry (execution wiring, dark behind FLY_REGIONS_LIVE)
// ============================================================
// (FLY_REGIONS_LIVE / FLY_REGION_SHADOW are defined in the flag block at the
// top of the file — they participate in the WASM_VAL_LOG_WRITES condition.)
struct RegionInfo {
	std::vector<ChainMemberFp> members;   // fingerprints; index == dense entry idx
	u32 table_idx = 0;
	u32 first_page = 0, last_page = 0;
	u32* gen_cell = nullptr;              // baked gen-sum baseline (owned)
	// Step-3 stage 1 (2026-07-21): monomorphic inline cache, one
	// {cached_pc, cached_idx} pair per member site (2N u32, owned).
	// Module probes pc-equality and routes; C fills on miss via the
	// g_region_dyn_miss mailbox. Pair written together (single-threaded)
	// so idx is always consistent with pc; cells die with the region.
	u32* ic_cells = nullptr;
	// Step-3 stage 2: shadow return stack ([0]=sp, [1..16]=entries; sp
	// reset in the region prologue — no cross-entry state) + baked
	// idx→vaddr table for pop verification (popped idx is dynamic, so
	// the pc-equality check must go through a table, not a constant).
	u32* ras_cells = nullptr;
	u32* pc_table = nullptr;
};
// IC miss mailbox: module stores (region_slot<<16)|site_idx on an IC miss;
// the dispatch loop resolves ctx.pc → member idx and fills the site's pair.
// 0xFFFFFFFF = empty.
u32 g_region_dyn_miss = 0xFFFFFFFFu;
#ifndef JIT_PROD_BUILD
u32 g_region_ic_ct[2];   // [0]=hit [1]=miss (in-module dev ticks)
u32 g_region_ras_ct[2];  // [0]=hit [1]=mispredict (pop verify failed)
#endif
static std::vector<RegionInfo> g_regions;             // tombstoned on invalidate
static std::unordered_map<u32, u32> g_region_member_of;  // pc → g_regions index + 1
static u32 g_regions_invalidated = 0;
static void fly_region_invalidate(u32 ridx);

// Rebuild the write-watch registry from live regions. gen_cell is a 2-slot
// allocation: [0] = baseline (module guard compares against), [1] = cur
// (bumped by fly_ram_written on in-range writes).
static void fly_region_watch_rebuild()
{
	g_rwatch_n = 0;
	for (RegionInfo& r : g_regions) {
		if (r.table_idx == 0 || !r.gen_cell) continue;
		if (g_rwatch_n >= FLY_RWATCH_MAX) break;
		g_rwatch_lo[g_rwatch_n] = r.first_page;
		g_rwatch_hi[g_rwatch_n] = r.last_page;
		g_rwatch_cell[g_rwatch_n] = r.gen_cell + 1;
		g_rwatch_n++;
	}
}

// Kill every chain containing `pc` — called whenever a block is replaced or
// evicted. The shadow farm proved this invariant non-negotiable: 1212 chains
// per 300s run went stale, each one a corrupted execution in the old design.
static void fly_chain_invalidate(u32 pc)
{
	// Regions share the staleness law — BUT fly_chain_invalidate also fires on
	// benign dispatch-slot COLLISION evictions (block unchanged, just lost its
	// table slot). Chains tolerate that (1.5 members, cheap rebuild); a
	// 543-member region does not — one benign eviction anywhere killed the
	// whole fight region within frames (farm run 12). So only invalidate when
	// the member is GENUINELY replaced: gone from blockByVaddr, or its
	// fingerprint changed. A live-and-matching block = benign; keep the region.
	{
		auto rit = g_region_member_of.find(pc);
		if (rit != g_region_member_of.end()) {
			u32 rj = rit->second - 1;
			bool genuine = true;
			if (rj < (u32)g_regions.size()) {
				RegionInfo& rr = g_regions[rj];
				auto bib = blockByVaddr.find(pc);
				if (bib != blockByVaddr.end()) {
					for (const ChainMemberFp& mm : rr.members) {
						if (mm.pc != pc) continue;
						RuntimeBlockInfo* nb = bib->second;
						// Content identity, not pointer (run 13): identical
						// re-decode is benign; only real code change invalidates.
						genuine = ((u32)nb->oplist.size() != mm.nops
						    || nb->guest_cycles != mm.gcycles
						    || (u32)nb->BlockType != mm.btype);
						if (!genuine) {
							auto sm = g_block_smc.find(pc);
							if (sm != g_block_smc.end() && sm->second.sz != 0
							    && hashRamBlock(pc, sm->second.sz) != sm->second.hash)
								genuine = true;
						}
						break;
					}
				}
			}
			if (genuine)
				fly_region_invalidate(rj);
		}
	}
	auto it = g_chain_member_index.find(pc);
	if (it == g_chain_member_index.end())
		return;
	for (u32 head : it->second) {
		auto cit = g_chain_by_head.find(head);
		if (cit == g_chain_by_head.end())
			continue;
		// If the head's dispatch slot points at this chain module, clear it —
		// the next dispatch re-primes to the single-block via the cached idx.
		u32 k = (head >> 1) & JIT_TABLE_MASK;
		if (jit_dispatch_pc[k] == head && jit_dispatch_table[k] == cit->second.table_idx) {
			jit_dispatch_table[k] = 0; jit_dispatch_pc[k] = 0;
			jit_dispatch_hash[k] = 0; jit_dispatch_sz[k] = 0;
		}
		wasm_remove_chain(head);
		free(cit->second.guard_cells);
		cit->second.guard_cells = nullptr;
		g_chain_by_head.erase(cit);
		g_chain_invalidated++;
	}
	g_chain_member_index.erase(it);
}

// Tear down region `ridx`: clear member head slots that point at the region
// module, unregister the module, free the baseline cell. Entry left as a
// tombstone (index stability for g_region_member_of values).
static void fly_region_invalidate(u32 ridx)
{
	if (ridx >= (u32)g_regions.size())
		return;
	RegionInfo& r = g_regions[ridx];
	if (r.table_idx == 0)
		return;   // already tombstoned
	for (const ChainMemberFp& m : r.members) {
		u32 k = (m.pc >> 1) & JIT_TABLE_MASK;
		if (jit_dispatch_pc[k] == m.pc && jit_dispatch_table[k] == r.table_idx) {
			jit_dispatch_table[k] = 0; jit_dispatch_pc[k] = 0;
			jit_dispatch_hash[k] = 0; jit_dispatch_sz[k] = 0;
			jit_dispatch_arg[k] = 0;
		}
		g_region_member_of.erase(m.pc);
	}
	wasm_remove_chain(0xFFFF0000u + ridx);   // region modules live in the chain cache
	free(r.gen_cell);
	r.gen_cell = nullptr;
	free(r.ic_cells);
	r.ic_cells = nullptr;
	free(r.ras_cells);
	r.ras_cells = nullptr;
	free(r.pc_table);
	r.pc_table = nullptr;
	g_region_dyn_miss = 0xFFFFFFFFu;   // mailbox may reference this region
	r.table_idx = 0;
	r.members.clear();
	g_regions_invalidated++;
	fly_region_watch_rebuild();   // stop write-path bumps into the freed cell
}
static std::unordered_set<u32> g_cs_done;
static u32 g_cs_n = 0, g_cs_diff = 0, g_cs_skip_mmio = 0, g_cs_trap = 0;
static u32 g_cs_infra = 0;   // reference stopped on infrastructure (eviction) — not comparable
static u32 g_cs_skip_dev = 0; // SQ/TA device state touched — unrepeatable (TAWriteSQ bypasses WriteMem hooks)
static u32 g_cs_stale = 0;   // member replaced since chain build — chain invalid

static bool fly_chain_is_stale(const ChainInfo& ci)
{
	for (const ChainMemberFp& m : ci.members) {
		auto it = blockByVaddr.find(m.pc);
		if (it == blockByVaddr.end()) return true;
		RuntimeBlockInfo* b = it->second;
		if (b != m.blk || (u32)b->oplist.size() != m.nops
		    || b->guest_cycles != m.gcycles || (u32)b->BlockType != m.btype)
			return true;
	}
	return false;
}
static u8 g_cs_pre[sizeof(Sh4Context)];
static u8 g_cs_ref[sizeof(Sh4Context)];
// Itinerary traces: the chain module stores each visited link index here
// (emission in buildMultiBlockModule, shadow builds only); the reference
// records executed pcs. Dumped on divergence — names the fork link.
u32 g_cs_trace[128];
u32 g_cs_trace_n = 0;
static u32 g_cs_ref_trace[128];
static u32 g_cs_ref_trace_n = 0;

#if FLY_CHAIN_SHADOW
// ★ v2 ORDER (2026-07-17, post-Opus review): REFERENCE FIRST, and its result
// is ALWAYS what continues live — chain executions can never leak into game
// state (v1 kept the chain result live on MMIO-touched runs: hundreds of
// untrusted chain executions injected per farm run, the suspected cause of
// the BLANK/reboot-churn farm captures). If the reference touched MMIO the
// chain is never run (unrepeatable). The chain runs from the restored
// snapshot, is compared, undone, and the reference state + writes are
// re-applied.
static void fly_chain_shadow(u32 head_pc, u32 ctx_ptr, u32 ram_ptr)
{
	Sh4Context& ctx = Sh4cntx;
	auto mit = g_chain_by_head.find(head_pc);
	if (mit == g_chain_by_head.end() || !wasm_has_chain(head_pc))
		return;
	if (fly_chain_is_stale(mit->second)) {
		// A member was replaced since build — the chain's baked fb indices,
		// exit constants and gc charges no longer describe reality. Drop it.
		g_cs_stale++;
		g_chain_by_head.erase(mit);
		return;
	}
	std::vector<u32> members;
	for (const ChainMemberFp& m : mit->second.members)
		members.push_back(m.pc);
	valInstallWriteHook();
#ifndef JIT_PROD_BUILD
	// SQ-write counter (storeq.cpp) — TAWriteSQ/doSqWrite mutate GLOBAL TA
	// device state that no snapshot restores AND bypass the WriteMem hooks
	// (the DOA2 lesson). Any SQ activity during the reference makes the
	// chain re-run non-repeatable (measured: ref's pref raised the TA
	// list-complete interrupt, chain's identical replay into the advanced
	// TA state machine could not).
	extern u32 g_fly_sq[8];
	u32 fly_sq_pre = g_fly_sq[0];
#endif

	// Run 1: sequential single-block REFERENCE from snapshot (stays live).
	// Mirrors the chain module: cc + interrupt checks before each link,
	// routing only along the just-run block's static targets that are chain
	// members, stop on pending exception (left set for the caller).
	memcpy(g_cs_pre, &ctx, sizeof(Sh4Context));
	g_val_writes.clear();
	g_val_mmio_touched = false;
	g_val_mmio_wrote = false;
	g_val_logging = true;
	g_ifb_exception_pending = false;
	// Stop reasons: 0=cc 1=int 2=route 3=member 4=EVICTED(infra) 5=pending
	// 6=guard 7=head-mismatch
	int ref_stop = -1;
	g_cs_ref_trace_n = 0;
	{
		u32 expect_pc = head_pc;
		RuntimeBlockInfo* prev = nullptr;
		int guard = 0;
		for (;;) {
			if (guard++ >= 4096) { ref_stop = 6; break; }
			if (ctx.cycle_counter <= 0) { ref_stop = 0; break; }
			if (ctx.interrupt_pend) { ref_stop = 1; break; }
			u32 pc = ctx.pc;
			if (prev != nullptr) {
				u32 bcls = BET_GET_CLS(prev->BlockType);
				bool route_ok = false;
				if (bcls == BET_CLS_Static && prev->BlockType != BET_StaticIntr)
					route_ok = (pc == prev->BranchBlock);
				else if (bcls == BET_CLS_COND)
					route_ok = (pc == prev->BranchBlock || pc == prev->NextBlock);
				if (!route_ok) { ref_stop = 2; break; }
				bool member = false;
				for (u32 m : members) if (m == pc) { member = true; break; }
				if (!member) { ref_stop = 3; break; }
			} else if (pc != expect_pc) {
				ref_stop = 7;
				break;
			}
			auto bit = blockByVaddr.find(pc);
			if (bit == blockByVaddr.end() || !wasm_has_block(pc)) { ref_stop = 4; break; }
			if (g_cs_ref_trace_n < 128)
				g_cs_ref_trace[g_cs_ref_trace_n++] = pc;
			wasm_execute_block(pc, ctx_ptr, ram_ptr);
			if (g_ifb_exception_pending) { ref_stop = 5; break; }
			prev = bit->second;
		}
	}
	g_val_logging = false;
	bool ref_pending = g_ifb_exception_pending;
	bool ref_mmio = g_val_mmio_touched;
	size_t ref_writes = g_val_writes.size();
	memcpy(g_cs_ref, &ctx, sizeof(Sh4Context));

	g_cs_n++;
	if (ref_mmio) {
		// Reference touched MMIO — unrepeatable, chain never runs.
		// Live state = reference result (already in place).
		g_cs_skip_mmio++;
		return;
	}
#ifndef JIT_PROD_BUILD
	if (g_fly_sq[0] != fly_sq_pre) {
		// SQ/TA device activity — unrepeatable, chain never runs.
		g_cs_skip_dev++;
		return;
	}
#endif
	if (ref_stop == 4) {
		// A member was evicted since chain compile — the reference can't
		// replicate the chain's path. Not comparable; count separately so
		// eviction noise never masquerades as chain divergence.
		g_cs_infra++;
		return;
	}

	// Save + undo the reference's RAM writes, restore the snapshot.
	std::vector<ValWrite> refw = g_val_writes;
	for (size_t i = refw.size(); i-- > 0; ) {
		const ValWrite& w = refw[i];
		if (!w.is_ram) continue;
		u8* p = &mem_b[(w.addr & 0x1FFFFFFF) & RAM_MASK];
		if (w.size >= 4)      *(u32*)p = w.old_val;
		else if (w.size == 2) *(u16*)p = (u16)w.old_val;
		else                  *p = (u8)w.old_val;
	}
	memcpy(&ctx, g_cs_pre, sizeof(Sh4Context));

	// Run 2: chain module from the same snapshot, writes logged.
	g_val_writes.clear();
	g_val_mmio_touched = false;
	g_val_logging = true;
	g_ifb_exception_pending = false;
	g_cs_trace_n = 0;
	int trap = wasm_execute_chain(head_pc, ctx_ptr, ram_ptr);
	g_val_logging = false;
	bool chain_pending = g_ifb_exception_pending;
	size_t chain_writes = g_val_writes.size();
	if (trap) g_cs_trap++;

	// Compare chain result (live ctx right now) vs reference (g_cs_ref).
	{
		const Sh4Context& cctx = ctx;                            // chain result
		const Sh4Context& rctx = *(const Sh4Context*)g_cs_ref;   // reference
		bool match = true;
		const char* diff_name = ""; int diff_idx = -1; u32 cv = 0, rv = 0;
#define CS_CMP(field, name) \
		if (match && cctx.field != rctx.field) { match = false; diff_name = name; \
			cv = (u32)cctx.field; rv = (u32)rctx.field; }
#define CS_CMP_ARR(arr, count, name) \
		if (match) for (int _i = 0; _i < (count); _i++) { \
			if (*(const u32*)&cctx.arr[_i] != *(const u32*)&rctx.arr[_i]) { \
				match = false; diff_name = name; diff_idx = _i; \
				cv = *(const u32*)&cctx.arr[_i]; rv = *(const u32*)&rctx.arr[_i]; break; } }
		// Order: registers before pc — the earliest-cause field should
		// surface, and pc is almost always downstream of a register diff.
		CS_CMP_ARR(r, 16, "r")
		CS_CMP(jdyn, "jdyn")
		CS_CMP(sr.T, "sr.T")
		CS_CMP(sr.status, "sr.status")
		CS_CMP_ARR(fr, 16, "fr")
		CS_CMP_ARR(xf, 16, "xf")
		CS_CMP(mac.l, "mac.l")
		CS_CMP(mac.h, "mac.h")
		CS_CMP(pr, "pr")
		CS_CMP(fpscr.full, "fpscr")
		CS_CMP(gbr, "gbr")
		CS_CMP(fpul, "fpul")
		CS_CMP_ARR(r_bank, 8, "r_bank")
		CS_CMP(vbr, "vbr")
		CS_CMP(ssr, "ssr")
		CS_CMP(spc, "spc")
		CS_CMP(interrupt_pend, "interrupt_pend")
		CS_CMP(cycle_counter, "cycle_counter")
		CS_CMP(pc, "pc")
#undef CS_CMP
#undef CS_CMP_ARR
		if (match && chain_pending != ref_pending) {
			match = false; diff_name = "exc_pending";
			cv = chain_pending; rv = ref_pending;
		}
		if (match && chain_writes != ref_writes) {
			match = false; diff_name = "write_count";
			cv = (u32)chain_writes; rv = (u32)ref_writes;
		}
		if (!match) {
			g_cs_diff++;
			if (g_cs_diff <= 20) {
				EM_ASM({ console.error('[CHAIN-DIFF] #' + $0 + ' head=0x' + ($1>>>0).toString(16) +
					' links=' + $2 + ' field=' + UTF8ToString($3) +
					($4 >= 0 ? ('[' + $4 + ']') : '') +
					' chain=0x' + ($5>>>0).toString(16) + ' ref=0x' + ($6>>>0).toString(16) +
					' | cc chain=' + $7 + ' ref=' + $8 + ' ref_stop=' + $9); },
					g_cs_diff, head_pc, (u32)members.size(), diff_name, diff_idx, cv, rv,
					cctx.cycle_counter, rctx.cycle_counter, ref_stop);
				for (u32 mi = 0; mi < members.size() && mi < 12; mi++) {
					auto lit = blockByVaddr.find(members[mi]);
					u32 bt = (lit != blockByVaddr.end()) ? (u32)lit->second->BlockType : 0xFFFFFFFF;
					u32 gc = (lit != blockByVaddr.end()) ? lit->second->guest_cycles : 0;
					u32 bb = (lit != blockByVaddr.end()) ? lit->second->BranchBlock : 0;
					u32 nb = (lit != blockByVaddr.end()) ? lit->second->NextBlock : 0;
					EM_ASM({ console.error('[CHAIN-DIFF]   link[' + $0 + '] pc=0x' + ($1>>>0).toString(16) +
						' BT=0x' + ($2>>>0).toString(16) + ' gc=' + $3 +
						' br=0x' + ($4>>>0).toString(16) + ' nb=0x' + ($5>>>0).toString(16)); },
						mi, members[mi], bt, gc, bb, nb);
				}
				// Itineraries: chain link-index sequence vs reference pc sequence.
				for (u32 ti = 0; ti < g_cs_trace_n && ti < 32; ti++)
					EM_ASM({ console.error('[CHAIN-DIFF]   chain_visit[' + $0 + '] link=' + $1); },
						ti, g_cs_trace[ti]);
				for (u32 ti = 0; ti < g_cs_ref_trace_n && ti < 32; ti++)
					EM_ASM({ console.error('[CHAIN-DIFF]   ref_visit[' + $0 + '] pc=0x' + ($1>>>0).toString(16)); },
						ti, g_cs_ref_trace[ti]);
				// End-state int_pend both sides + oplists of the first two
				// chain-visited links (fork forensics).
				EM_ASM({ console.error('[CHAIN-DIFF]   int_pend chain=' + $0 + ' ref=' + $1); },
					cctx.interrupt_pend, rctx.interrupt_pend);
				for (u32 vi = 0; vi < g_cs_trace_n && vi < 2; vi++) {
					u32 li = g_cs_trace[vi];
					if (li >= members.size()) continue;
					auto lit2 = blockByVaddr.find(members[li]);
					if (lit2 == blockByVaddr.end()) continue;
					RuntimeBlockInfo* lb = lit2->second;
					for (u32 oi = 0; oi < lb->oplist.size() && oi < 20; oi++) {
						auto& sop = lb->oplist[oi];
						EM_ASM({ console.error('[CHAIN-DIFF]   link' + $0 + '.op[' + $1 + '] shop=' + $2 +
							' rd=' + $3 + ':0x' + ($4>>>0).toString(16) +
							' rs1=' + $5 + ':0x' + ($6>>>0).toString(16)); },
							li, oi, (int)sop.op,
							(int)sop.rd.type, sop.rd._imm,
							(int)sop.rs1.type, sop.rs1._imm);
					}
				}
			}
		}
	}
	// Discard the chain run entirely: undo its RAM writes, restore the
	// reference's end state, re-apply the reference's writes (forward
	// order). Live execution continues from the PROVEN path uncontaminated.
	for (size_t i = g_val_writes.size(); i-- > 0; ) {
		const ValWrite& w = g_val_writes[i];
		if (!w.is_ram) continue;
		u8* p = &mem_b[(w.addr & 0x1FFFFFFF) & RAM_MASK];
		if (w.size >= 4)      *(u32*)p = w.old_val;
		else if (w.size == 2) *(u16*)p = (u16)w.old_val;
		else                  *p = (u8)w.old_val;
	}
	memcpy(&ctx, g_cs_ref, sizeof(Sh4Context));
	for (const ValWrite& w : refw) {
		if (!w.is_ram) continue;
		u8* p = &mem_b[(w.addr & 0x1FFFFFFF) & RAM_MASK];
		if (w.size >= 4)      *(u32*)p = w.new_val;
		else if (w.size == 2) *(u16*)p = (u16)w.new_val;
		else                  *p = (u8)w.new_val;
	}
	g_ifb_exception_pending = ref_pending;

	if ((g_cs_n & 0xFF) == 0)
		EM_ASM({ console.log('[CHAIN-SHADOW] n=' + $0 + ' diffs=' + $1 +
			' mmio_skipped=' + $2 + ' infra_skipped=' + $3 + ' dev_skipped=' + $4 +
			' stale=' + $5 + ' traps=' + $6); },
			g_cs_n, g_cs_diff, g_cs_skip_mmio, g_cs_infra, g_cs_skip_dev,
			g_cs_stale, g_cs_trap);
}
#endif  // FLY_CHAIN_SHADOW

#if FLY_REGION_SHADOW
// Region differential (chain-shadow adapted): reference = sequential singles
// routed within region MEMBERSHIP from the entry pc; then the region module
// from the same snapshot; compare; reference stays live (reference-first law).
static std::unordered_set<u32> g_rs_done;
static u32 g_rs_n = 0, g_rs_diff = 0, g_rs_skip_mmio = 0, g_rs_skip_dev = 0;
static u32 g_rs_skip_smc = 0, g_rs_infra = 0, g_rs_stale = 0, g_rs_trap = 0;
u32 g_rs_hook_hits = 0, g_rs_early_ret = 0;   // farm-pump diagnostics
static void fly_region_shadow(u32 entry_pc, u32 ctx_ptr, u32 ram_ptr)
{
	Sh4Context& ctx = Sh4cntx;
	g_rs_hook_hits++;
	auto rit = g_region_member_of.find(entry_pc);
	if (rit == g_region_member_of.end())
		{ g_rs_early_ret++; return; }
	u32 ridx = rit->second - 1;
	if (ridx >= (u32)g_regions.size())
		{ g_rs_early_ret++; return; }
	RegionInfo& reg = g_regions[ridx];
	if (reg.table_idx == 0)
		{ g_rs_early_ret++; return; }
	// Staleness by CONTENT, not pointer identity (farm run 13): a member
	// collision-evicted and re-decoded IDENTICALLY gets a new RuntimeBlockInfo*
	// but the same code — the region is still valid. Raw-pointer comparison
	// false-invalidated the whole 543-block region on the first such churn.
	// Check content fingerprint (nops/gcycles/btype) + live RAM hash instead;
	// genuine SMC still changes the hash and invalidates.
	for (const ChainMemberFp& m : reg.members) {
		auto bit0 = blockByVaddr.find(m.pc);
		bool bad = (bit0 == blockByVaddr.end()
		    || (u32)bit0->second->oplist.size() != m.nops
		    || bit0->second->guest_cycles != m.gcycles
		    || (u32)bit0->second->BlockType != m.btype);
		if (!bad) {
			auto s0 = g_block_smc.find(m.pc);
			if (s0 != g_block_smc.end() && s0->second.sz != 0
			    && hashRamBlock(m.pc, s0->second.sz) != s0->second.hash)
				bad = true;
		}
		if (bad) {
			g_rs_stale++;
			EM_ASM({ console.log('[REGION-SHADOW] STALE member=0x' + ($0>>>0).toString(16) +
				' region invalidated (n=' + $1 + ' so far)'); }, m.pc, g_rs_n);
			fly_region_invalidate(ridx);
			return;
		}
	}
	// Dense entry index
	u32 entry_idx = 0xFFFFFFFF;
	for (u32 mi = 0; mi < (u32)reg.members.size(); mi++)
		if (reg.members[mi].pc == entry_pc) { entry_idx = mi; break; }
	if (entry_idx == 0xFFFFFFFF)
		{ g_rs_early_ret++; return; }

	valInstallWriteHook();
#ifndef JIT_PROD_BUILD
	extern u32 g_fly_sq[8];
	u32 fly_sq_pre = g_fly_sq[0];
#endif
	// Run 1: reference (stays live). Stop reasons: 0=cc 1=int 2=route
	// 3=member 4=EVICTED 5=pending 6=guard 7=entry-mismatch
	memcpy(g_cs_pre, &ctx, sizeof(Sh4Context));
	g_val_writes.clear();
	g_val_mmio_touched = false;
	g_val_logging = true;
	g_ifb_exception_pending = false;
	int ref_stop = -1;
	{
		u32 expect_pc = entry_pc;
		RuntimeBlockInfo* prev = nullptr;
		int guard = 0;
		// ★ HARNESS v2 (step-3): the reference must stop exactly where the
		// region stops, and the region now routes THROUGH dyn edges via the
		// IC cells and the shadow return stack. Mirror both here: simulate
		// the RAS (push at call sites with in-region hints, pop+verify at
		// rts) and read the region's LIVE ic_cells for call/jump edges.
		// Cells are not mutated between this sim and run 2 (the shadow
		// build has no LIVE fill hook; fills happen post-compare below).
		std::unordered_map<u32, u32> pcIdx;
		for (u32 mi2 = 0; mi2 < (u32)reg.members.size(); mi2++)
			pcIdx[reg.members[mi2].pc] = mi2;
		u32 sim_sp = 0;
		u32 sim_ras[16];
		for (;;) {
			if (guard++ >= 8192) { ref_stop = 6; break; }
			if (ctx.cycle_counter <= 0) { ref_stop = 0; break; }
			if (ctx.interrupt_pend) { ref_stop = 1; break; }
			u32 pc = ctx.pc;
			if (prev != nullptr) {
				u32 bcls = BET_GET_CLS(prev->BlockType);
				bool route_ok = false;
				if (bcls == BET_CLS_Static && prev->BlockType != BET_StaticIntr)
					route_ok = (pc == prev->BranchBlock);
				else if (bcls == BET_CLS_COND)
					route_ok = (pc == prev->BranchBlock || pc == prev->NextBlock);
				else if (bcls == BET_CLS_Dynamic) {
					if (prev->BlockType == BET_DynamicRet) {
						// Region pops (consuming the entry) and routes only
						// on pc_table match; empty stack → region exits.
						if (sim_sp > 0) {
							u32 popped = sim_ras[--sim_sp];
							route_ok = (popped < (u32)reg.members.size()
							    && reg.members[popped].pc == pc);
						}
					} else if (prev->BlockType == BET_DynamicCall
					           || prev->BlockType == BET_DynamicJump) {
						// Region routes only on an IC pc match at this site.
						if (reg.ic_cells) {
							auto pit = pcIdx.find(prev->vaddr);
							if (pit != pcIdx.end())
								route_ok = (reg.ic_cells[pit->second * 2] == pc);
						}
					}
					// DynamicIntr: region always exits → route_ok stays false
				}
				if (!route_ok) { ref_stop = 2; break; }
				auto mm = g_region_member_of.find(pc);
				if (mm == g_region_member_of.end() || mm->second - 1 != ridx) { ref_stop = 3; break; }
			} else if (pc != expect_pc) {
				ref_stop = 7;
				break;
			}
			auto bit = blockByVaddr.find(pc);
			if (bit == blockByVaddr.end() || !wasm_has_block(pc)) { ref_stop = 4; break; }
			wasm_execute_block(pc, ctx_ptr, ram_ptr);
			if (g_ifb_exception_pending) { ref_stop = 5; break; }
			prev = bit->second;
			// Mirror the module's RAS push (hint pushed whether or not the
			// call then stays in-region; saturate at 16, never wrap)
			if ((prev->BlockType == BET_StaticCall
			     || prev->BlockType == BET_DynamicCall) && sim_sp < 16) {
				auto rh = pcIdx.find(prev->NextBlock);
				if (rh != pcIdx.end())
					sim_ras[sim_sp++] = rh->second;
			}
		}
	}
	g_val_logging = false;
	bool ref_pending = g_ifb_exception_pending;
	bool ref_mmio = g_val_mmio_touched;
	size_t ref_writes = g_val_writes.size();
	memcpy(g_cs_ref, &ctx, sizeof(Sh4Context));

	g_rs_n++;
	if (ref_mmio) { g_rs_skip_mmio++; return; }
#ifndef JIT_PROD_BUILD
	if (g_fly_sq[0] != fly_sq_pre) { g_rs_skip_dev++; return; }
#endif
	if (ref_stop == 4) { g_rs_infra++; return; }
	// In-range guest writes flip the region's gen-guard mid-run — the region
	// legitimately exits early where the reference continued. Not comparable.
	for (const ValWrite& w : g_val_writes) {
		if (!w.is_ram) continue;
		u32 wpg = ((w.addr & 0x1FFFFFFF) & RAM_MASK) >> 12;
		if (wpg >= reg.first_page && wpg <= reg.last_page) { g_rs_skip_smc++; return; }
	}

	// Undo reference writes, restore snapshot
	std::vector<ValWrite> refw = g_val_writes;
	for (size_t i = refw.size(); i-- > 0; ) {
		const ValWrite& w = refw[i];
		if (!w.is_ram) continue;
		u8* p = &mem_b[(w.addr & 0x1FFFFFFF) & RAM_MASK];
		if (w.size >= 4)      *(u32*)p = w.old_val;
		else if (w.size == 2) *(u16*)p = (u16)w.old_val;
		else                  *p = (u8)w.old_val;
	}
	memcpy(&ctx, g_cs_pre, sizeof(Sh4Context));

	// Run 2: region module from the same snapshot
	g_val_writes.clear();
	g_val_mmio_touched = false;
	g_val_logging = true;
	g_ifb_exception_pending = false;
	g_region_entry_idx = entry_idx;
	int trap = wasm_execute_chain(0xFFFF0000u + ridx, ctx_ptr, ram_ptr);
	g_val_logging = false;
	bool reg_pending = g_ifb_exception_pending;
	size_t reg_writes = g_val_writes.size();
	if (trap) g_rs_trap++;

	// ★ HARNESS v2: process the IC-miss mailbox the region posted during
	// run 2 (the shadow build has no LIVE fill hook). ctx.pc right now is
	// the region's exit target. Filling AFTER the reference sim read the
	// cells keeps this comparison consistent; the farm pump re-arms the
	// member, so the NEXT comparison exercises the freshly-filled hit path
	// — ICs converge across the run and hit routing gets real coverage.
	if (g_region_dyn_miss != 0xFFFFFFFFu) {
		u32 enc = g_region_dyn_miss;
		g_region_dyn_miss = 0xFFFFFFFFu;
		u32 rs2 = enc >> 16, si2 = enc & 0xFFFFu;
		if (rs2 == ridx && reg.ic_cells && si2 < (u32)reg.members.size()) {
			u32 tpc2 = ctx.pc;
			for (u32 ti2 = 0; ti2 < (u32)reg.members.size(); ti2++) {
				if (reg.members[ti2].pc == tpc2) {
					reg.ic_cells[si2 * 2 + 1] = ti2;
					reg.ic_cells[si2 * 2] = tpc2;
					break;
				}
			}
		}
	}

	{
		const Sh4Context& cctx = ctx;
		const Sh4Context& rctx = *(const Sh4Context*)g_cs_ref;
		bool match = true;
		const char* diff_name = ""; int diff_idx = -1; u32 cv = 0, rv = 0;
#define RS_CMP(field, name) \
		if (match && cctx.field != rctx.field) { match = false; diff_name = name; \
			cv = (u32)cctx.field; rv = (u32)rctx.field; }
#define RS_CMP_ARR(arr, count, name) \
		if (match) for (int _i = 0; _i < (count); _i++) { \
			if (*(const u32*)&cctx.arr[_i] != *(const u32*)&rctx.arr[_i]) { \
				match = false; diff_name = name; diff_idx = _i; \
				cv = *(const u32*)&cctx.arr[_i]; rv = *(const u32*)&rctx.arr[_i]; break; } }
		RS_CMP_ARR(r, 16, "r")
		RS_CMP(jdyn, "jdyn")
		RS_CMP(sr.T, "sr.T")
		RS_CMP(sr.status, "sr.status")
		RS_CMP_ARR(fr, 16, "fr")
		RS_CMP_ARR(xf, 16, "xf")
		RS_CMP(mac.l, "mac.l")
		RS_CMP(mac.h, "mac.h")
		RS_CMP(pr, "pr")
		RS_CMP(fpscr.full, "fpscr")
		RS_CMP(gbr, "gbr")
		RS_CMP(fpul, "fpul")
		RS_CMP_ARR(r_bank, 8, "r_bank")
		RS_CMP(vbr, "vbr")
		RS_CMP(ssr, "ssr")
		RS_CMP(spc, "spc")
		RS_CMP(interrupt_pend, "interrupt_pend")
		RS_CMP(cycle_counter, "cycle_counter")
		RS_CMP(pc, "pc")
#undef RS_CMP
#undef RS_CMP_ARR
		if (match && reg_pending != ref_pending) {
			match = false; diff_name = "exc_pending"; cv = reg_pending; rv = ref_pending;
		}
		if (match && reg_writes != ref_writes) {
			match = false; diff_name = "write_count"; cv = (u32)reg_writes; rv = (u32)ref_writes;
		}
		if (!match) {
			g_rs_diff++;
			if (g_rs_diff <= 20) {
				EM_ASM({ console.error('[REGION-DIFF] #' + $0 + ' entry=0x' + ($1>>>0).toString(16) +
					' idx=' + $2 + ' field=' + UTF8ToString($3) +
					($4 >= 0 ? ('[' + $4 + ']') : '') +
					' region=0x' + ($5>>>0).toString(16) + ' ref=0x' + ($6>>>0).toString(16) +
					' | cc region=' + $7 + ' ref=' + $8 + ' ref_stop=' + $9); },
					g_rs_diff, entry_pc, entry_idx, diff_name, diff_idx, cv, rv,
					cctx.cycle_counter, rctx.cycle_counter, ref_stop);
			}
		}
	}
	// Reference-first: discard the region run entirely
	for (size_t i = g_val_writes.size(); i-- > 0; ) {
		const ValWrite& w = g_val_writes[i];
		if (!w.is_ram) continue;
		u8* p = &mem_b[(w.addr & 0x1FFFFFFF) & RAM_MASK];
		if (w.size >= 4)      *(u32*)p = w.old_val;
		else if (w.size == 2) *(u16*)p = (u16)w.old_val;
		else                  *p = (u8)w.old_val;
	}
	memcpy(&ctx, g_cs_ref, sizeof(Sh4Context));
	for (const ValWrite& w : refw) {
		if (!w.is_ram) continue;
		u8* p = &mem_b[(w.addr & 0x1FFFFFFF) & RAM_MASK];
		if (w.size >= 4)      *(u32*)p = w.new_val;
		else if (w.size == 2) *(u16*)p = (u16)w.new_val;
		else                  *p = (u8)w.new_val;
	}
	g_ifb_exception_pending = ref_pending;

	if (g_rs_n <= 5 || (g_rs_n & 0xF) == 0)
		EM_ASM({ console.log('[REGION-SHADOW] n=' + $0 + ' diffs=' + $1 +
			' mmio=' + $2 + ' dev=' + $3 + ' smc=' + $4 +
			' infra=' + $5 + ' stale=' + $6 + ' traps=' + $7); },
			g_rs_n, g_rs_diff, g_rs_skip_mmio, g_rs_skip_dev, g_rs_skip_smc,
			g_rs_infra, g_rs_stale, g_rs_trap);
}
#endif  // FLY_REGION_SHADOW
#endif  // FLY_CHAINS_ANY

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
		// Resolve Emscripten lazy thunks to get raw WASM function references.
		//
		// With -O3 -flto, Emscripten exports are mangled (e.g. _wasm_mem_read32 -> "ck")
		// and Module._fn starts as a lazy thunk: a JS arrow function that on first call
		// replaces Module["_fn"] with wasmExports["ck"]. But since we bind imports at
		// WebAssembly.Instance creation time, we'd bind the thunk (a JS function),
		// causing WASM->JS->WASM trampolining on every import call.
		//
		// Fix: call each function once with safe dummy args to resolve the thunks.
		// After resolution, Module._fn IS wasmExports["xx"] (a raw WebAssembly.Function),
		// so V8 can do direct WASM->WASM cross-module calls.
		if (!Module._jitImportsResolved) {
			// Force thunk resolution — reads from addr 0 (BIOS ROM, harmless)
			Module._wasm_mem_read8(0);
			Module._wasm_mem_read16(0);
			Module._wasm_mem_read32(0);
			// Writes to addr 0 (BIOS ROM area, writes are ignored by hardware)
			Module._wasm_mem_write8(0, 0);
			Module._wasm_mem_write16(0, 0);
			Module._wasm_mem_write32(0, 0);
			// ifb: opcode 0 is a benign SH4 instruction, pc=0 is safe during init
			Module._wasm_exec_ifb(0, 0);
			// shil_fb: block_vaddr 0 will miss in blockByVaddr (increments miss counter, harmless)
			Module._wasm_exec_shil_fb(0, 0);
			Module._wasm_sq_pref(0);
			Module._wasm_div32u(0, 0, 0);
			Module._wasm_div32s(0, 0, 0);
			Module._wasm_div1(0, 0, 0);

			// Now Module._fn references are raw WebAssembly.Function objects
			var isWasm = typeof WebAssembly.Function !== 'undefined'
				? Module._wasm_mem_read32 instanceof WebAssembly.Function
				: typeof Module._wasm_mem_read32 === 'function';
			Module._jitImports = {
				memory: wasmMemory,
				read8:   Module._wasm_mem_read8,
				read16:  Module._wasm_mem_read16,
				read32:  Module._wasm_mem_read32,
				write8:  Module._wasm_mem_write8,
				write16: Module._wasm_mem_write16,
				write32: Module._wasm_mem_write32,
				ifb:     Module._wasm_exec_ifb,
				shil_fb: Module._wasm_exec_shil_fb,
				sq_pref: Module._wasm_sq_pref,
				div32u:  Module._wasm_div32u,
				div32s:  Module._wasm_div32s,
				div1:    Module._wasm_div1
			};
			if (!Module._flyQ) console.log('[rec_wasm] JIT imports resolved (WASM native: ' + isWasm + ')');
			Module._jitImportsResolved = true;
		}
		var instance = new WebAssembly.Instance(mod, { env: Module._jitImports });

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
		if (!Module._wasmBlockIdx) Module._wasmBlockIdx = {};
		Module._wasmBlockIdx[block_pc] = idx;
		Module._prof.compileMs += performance.now() - t0;
		return idx;  // table index (>0 on success, 0 reserved for NULL)
	} catch (e) {
		if (!Module._flyQ) console.error('[rec_wasm] compile fail PC=0x' + (block_pc >>> 0).toString(16) + ': ' + e.message);
		return 0;
	}
});

// ASYNC twin of wasm_compile_block (2026-07-16): copies the module bytes and
// hands them to the browser's off-thread compiler. On resolve, registers the
// function in table + cache and queues a ready-entry that drainCompileQueue()
// promotes into the dispatch table after a C-side staleness check. A
// generation counter guards against promises resolving across a cache reset.
EM_JS(void, wasm_compile_block_async, (const u8* bytesPtr, u32 len, u32 block_pc, u32 smc_hash, u32 smc_nw), {
	var wasmBytes = Module.HEAPU8.slice(bytesPtr, bytesPtr + len);  // C buffer dies at return
	if (!Module._flyReady) Module._flyReady = [];
	Module._flyGen = Module._flyGen | 0;
	var gen = Module._flyGen;
	if (!Module._jitImportsResolved) {
		// Same one-time thunk resolution as the sync path (see above).
		Module._wasm_mem_read8(0);
		Module._wasm_mem_read16(0);
		Module._wasm_mem_read32(0);
		Module._wasm_mem_write8(0, 0);
		Module._wasm_mem_write16(0, 0);
		Module._wasm_mem_write32(0, 0);
		Module._wasm_exec_ifb(0, 0);
		Module._wasm_exec_shil_fb(0, 0);
		Module._wasm_sq_pref(0);
		Module._wasm_div32u(0, 0, 0);
		Module._wasm_div32s(0, 0, 0);
		Module._wasm_div1(0, 0, 0);
		Module._jitImports = {
			memory: wasmMemory,
			read8:   Module._wasm_mem_read8,
			read16:  Module._wasm_mem_read16,
			read32:  Module._wasm_mem_read32,
			write8:  Module._wasm_mem_write8,
			write16: Module._wasm_mem_write16,
			write32: Module._wasm_mem_write32,
			ifb:     Module._wasm_exec_ifb,
			shil_fb: Module._wasm_exec_shil_fb,
			sq_pref: Module._wasm_sq_pref,
			div32u:  Module._wasm_div32u,
			div32s:  Module._wasm_div32s,
			div1:    Module._wasm_div1
		};
		Module._jitImportsResolved = true;
	}
	WebAssembly.instantiate(wasmBytes, { env: Module._jitImports }).then(function(result) {
		if (gen !== (Module._flyGen | 0)) return;  // cache reset while compiling
		var instance = result.instance;
		var table = wasmTable;
		if (!Module._jitTableBase) {
			Module._jitTableBase = table.length;
			Module._jitNextIdx = table.length;
			table.grow(4096);
		}
		var idx = Module._jitNextIdx++;
		if (idx >= table.length)
			table.grow(4096);
		table.set(idx, instance.exports.b);
		if (!Module._wasmBlockCache) Module._wasmBlockCache = {};
		Module._wasmBlockCache[block_pc] = instance.exports.b;
		if (!Module._wasmBlockIdx) Module._wasmBlockIdx = {};
		Module._wasmBlockIdx[block_pc] = idx;
		Module._flyReady.push({ pc: block_pc, idx: idx, hash: smc_hash, nw: smc_nw });
	}, function(e) {
		if (!Module._flyQ) console.error('[rec_wasm] async compile fail PC=0x' + (block_pc >>> 0).toString(16) + ': ' + (e && e.message));
	});
});

// SYNC batch compile: N block functions in one Module, exported b0..bN-1.
// Registers each in the block cache + indirect table; writes table indices to
// outIdxPtr (0 on per-function failure). Returns count on success, 0 on module
// failure. Legal on the main thread in all engines (Chrome's 4KB sync limit was
// removed in ~114); cost ≈ one module overhead + Liftoff codegen.
EM_JS(int, wasm_compile_block_batch, (const u8* bytesPtr, u32 len, const u32* pcsPtr, u32 count, u32* outIdxPtr), {
	try {
		if (!Module._jitImportsResolved) {
			Module._wasm_mem_read8(0);
			Module._wasm_mem_read16(0);
			Module._wasm_mem_read32(0);
			Module._wasm_mem_write8(0, 0);
			Module._wasm_mem_write16(0, 0);
			Module._wasm_mem_write32(0, 0);
			Module._wasm_exec_ifb(0, 0);
			Module._wasm_exec_shil_fb(0, 0);
			Module._wasm_sq_pref(0);
			Module._wasm_div32u(0, 0, 0);
			Module._wasm_div32s(0, 0, 0);
			Module._wasm_div1(0, 0, 0);
			Module._jitImports = {
				memory: wasmMemory,
				read8:   Module._wasm_mem_read8,
				read16:  Module._wasm_mem_read16,
				read32:  Module._wasm_mem_read32,
				write8:  Module._wasm_mem_write8,
				write16: Module._wasm_mem_write16,
				write32: Module._wasm_mem_write32,
				ifb:     Module._wasm_exec_ifb,
				shil_fb: Module._wasm_exec_shil_fb,
				sq_pref: Module._wasm_sq_pref,
				div32u:  Module._wasm_div32u,
				div32s:  Module._wasm_div32s,
				div1:    Module._wasm_div1
			};
			Module._jitImportsResolved = true;
		}
		var wasmBytes = Module.HEAPU8.slice(bytesPtr, bytesPtr + len);
		var mod = new WebAssembly.Module(wasmBytes);
		var instance = new WebAssembly.Instance(mod, { env: Module._jitImports });
		var table = wasmTable;
		if (!Module._jitTableBase) {
			Module._jitTableBase = table.length;
			Module._jitNextIdx = table.length;
			table.grow(4096);
		}
		if (!Module._wasmBlockCache) Module._wasmBlockCache = {};
		if (!Module._wasmBlockIdx) Module._wasmBlockIdx = {};
		for (var i = 0; i < count; i++) {
			var pc = Module.HEAPU32[(pcsPtr >> 2) + i];
			var fn = instance.exports['b' + i];
			if (!fn) { Module.HEAPU32[(outIdxPtr >> 2) + i] = 0; continue; }
			var idx = Module._jitNextIdx++;
			if (idx >= table.length)
				table.grow(4096);
			table.set(idx, fn);
			Module._wasmBlockCache[pc] = fn;
			Module._wasmBlockIdx[pc] = idx;
			Module.HEAPU32[(outIdxPtr >> 2) + i] = idx;
		}
		return count;
	} catch (e) {
		if (!Module._flyQ) console.error('[batch] compile fail n=' + count + ': ' + (e && e.message));
		return 0;
	}
});

EM_JS(int, wasm_execute_block, (u32 block_pc, u32 ctx_ptr, u32 ram_base), {
	try {
		if (!Module._prof) Module._prof = { execCount: 0, execMs: 0, execSamples: 0 };
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
			if (!Module._flyQ) console.error('[wasm-trap] PC=0x' + (block_pc >>> 0).toString(16) + ': ' + e.message);
		}
		return 1;
	}
});

EM_JS(int, wasm_has_block, (u32 block_pc), {
	return (Module._wasmBlockCache && Module._wasmBlockCache[block_pc]) ? 1 : 0;
});

EM_JS(void, wasm_clear_cache, (), {
	Module._wasmBlockCache = {};
	// Null out every allocated table slot — a populated slot pins its block's
	// WebAssembly.Instance (and Module) against GC forever. This was the
	// ROM-switch memory accumulation the user observed (2026-07-17): resets
	// rewound the INDEX but left thousands of instances referenced.
	if (Module._jitTableBase) {
		var table = wasmTable;
		var end = Math.min(Module._jitNextIdx | 0, table.length);
		for (var i = Module._jitTableBase | 0; i < end; i++)
			table.set(i, null);
	}
	// Reset table allocation — old entries become unreachable
	Module._jitTableBase = 0;
	Module._jitNextIdx = 0;
	// Invalidate in-flight async compiles + drop unpromoted ready entries
	Module._flyGen = (Module._flyGen | 0) + 1;
	Module._flyReady = [];
	Module._flyChainReady = [];
	Module._flyChainFail = [];
	Module._wasmBlockIdx = {};
});

EM_JS(void, wasm_remove_block, (u32 block_pc), {
	if (Module._wasmBlockCache) delete Module._wasmBlockCache[block_pc];
	if (Module._wasmBlockIdx) delete Module._wasmBlockIdx[block_pc];
});

// Table index of an already-compiled block (0 if none) — lets the dispatch
// miss path re-prime a collision-evicted slot without recompiling.
EM_JS(int, wasm_get_block_idx, (u32 block_pc), {
	return (Module._wasmBlockIdx && Module._wasmBlockIdx[block_pc]) | 0;
});

// Chain-module cache (FLY_CHAIN_SHADOW): multi-block modules keyed by head
// pc, kept SEPARATE from the single-block cache so production dispatch is
// untouched while the differential exercises chains.
EM_JS(int, wasm_compile_chain, (const u8* bytesPtr, u32 len, u32 head_pc), {
	try {
		var wasmBytes = Module.HEAPU8.slice(bytesPtr, bytesPtr + len);
		var mod = new WebAssembly.Module(wasmBytes);
		var instance = new WebAssembly.Instance(mod, { env: Module._jitImports });
		if (!Module._wasmChainCache) Module._wasmChainCache = {};
		Module._wasmChainCache[head_pc] = instance.exports.b;
		// Table registration for production call_indirect dispatch
		var table = wasmTable;
		if (!Module._jitTableBase) {
			Module._jitTableBase = table.length;
			Module._jitNextIdx = table.length;
			table.grow(4096);
		}
		var idx = Module._jitNextIdx++;
		if (idx >= table.length)
			table.grow(4096);
		table.set(idx, instance.exports.b);
		if (!Module._wasmChainIdx) Module._wasmChainIdx = {};
		Module._wasmChainIdx[head_pc] = idx;
		return idx;   // >0 on success
	} catch (e) {
		if (!Module._flyQ) console.error('[chain] compile fail head=0x' + (head_pc >>> 0).toString(16) + ': ' + e.message);
		return 0;
	}
});
EM_JS(void, wasm_remove_chain, (u32 head_pc), {
	if (Module._wasmChainCache) delete Module._wasmChainCache[head_pc];
	if (Module._wasmChainIdx) delete Module._wasmChainIdx[head_pc];
});

// ASYNC chain compile (2026-07-18): off-thread twin of wasm_compile_chain,
// mirroring wasm_compile_block_async. On resolve, registers the export in the
// indirect table and queues a ready-entry; drainChainQueue() promotes it after
// C-side fingerprint + freshness re-validation. _flyGen guards cache resets.
// Failures queue the head pc so C can reap the pending guard_cells.
EM_JS(void, wasm_compile_chain_async, (const u8* bytesPtr, u32 len, u32 head_pc), {
	var wasmBytes = Module.HEAPU8.slice(bytesPtr, bytesPtr + len);  // C buffer dies at return
	if (!Module._flyChainReady) Module._flyChainReady = [];
	if (!Module._flyChainFail) Module._flyChainFail = [];
	Module._flyGen = Module._flyGen | 0;
	var gen = Module._flyGen;
	WebAssembly.instantiate(wasmBytes, { env: Module._jitImports }).then(function(result) {
		if (gen !== (Module._flyGen | 0)) return;  // cache reset while compiling
		var instance = result.instance;
		var table = wasmTable;
		if (!Module._jitTableBase) {
			Module._jitTableBase = table.length;
			Module._jitNextIdx = table.length;
			table.grow(4096);
		}
		var idx = Module._jitNextIdx++;
		if (idx >= table.length)
			table.grow(4096);
		table.set(idx, instance.exports.b);
		if (!Module._wasmChainCache) Module._wasmChainCache = {};
		Module._wasmChainCache[head_pc] = instance.exports.b;
		if (!Module._wasmChainIdx) Module._wasmChainIdx = {};
		Module._wasmChainIdx[head_pc] = idx;
		Module._flyChainReady.push({ pc: head_pc, idx: idx });
	}, function(e) {
		if (!Module._flyQ) console.error('[chain] async compile fail head=0x' + (head_pc >>> 0).toString(16) + ': ' + (e && e.message));
		Module._flyChainFail.push(head_pc);
	});
});
EM_JS(int, wasm_execute_chain, (u32 head_pc, u32 ctx_ptr, u32 ram_base), {
	try {
		Module._wasmChainCache[head_pc](ctx_ptr, ram_base);
		return 0;
	} catch (e) {
		if (!Module._flyQ) console.error('[chain-trap] head=0x' + (head_pc >>> 0).toString(16) + ': ' + (e && e.message));
		return 1;
	}
});
EM_JS(int, wasm_has_chain, (u32 head_pc), {
	return (Module._wasmChainCache && Module._wasmChainCache[head_pc]) ? 1 : 0;
});
EM_JS(void, wasm_clear_chains, (), {
	Module._wasmChainCache = {};
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
// PC trace ring — the last 256 block PCs dispatched. Dumped on FREEZE_DETECTED.
// Hot-path cost is one store + one mask + one increment. Not in the fly ring
// because that would 10-100x the event volume.
#ifndef JIT_PROD_BUILD
#define PC_TRACE_SIZE 256
#define PC_TRACE_MASK (PC_TRACE_SIZE - 1)
static u32 g_pc_trace[PC_TRACE_SIZE];
static u32 g_pc_trace_spc[PC_TRACE_SIZE];  // ctx.spc at each dispatch — tracks SPC trajectory into handler
static u32 g_pc_trace_sr[PC_TRACE_SIZE];   // sr.status low 32 bits — lets us flag BL/RB transitions
static u32 g_pc_trace_head = 0;  // monotonic; last written = (head-1) & MASK

// First-dispatch-to-new-PC ring. Each entry captures the first time a given
// PC was dispatched, along with the PC that came right before it. This lets
// us find the original caller of an "illegal" jump target — the PC trace
// ring shows the fault-loop, but we want the one-time event that started it.
// Uses a hash set keyed on PC to dedupe; capacity 8192.
#define NEWPC_HASH_SIZE 8192
#define NEWPC_HASH_MASK (NEWPC_HASH_SIZE - 1)
static u32 g_newpc_seen[NEWPC_HASH_SIZE];  // hash slot = PC, 0 = empty
#define NEWPC_RING_SIZE 256
#define NEWPC_RING_MASK (NEWPC_RING_SIZE - 1)
static u32 g_newpc_ring_pc[NEWPC_RING_SIZE];
static u32 g_newpc_ring_prev[NEWPC_RING_SIZE];  // caller PC
static u32 g_newpc_ring_head = 0;

// Snapshot of the PC trace taken at the moment of the FIRST illegal-instruction
// exception in the session. The live PC trace is a ring that quickly gets
// overwritten by the handler loop; this frozen snapshot preserves the state
// right before the fault cascade started.
static u32 g_first_exc_pc_trace[PC_TRACE_SIZE];
static u32 g_first_exc_pc_trace_spc[PC_TRACE_SIZE];
static u32 g_first_exc_pc_trace_sr[PC_TRACE_SIZE];
static u32 g_first_exc_pc_trace_head = 0;  // 0 = not yet captured
static u32 g_first_exc_epc = 0;
static u32 g_first_exc_r14 = 0;  // r[14] value — common JSR target source
static u32 g_first_exc_evn = 0;
// Register snapshot at the moment of the first illegal fault. Pre-handler,
// so r0-r15 still hold values that the faulting block set up. One of them
// is the bad jump target.
static u32 g_first_exc_r[16];
static u32 g_first_exc_r_bank[8];   // opposite bank regs at snapshot
static u32 g_first_exc_pr = 0;
static u32 g_first_exc_gbr = 0;
static u32 g_first_exc_mach = 0;
static u32 g_first_exc_macl = 0;
static u32 g_first_exc_sr = 0;
static u32 g_first_exc_spc = 0;
static u32 g_first_exc_ssr = 0;
static u32 g_first_exc_sgr = 0;
static u32 g_first_exc_vbr = 0;
#endif

#if FLY_INLINE_DISPATCH
// Faithful replica of the mainloop's miss handler (see the
// g_dispatch_result == 1 branch there), callable from inside c_dispatch_loop.
// Same operations in the same order — the ONLY change Variant A makes is
// WHERE this runs (inline vs after a return to the mainloop).
static void inline_handle_miss(u32 miss_pc, u32 ctx_ptr, u32 ram_ptr) {
	Sh4Context* sh4ctx = &Sh4cntx;

	// Miss-path SMC check (identical to mainloop's)
	{
		auto smc_it = g_block_smc.find(miss_pc);
		if (smc_it != g_block_smc.end() && smc_it->second.sz > 0) {
			u32 phys = miss_pc & 0x1FFFFFFF;
			if ((phys >> 26) == 3) {
				u32 live = hashRamBlock(miss_pc, smc_it->second.sz);
				if (live != smc_it->second.hash) {
					auto be = blockByVaddr.find(miss_pc);
					if (be != blockByVaddr.end()) blockByVaddr.erase(be);
					wasm_remove_block(miss_pc);
					#if FLY_CHAINS_ANY
					fly_chain_invalidate(miss_pc);
					#endif
					g_block_smc.erase(miss_pc);
					u32 k = (miss_pc >> 1) & JIT_TABLE_MASK;
					if (jit_dispatch_pc[k] == miss_pc) {
						jit_dispatch_table[k] = 0; jit_dispatch_pc[k] = 0;
						jit_dispatch_hash[k] = 0; jit_dispatch_sz[k] = 0;
					}
				}
			}
		}
	}

	auto it = blockByVaddr.find(miss_pc);
	if (it != blockByVaddr.end()) {
		// Compiled but not in table (collision/eviction). ★ FPSCR-ORDER FIX
		// (2026-07-16): recompile BEFORE executing — see mainloop comments.
		// The old execute-then-recompile order sampled post-execution fpscr
		// into the replacement block's fpu_cfg, miscompiling fmov sizes.
		rdv_FailedToFindBlock(miss_pc);  // ctx.pc = miss_pc — no clobber pre-exec
		it = blockByVaddr.find(miss_pc);
		RuntimeBlockInfo* block = it != blockByVaddr.end() ? it->second : nullptr;
		sh4ctx->pc = miss_pc;
		g_ifb_exception_pending = false;
		if (wasm_has_block(miss_pc)) {
			wasm_execute_block(miss_pc, ctx_ptr, ram_ptr);
		} else if (block) {
			sh4ctx->cycle_counter -= block->guest_cycles;
			for (u32 i = 0; i < block->oplist.size(); i++)
				wasm_exec_shil_fb(block->vaddr, i);
			applyBlockExitCpp(block);
		} else {
			sh4ctx->pc = miss_pc + 2;
			u16 rawOp = IReadMem16(miss_pc);
			OpPtr[rawOp](sh4ctx, rawOp);
			sh4ctx->cycle_counter -= 1;
		}
		if (g_ifb_exception_pending) {
			Do_Exception(g_ifb_exception_epc, g_ifb_exception_expEvn);
			g_ifb_exception_pending = false;
		}
		g_wasm_block_count++;
	} else {
		// Cold: compile then execute (no budget — compile-always).
		rdv_FailedToFindBlock(miss_pc);
		it = blockByVaddr.find(miss_pc);
		if (it != blockByVaddr.end()) {
			RuntimeBlockInfo* block = it->second;
			sh4ctx->pc = miss_pc;
			g_ifb_exception_pending = false;
			if (wasm_has_block(miss_pc)) {
				wasm_execute_block(miss_pc, ctx_ptr, ram_ptr);
			} else {
				sh4ctx->cycle_counter -= block->guest_cycles;
				for (u32 i = 0; i < block->oplist.size(); i++)
					wasm_exec_shil_fb(block->vaddr, i);
				applyBlockExitCpp(block);
			}
			if (g_ifb_exception_pending) {
				Do_Exception(g_ifb_exception_epc, g_ifb_exception_expEvn);
				g_ifb_exception_pending = false;
			}
			g_wasm_block_count++;
		} else {
			// Compilation failed — interpret one instruction (throws
			// propagate to the mainloop catch exactly as before).
			sh4ctx->pc = miss_pc + 2;
			u16 rawOp = IReadMem16(miss_pc);
			if (sh4ctx->sr.FD == 1 && OpDesc[rawOp]->IsFloatingPoint())
				throw SH4ThrownException(miss_pc, Sh4Ex_FpuDisabled);
			OpPtr[rawOp](sh4ctx, rawOp);
			sh4ctx->cycle_counter -= 1;
		}
	}
}
#endif  // FLY_INLINE_DISPATCH

static int c_dispatch_loop(u32 ctx_ptr, u32 ram_base) {
	typedef void (*block_fn_t)(u32, u32);
	Sh4Context& ctx = Sh4cntx;
	int blocks_run = 0;

	while (ctx.cycle_counter > 0) {
		u32 pc = ctx.pc;
		dispTrace(pc);  // JIT dispatch block-entry trace
#if !defined(JIT_PROD_BUILD) && !FLY_NO_LOOP_PROBES
		// Watch for pr (link register) being corrupted from a valid address to 0
		// — the block that does it is the bug (leads to RTS-to-null → garbage).
		{
			static u32 g_last_pr = 0xFFFFFFFFu;
			if (ctx.pr == 0 && g_last_pr != 0 && g_last_pr != 0xFFFFFFFFu) {
				static u32 prz = 0;
				if (prz < 30) {
					prz++;
					u32 head = g_pc_trace_head;
					u32 pp = head > 0 ? g_pc_trace[(head - 1) & PC_TRACE_MASK] : 0;
					EM_ASM({ console.error('[PR-ZERO] #' + $0 + ' pr 0x' + ($1>>>0).toString(16) +
						'->0 set by block 0x' + ($2>>>0).toString(16) + ' now at pc=0x' +
						($3>>>0).toString(16) + ' r15=0x' + ($4>>>0).toString(16)); },
						prz, g_last_pr, pp, pc, ctx.r[15]);
				}
			}
			g_last_pr = ctx.pr;
		}
		// Catch the JIT dispatching a suspiciously LOW pc (exception-vector /
		// garbage region, < 0x00010000) — the Skies crash signature. Dump the
		// exception-related ctx + recent PC trace so we can tell whether it's a
		// mis-delivered exception (VBR/SPC set) or a corrupted jump, and what ran
		// just before.
		if (pc < 0x00010000u) {
			static u32 lowpc_logged = 0;
			if (lowpc_logged < 4) {
				lowpc_logged++;
				EM_ASM({ console.error('[LOWPC] #' + $0 + ' dispatched pc=0x' + ($1>>>0).toString(16) +
					' | vbr=0x' + ($2>>>0).toString(16) + ' spc=0x' + ($3>>>0).toString(16) +
					' ssr=0x' + ($4>>>0).toString(16) + ' pr=0x' + ($5>>>0).toString(16) +
					' sr=0x' + ($6>>>0).toString(16)); },
					lowpc_logged, pc, ctx.vbr, ctx.spc, ctx.ssr, ctx.pr, ctx.sr.status);
				u32 head = g_pc_trace_head;
				u32 cnt = head < 16 ? head : 16;
				for (u32 t = 0; t < cnt; t++) {
					u32 idx = (head - cnt + t) & PC_TRACE_MASK;
					EM_ASM({ console.error('[LOWPC]   prev[' + $0 + ']=0x' + ($1>>>0).toString(16) +
						' sr=0x' + ($2>>>0).toString(16)); },
						(int)t - (int)cnt, g_pc_trace[idx], g_pc_trace_sr[idx]);
				}
			}
		}
#endif
		u32 key = (pc >> 1) & JIT_TABLE_MASK;
		u32 table_idx = jit_dispatch_table[key];

		if (table_idx == 0 || jit_dispatch_pc[key] != pc) {
#if FLY_INLINE_DISPATCH
			// Variant A: handle the miss right here — no mainloop round-trip.
			inline_handle_miss(pc, ctx_ptr, ram_base);
			blocks_run++;
			continue;
#else
			g_dispatch_result = 1;  // miss or collision
			g_dispatch_miss_pc = pc;
			return blocks_run;
#endif
		}

		// SMC check: full-block hash (covers all sh4_code_size bytes, not
		// just PC+0/1). Layer 1b test smc_inner_byte confirmed the first-
		// opcode-only check missed inner-byte rewrites. Only RAM blocks
		// (area 3: 0x0C/0x8C/0xAC) — ROM/BIOS blocks have sz=0 → skip.
		{
			u32 phys = pc & 0x1FFFFFFF;
			u16 sz = jit_dispatch_sz[key];
			if ((phys >> 26) == 3 && sz > 0) {
				// Page-generation gate: hash only if a covered page changed
				// since last verify, or on the 1/64 insurance tick.
#ifndef JIT_PROD_BUILD
				// Page-locality histogram + inter-page edges (region scoping)
				{ extern void fly_track_edge(u32 page); fly_track_edge((phys & RAM_MASK) >> 12); }
#elif FLY_REGIONS_LIVE
				// PROD + regions: discovery needs the page-exec histogram
				// (one array bump; the edge/locality extras stay dev-only).
				// Without this, prod discovery is blind (2026-07-21 lesson).
				g_fly_page_execs[(phys & RAM_MASK) >> 12]++;
#endif
				u32 pg0 = (phys & RAM_MASK) >> 12;
				u32 pg1 = ((phys + (u32)sz * 2 - 1) & RAM_MASK) >> 12;
				u32 pgen = g_fly_page_gen[pg0] + (pg1 != pg0 ? g_fly_page_gen[pg1] : 0);
#if FLY_SMC_INSURANCE_TICK
				bool fly_forced = (++jit_dispatch_tick[key] == 0) || ((jit_dispatch_tick[key] & 63) == 0);
#else
				// Tick retired: gen coverage is complete (see flag comment).
				const bool fly_forced = false;
#endif
				if (pgen == jit_dispatch_pgen[key] && !fly_forced)
					goto fly_smc_ok;
				jit_dispatch_pgen[key] = pgen;
				{
#ifndef JIT_PROD_BUILD
				{ extern u32 g_fly_hash_words; g_fly_hash_words += sz; }
#endif
				u32 currentHash = hashRamBlock(pc, sz);
				if (currentHash != jit_dispatch_hash[key]) {
#if !defined(JIT_PROD_BUILD) && !FLY_NO_LOOP_PROBES
					static u32 smc_log_count = 0;
					if (smc_log_count < 50) {
						smc_log_count++;
						EM_ASM({ console.log('[JIT-SMC] pc=0x' + ($0>>>0).toString(16) +
							' old_hash=0x' + ($1>>>0).toString(16) +
							' new_hash=0x' + ($2>>>0).toString(16) +
							' sz=' + $3); },
							pc, jit_dispatch_hash[key], currentHash, sz);
					}
#endif
					jit_dispatch_table[key] = 0;
					jit_dispatch_pc[key] = 0;
					jit_dispatch_hash[key] = 0;
					jit_dispatch_sz[key] = 0;
					auto blk_it = blockByVaddr.find(pc);
					if (blk_it != blockByVaddr.end())
						blockByVaddr.erase(blk_it);
					wasm_remove_block(pc);
					#if FLY_CHAINS_ANY
					fly_chain_invalidate(pc);
					#endif
#if FLY_INLINE_DISPATCH
					// Variant A: recompile+execute inline — no mainloop round-trip.
					inline_handle_miss(pc, ctx_ptr, ram_base);
					blocks_run++;
					continue;
#else
					g_dispatch_result = 1;  // miss — recompile
					g_dispatch_miss_pc = pc;
					return blocks_run;
#endif
				}
				}
			}
			fly_smc_ok:;
		}

		// Region entry plumbing: the region module reads its entry block's
		// dense index from this cell (0 for non-region slots — harmless).
		g_region_entry_idx = jit_dispatch_arg[key];
		// Cast table index to function pointer — Emscripten compiles
		// this to call_indirect, staying entirely within WASM.
		block_fn_t fn = (block_fn_t)(uintptr_t)table_idx;
#if !defined(JIT_PROD_BUILD) && !FLY_NO_LOOP_PROBES
		u32 prev_pc = (g_pc_trace_head > 0)
			? g_pc_trace[(g_pc_trace_head - 1) & PC_TRACE_MASK]
			: 0;
		g_pc_trace[g_pc_trace_head & PC_TRACE_MASK] = pc;
		g_pc_trace_spc[g_pc_trace_head & PC_TRACE_MASK] = Sh4cntx.spc;
		g_pc_trace_sr[g_pc_trace_head & PC_TRACE_MASK] = Sh4cntx.sr.status;
		g_pc_trace_head++;

		// First-dispatch check: hash-probe and record if this PC hasn't been
		// seen. Hash uses 8K slots with linear probing (up to 4 slots).
		u32 hash = ((pc * 0x9E3779B1u) >> 19) & NEWPC_HASH_MASK;
		bool seen = false;
		for (u32 probe = 0; probe < 4; probe++) {
			u32 slot = (hash + probe) & NEWPC_HASH_MASK;
			if (g_newpc_seen[slot] == pc) { seen = true; break; }
			if (g_newpc_seen[slot] == 0) {
				g_newpc_seen[slot] = pc;
				u32 idx = g_newpc_ring_head & NEWPC_RING_MASK;
				g_newpc_ring_pc[idx] = pc;
				g_newpc_ring_prev[idx] = prev_pc;
				g_newpc_ring_head++;
				seen = true;
				break;
			}
		}
		(void)seen;
#endif
#if !defined(JIT_PROD_BUILD) && !FLY_NO_LOOP_PROBES
		// Per-dispatch diagnostic for suspected writer block 0x8c0084f0.
		// Captures r[15] and mem[r[15]+8] BEFORE and AFTER fn() runs, plus
		// the specific memory slot that MEM-WATCH cares about.
		// Log only when the block produces a "suspicious" write event.
		u32 dbg_pre_r15 = 0, dbg_pre_stack_val = 0, dbg_pre_target_val = 0;
		bool dbg_watched = (pc == 0x8c0084f0);
		if (dbg_watched) {
			dbg_pre_r15 = ctx.r[15];
			u32 stack_addr = (ctx.r[15] + 8) & 0x1FFFFFFF;
			if ((stack_addr >> 26) == 3)
				dbg_pre_stack_val = *(volatile u32*)&mem_b[stack_addr & RAM_MASK];
			dbg_pre_target_val = *(volatile u32*)&mem_b[0x0C0D9324 & RAM_MASK];
		}
#endif

#if FLY_DISPATCH_VIA_CACHE
		// ISOLATION TEST (2026-06-11): execute the block via _wasmBlockCache[pc]
		// (by exact PC, same as the clean mode-8 hybrid) instead of call_indirect
		// through jit_dispatch_table[key]. If Skies boots with this but not with
		// fn() below, the table_idx/call_indirect HIT path is the bug.
		(void)fn;
		wasm_execute_block(pc, ctx_ptr, ram_base);
#else
		fn(ctx_ptr, ram_base);
#endif
		blocks_run++;

#if FLY_REGIONS_LIVE
		// Step-3 IC fill: a region posted an IC miss — resolve the exit pc
		// to a member idx and fill the site's {pc, idx} pair (written
		// together, single-threaded → always consistent). One load+cmp per
		// dispatch when no miss is pending; misses are transient (each
		// monomorphic site fills once per target change).
		if (g_region_dyn_miss != 0xFFFFFFFFu) {
			u32 enc = g_region_dyn_miss;
			g_region_dyn_miss = 0xFFFFFFFFu;
			u32 rs = enc >> 16, si = enc & 0xFFFFu;
			if (rs < (u32)g_regions.size()) {
				RegionInfo& rr = g_regions[rs];
				if (rr.table_idx && rr.ic_cells && si < (u32)rr.members.size()) {
					u32 tpc = Sh4cntx.pc;
					auto mit = g_region_member_of.find(tpc);
					if (mit != g_region_member_of.end() && mit->second - 1 == rs) {
						for (u32 ti = 0; ti < (u32)rr.members.size(); ti++) {
							if (rr.members[ti].pc == tpc) {
								rr.ic_cells[si * 2 + 1] = ti;
								rr.ic_cells[si * 2] = tpc;
								break;
							}
						}
					}
				}
			}
		}
#endif

		// ★ DEFERRED-EXCEPTION DELIVERY (2026-06-11): when a fallback op inside a
		// WASM block raises an SH4 exception it sets g_ifb_exception_pending, aborts
		// the rest of the block, and RETURNS — leaving the caller to actually run
		// Do_Exception (jump to the vector). cpp_execute_block and the miss handler
		// do this; c_dispatch_loop did NOT, so production silently DROPPED these
		// exceptions: the handler never ran, pc never reached the vector, and the
		// stuck pending flag poisoned later fallback ops (`if (pending) return;`).
		// The clean (mode-8) dispatch delivers them → no glitches; this closes the
		// gap. Matches the contract used everywhere else (reset-before / deliver-after).
		if (g_ifb_exception_pending) {
#if !defined(JIT_PROD_BUILD) && !FLY_NO_LOOP_PROBES
			static u32 defexc = 0;
			if (defexc < 20 || (defexc & 0x3FF) == 0)
				EM_ASM({ console.log('[DEF-EXC] #' + $0 + ' delivered at block 0x' + ($1>>>0).toString(16) +
					' evn=0x' + ($2>>>0).toString(16) + ' (was dropped by c_dispatch_loop pre-fix)'); },
					defexc, pc, g_ifb_exception_expEvn);
			defexc++;
#endif
			Do_Exception(g_ifb_exception_epc, g_ifb_exception_expEvn);
			g_ifb_exception_pending = false;
		}

#if !defined(JIT_PROD_BUILD) && !FLY_NO_LOOP_PROBES
		if (dbg_watched) {
			u32 post_r15 = ctx.r[15];
			u32 stack_addr = (ctx.r[15] + 8) & 0x1FFFFFFF;
			u32 post_stack_val = 0;
			if ((stack_addr >> 26) == 3)
				post_stack_val = *(volatile u32*)&mem_b[stack_addr & RAM_MASK];
			// volatile to defeat any compiler CSE around the indirect fn() call
			u32 post_target_val = *(volatile u32*)&mem_b[0x0C0D9324 & RAM_MASK];
			// Log ANY dispatch of this block where post_target == the suspicious
			// value (0x8c1c47c8) OR the target changed. Catches both "this block
			// wrote it" and "arrived here with bad memory already in place".
			bool suspicious_val = (post_target_val == 0x8c1c47c8);
			if (post_target_val != dbg_pre_target_val || suspicious_val) {
				static u32 dbg_log_count = 0;
				if (dbg_log_count < 20) {
					dbg_log_count++;
					EM_ASM({
						if (!window._flyLog) window._flyLog = [];
						window._flyLog.push('[BLOCK-DBG] 0x8c0084f0 run #' + $0
							+ ' pre_r15=0x' + ($1>>>0).toString(16)
							+ ' post_r15=0x' + ($2>>>0).toString(16)
							+ ' pre_stack_val=0x' + ($3>>>0).toString(16)
							+ ' post_stack_val=0x' + ($4>>>0).toString(16)
							+ ' target[0x8c0d9324]: 0x' + ($5>>>0).toString(16)
							+ ' -> 0x' + ($6>>>0).toString(16));
					}, dbg_log_count, dbg_pre_r15, post_r15,
					   dbg_pre_stack_val, post_stack_val,
					   dbg_pre_target_val, post_target_val);
				}
			}
		}

		// MEM-WATCH probe: DOA2 runs show memory at 0x8c0d9324 silently
		// transitions from valid SH4 code to a pointer value (0x8c1c47c8).
		// Watch for writes to these addresses — log the block PC that
		// caused each change.
		{
			static u32 g_memwatch_last[3] = {0, 0, 0};
			static u32 g_memwatch_log_count = 0;
			static bool g_memwatch_init = false;
			static const u32 k_watch_addrs[3] = {
				0x8c0d9324,  // run 2/3/4 culprit
				0x8c1b89b2 & ~3,  // run 1 (align to u32)
				0x8c1c47c8 & ~3,  // run 3 pointer location
			};
			if (!g_memwatch_init) {
				for (int i = 0; i < 3; i++) {
					u32 phys = k_watch_addrs[i] & 0x1FFFFFFF;
					if ((phys >> 26) == 3)
						g_memwatch_last[i] = *(u32*)&mem_b[phys & RAM_MASK];
				}
				g_memwatch_init = true;
			}
			for (int i = 0; i < 3 && g_memwatch_log_count < 50; i++) {
				u32 phys = k_watch_addrs[i] & 0x1FFFFFFF;
				if ((phys >> 26) != 3) continue;
				u32 cur = *(u32*)&mem_b[phys & RAM_MASK];
				if (cur != g_memwatch_last[i]) {
					g_memwatch_log_count++;
					EM_ASM({
						if (!window._flyLog) window._flyLog = [];
						window._flyLog.push('[MEM-WATCH] #' + $0
							+ ' block_pc=0x' + ($1>>>0).toString(16)
							+ ' addr=0x' + ($2>>>0).toString(16)
							+ ' old=0x' + ($3>>>0).toString(16)
							+ ' new=0x' + ($4>>>0).toString(16));
					}, g_memwatch_log_count, pc, k_watch_addrs[i],
					   g_memwatch_last[i], cur);
					// Also dump last 8 PC trace entries — real writer is
					// among these (current block may just be next dispatched).
					u32 head = g_pc_trace_head;
					u32 count = head < 8 ? head : 8;
					for (u32 t = 0; t < count; t++) {
						u32 idx = (head - count + t) & PC_TRACE_MASK;
						u32 trace_pc = g_pc_trace[idx];
						EM_ASM({
							window._flyLog.push('[MEM-WATCH]   recent[' + $0 + ']=0x'
								+ ($1>>>0).toString(16));
						}, (int)t - (int)count, trace_pc);
					}
					g_memwatch_last[i] = cur;
				}
			}
		}

		// PR-corruption probe: DOA2 freezes show pr=fault_PC (corrupted link
		// register → RTS to bad address). Watch for PR transitioning INTO
		// any of the known data-region windows observed in 3 freeze runs.
		// The block that just ran is where PR got corrupted.
		{
			static u32 g_probe_last_pr = 0;
			static u32 g_probe_log_count = 0;
			u32 v = ctx.pr;
			if (v != g_probe_last_pr && g_probe_log_count < 200) {
				auto in_window = [](u32 a) {
					return (a >= 0x8c1c4000 && a < 0x8c1c5000) ||  // run 3 pool
					       (a >= 0x8c1b8000 && a < 0x8c1bc000) ||  // run 1 pool
					       (a >= 0x8c0d9000 && a < 0x8c0d9400);    // run 2/3 pool
				};
				if (in_window(v) && !in_window(g_probe_last_pr)) {
					g_probe_log_count++;
					u32 phys = v & 0x1FFFFFFF;
					u32 w0 = 0, w1 = 0, w2 = 0, w3 = 0;
					if ((phys >> 26) == 3) {
						u32 off = phys & RAM_MASK;
						w0 = *(u16*)&mem_b[off];
						w1 = *(u16*)&mem_b[(off+2) & RAM_MASK];
						w2 = *(u16*)&mem_b[(off+4) & RAM_MASK];
						w3 = *(u16*)&mem_b[(off+6) & RAM_MASK];
					}
					EM_ASM({
						if (!window._flyLog) window._flyLog = [];
						window._flyLog.push('[PR-PROBE] #' + $0
							+ ' block_pc=0x' + ($1>>>0).toString(16)
							+ ' prev_pr=0x' + ($2>>>0).toString(16)
							+ ' new_pr=0x' + ($3>>>0).toString(16)
							+ ' bytes_at_new_pr=[' + ($4>>>0).toString(16) + ','
							+ ($5>>>0).toString(16) + ','
							+ ($6>>>0).toString(16) + ','
							+ ($7>>>0).toString(16) + ']');
					}, g_probe_log_count, pc, g_probe_last_pr, v, w0, w1, w2, w3);
				}
				g_probe_last_pr = v;
			}
		}
#endif

		if (ctx.interrupt_pend) {
#if FLY_INLINE_DISPATCH
			// Variant A: deliver inline (exactly like clean_dispatch_loop)
			// instead of returning result 3 to the mainloop.
			UpdateINTC();
#else
			g_dispatch_result = 3;  // interrupt
			return blocks_run;
#endif
		}
	}

	g_dispatch_result = 0;  // timeslice complete
	return blocks_run;
}

// ============================================================
// CLEAN dispatch loop — live A/B toggle (2026-06-11)
// ============================================================
// The mode-8 (HYBRID) dispatch, made callable in production (EXECUTOR_MODE 6).
// Runs an ENTIRE timeslice: every block via wasm_execute_block(pc) [by exact PC
// through _wasmBlockCache — NO hash table / call_indirect], with a per-block SMC
// check and per-block deferred-exception delivery, handling misses + interrupts
// INLINE (never returns to the mainloop mid-timeslice). This is the path the user
// reported glitch-free. Toggle live via `Module._useCleanDispatch = 1` in the
// browser console — lets us A/B the two dispatch paths in the same scene with no
// rebuild. If glitches track the toggle, dispatch is the cause.
static int clean_dispatch_loop(u32 ctx_ptr, u32 ram_base) {
	Sh4Context& ctx = Sh4cntx;
	int blocks_run = 0;
	while (ctx.cycle_counter > 0) {
		// Stuck-loop bail: a swallowed trap leaves pc/cycle_counter unchanged and
		// would spin forever (a timeslice is ~50-200 blocks; 2M = wedged). Bail
		// instead of hanging the tab — visible as a huge blocks_run, not a freeze.
		if (blocks_run > 2000000) {
			EM_ASM({ console.error('[clean-dispatch] STUCK at pc=0x' + ($0>>>0).toString(16) +
				' after ' + $1 + ' blocks — bailing out of timeslice'); }, ctx.pc, blocks_run);
			break;
		}
		u32 pc = ctx.pc;
		dispTrace(pc);
#if FLY_VARIANT_B
		// VARIANT B: lookup + SMC fingerprint via the HASH-TABLE SLOTS
		// (production's oracle) instead of the per-PC maps. Execution stays
		// clean-style (by PC). This is the single flipped variable.
		bool vb_have;
		{
			u32 key = (pc >> 1) & JIT_TABLE_MASK;
			vb_have = (jit_dispatch_table[key] != 0 && jit_dispatch_pc[key] == pc);
			if (vb_have) {
				u32 phys = pc & 0x1FFFFFFF;
				u16 sz = jit_dispatch_sz[key];
				if ((phys >> 26) == 3 && sz > 0) {
					if (hashRamBlock(pc, sz) != jit_dispatch_hash[key]) {
						jit_dispatch_table[key] = 0; jit_dispatch_pc[key] = 0;
						jit_dispatch_hash[key] = 0; jit_dispatch_sz[key] = 0;
						auto be = blockByVaddr.find(pc);
						if (be != blockByVaddr.end()) blockByVaddr.erase(be);
						wasm_remove_block(pc);
						#if FLY_CHAINS_ANY
						fly_chain_invalidate(pc);
						#endif
						g_block_smc.erase(pc);
						vb_have = false;
					}
				}
			}
			if (!vb_have) {
				rdv_FailedToFindBlock(pc);   // compiles + primes the slot
				vb_have = (jit_dispatch_table[key] != 0 && jit_dispatch_pc[key] == pc);
			}
		}
		if (!vb_have) {
#else
		// Per-block SMC: evict + recompile if the block's RAM changed since compile.
		{
			auto smc_it = g_block_smc.find(pc);
			if (smc_it != g_block_smc.end() && smc_it->second.sz > 0) {
				u32 phys = pc & 0x1FFFFFFF;
				if ((phys >> 26) == 3) {
					u32 live = hashRamBlock(pc, smc_it->second.sz);
					if (live != smc_it->second.hash) {
						auto be = blockByVaddr.find(pc);
						if (be != blockByVaddr.end()) blockByVaddr.erase(be);
						wasm_remove_block(pc);
						#if FLY_CHAINS_ANY
						fly_chain_invalidate(pc);
						#endif
						g_block_smc.erase(pc);
						u32 k = (pc >> 1) & JIT_TABLE_MASK;
						if (jit_dispatch_pc[k] == pc) {
							jit_dispatch_table[k] = 0; jit_dispatch_pc[k] = 0;
							jit_dispatch_hash[k] = 0; jit_dispatch_sz[k] = 0;
						}
					}
				}
			}
		}
		auto it = blockByVaddr.find(pc);
		if (it == blockByVaddr.end()) {
			rdv_FailedToFindBlock(pc);   // sets Sh4cntx.pc = pc (== current pc, no clobber)
			it = blockByVaddr.find(pc);
		}
		if (it == blockByVaddr.end()) {
#endif
			// Couldn't find/compile — interpret one instruction.
			ctx.pc = pc + 2;
			u16 rawOp = IReadMem16(pc);
			if (ctx.sr.FD == 1 && OpDesc[rawOp]->IsFloatingPoint())
				throw SH4ThrownException(pc, Sh4Ex_FpuDisabled);
			OpPtr[rawOp](&ctx, rawOp);
			ctx.cycle_counter -= 1;
		} else {
			g_ifb_exception_pending = false;
			if (wasm_has_block(pc)) {
				wasm_execute_block(pc, ctx_ptr, ram_base);   // block self-charges guest_cycles
			} else {
				ctx.pc = pc + 2;
				u16 rawOp = IReadMem16(pc);
				OpPtr[rawOp](&ctx, rawOp);
				ctx.cycle_counter -= 1;
			}
			if (g_ifb_exception_pending) {
				Do_Exception(g_ifb_exception_epc, g_ifb_exception_expEvn);
				g_ifb_exception_pending = false;
			}
			blocks_run++;
		}
		if (ctx.interrupt_pend) {
#if LOCKSTEP_DIFF_BUILD
			extern u32 g_ls_intc_calls;  // defined below — in-slice interrupt tripwire
			g_ls_intc_calls++;
#endif
			UpdateINTC();
		}
	}
	g_dispatch_result = 0;
	return blocks_run;
}

#if LOCKSTEP_DIFF_BUILD
// ============================================================
// LOCKSTEP timeslice differential — production vs clean dispatch
// ============================================================
// The objective instrument for the glitch hunt: every timeslice is executed
// TWICE from the same Sh4Context snapshot — first through the PRODUCTION path
// (c_dispatch_loop + mainloop miss handler, exactly as shipped), then, after
// undoing its RAM writes and restoring the snapshot, through the glitch-free
// clean_dispatch_loop. Registers and the complete guest write stream (RAM,
// VRAM, MMIO — addr/size/value/order) are then binary-compared. The first
// divergence is dumped and execution freezes (halt latch), exactly like the
// block validator that caught the double-execution bug.
//
// Comparability policy (mirrors the validator's MMIO policy):
//  - Slices where either run delivered an interrupt (UpdateINTC) are compared
//    but never halt: global INTC state can't be restored for the re-run.
//  - Slices where either run READ MMIO (volatile timers/status regs) never
//    halt: the two reads legitimately observe different values.
//  - MMIO/VRAM writes can't be undone; they ARE compared as stream entries.
//    Double-applied identical writes are harmless for CPU-state comparison
//    (rendering in this diagnostic build is expected to be garbage).
// Execution always continues from the CLEAN run's state, so production errors
// never accumulate — each slice is tested in isolation from a good baseline.
u32 g_ls_intc_calls = 0;            // bumped at every in-slice UpdateINTC site
static u32 g_ls_intc_at_begin = 0;
u32 g_ls_interp_execs = 0;          // bumped at every single-instruction interp site
static u32 g_ls_interp_at_begin = 0;
static bool g_lockstep_halt = false;
static u32 g_ls_compared = 0, g_ls_matched = 0, g_ls_skip_int = 0,
           g_ls_skip_mmio = 0, g_ls_skip_threw = 0, g_ls_cc_only = 0,
           g_ls_skew = 0;
alignas(16) static u8 g_ls_pre[sizeof(Sh4Context)];
alignas(16) static u8 g_ls_prod[sizeof(Sh4Context)];
static std::vector<ValWrite> g_ls_prod_writes;

static void lockstep_begin() {
	if (g_lockstep_halt) return;
	valInstallWriteHook();
	memcpy(g_ls_pre, &Sh4cntx, sizeof(Sh4Context));
	g_val_writes.clear();
	g_val_mmio_touched = false;
	g_val_mmio_wrote = false;
	g_ls_intc_at_begin = g_ls_intc_calls;
	g_ls_interp_at_begin = g_ls_interp_execs;
	g_val_logging = true;
}

static void lockstep_compare(u32 ctx_ptr, u32 ram_base) {
	Sh4Context& ctx = Sh4cntx;
	g_val_logging = false;
	if (g_lockstep_halt) return;
	g_ls_compared++;

	// Snapshot production result + its write log.
	memcpy(g_ls_prod, &ctx, sizeof(Sh4Context));
	g_ls_prod_writes = g_val_writes;
	bool prod_mmio = g_val_mmio_touched;
	u32 prod_interp = g_ls_interp_execs - g_ls_interp_at_begin;  // interp'd instrs in prod slice

	// Undo production's RAM writes (reverse order, size bytes) — same as the
	// block validator. MMIO/VRAM side effects persist (handled by policy above).
	for (size_t _wi = g_ls_prod_writes.size(); _wi-- > 0; ) {
		const ValWrite& w = g_ls_prod_writes[_wi];
		if (!w.is_ram) continue;
		u8* p = &mem_b[(w.addr & 0x1FFFFFFF) & RAM_MASK];
		if (w.size >= 4)      *(u32*)p = w.old_val;
		else if (w.size == 2) *(u16*)p = (u16)w.old_val;
		else                  *p = (u8)w.old_val;
	}

	// Restore the snapshot and re-run the same slice through CLEAN.
	memcpy(&ctx, g_ls_pre, sizeof(Sh4Context));
	g_val_writes.clear();
	g_val_mmio_touched = false;
	g_val_mmio_wrote = false;
	g_val_logging = true;
	bool clean_threw = false;
	try {
		clean_dispatch_loop(ctx_ptr, ram_base);
	} catch (const SH4ThrownException& ex) {
		Do_Exception(ex.epc, ex.expEvn);
		clean_threw = true;
	} catch (...) {
		clean_threw = true;
	}
	g_val_logging = false;
	bool clean_mmio = g_val_mmio_touched;
	bool intc_fired = (g_ls_intc_calls != g_ls_intc_at_begin);

	// ---- Compare registers: production result (g_ls_prod) vs live ctx (clean).
	const Sh4Context& pctx = *(const Sh4Context*)g_ls_prod;
	bool match = true;
	const char* diff_name = ""; int diff_idx = -1; u32 pv = 0, cv = 0;
#define LS_CMP(field, name) \
	if (match && pctx.field != ctx.field) { match = false; diff_name = name; \
		pv = (u32)pctx.field; cv = (u32)ctx.field; }
#define LS_CMP_ARR(arr, count, name) \
	if (match) for (int _i = 0; _i < (count); _i++) { \
		if (*(const u32*)&pctx.arr[_i] != *(const u32*)&ctx.arr[_i]) { \
			match = false; diff_name = name; diff_idx = _i; \
			pv = *(const u32*)&pctx.arr[_i]; cv = *(const u32*)&ctx.arr[_i]; break; } }
	LS_CMP(pc, "pc")
	LS_CMP_ARR(r, 16, "r")
	LS_CMP(sr.T, "sr.T")
	LS_CMP(sr.status, "sr.status")
	LS_CMP_ARR(fr, 16, "fr")
	LS_CMP_ARR(xf, 16, "xf")
	LS_CMP(mac.l, "mac.l")
	LS_CMP(mac.h, "mac.h")
	LS_CMP(pr, "pr")
	LS_CMP(fpscr.full, "fpscr")
	LS_CMP(gbr, "gbr")
	LS_CMP(fpul, "fpul")
	LS_CMP_ARR(r_bank, 8, "r_bank")
	LS_CMP(vbr, "vbr")
	LS_CMP(ssr, "ssr")
	LS_CMP(spc, "spc")
	LS_CMP(sgr, "sgr")
	LS_CMP(dbr, "dbr")
	LS_CMP(cycle_counter, "cycle_counter")
#undef LS_CMP
#undef LS_CMP_ARR

	// ---- Compare write streams: EVERYTHING (RAM + VRAM + MMIO), in order.
	// Compare the OVERLAP first: an intra-stream mismatch (wdiff >= 0) is real
	// data divergence. Identical overlap with differing counts = one run got
	// further before its cycle budget expired = boundary skew, prefix class.
	size_t n_overlap = g_ls_prod_writes.size() < g_val_writes.size()
		? g_ls_prod_writes.size() : g_val_writes.size();
	int wdiff = -1;
	for (size_t i = 0; i < n_overlap; i++) {
		const ValWrite& a = g_ls_prod_writes[i];
		const ValWrite& b = g_val_writes[i];
		if (a.addr != b.addr || a.size != b.size || a.new_val != b.new_val) {
			wdiff = (int)i; break;
		}
	}
	bool wmatch = (wdiff < 0) && (g_ls_prod_writes.size() == g_val_writes.size());
	bool wprefix = (wdiff < 0) && !wmatch;  // overlap clean, counts differ → skew

	if (match && wmatch) {
		g_ls_matched++;
		if ((g_ls_matched & 0x3FFF) == 1)
			EM_ASM({ console.log('[LOCKSTEP] alive — ' + $0 + ' slices matched (skip: int=' +
				$1 + ' mmio=' + $2 + ' threw=' + $3 + ' cc-only=' + $4 + ' skew=' + $5 + ')'); },
				g_ls_matched, g_ls_skip_int, g_ls_skip_mmio, g_ls_skip_threw, g_ls_cc_only, g_ls_skew);
	} else if (!match && wmatch && strcmp(diff_name, "cycle_counter") == 0) {
		// GUEST-TIMING SKEW, not data corruption: every register and every write
		// matches; only the cycle charge differs. Fingerprint of the production
		// miss handler's over-budget interp path (-=1/instr vs block guest_cycles).
		// Logged + counted — a systematic skew here shifts vblank/interrupt timing
		// (a real glitch candidate), but it must not halt the data-diff hunt.
		g_ls_cc_only++;
		if (g_ls_cc_only <= 10 || (g_ls_cc_only & 0x3FF) == 0)
			EM_ASM({ console.log('[LOCKSTEP] CC-ONLY diff #' + $0 + ' @slice ' + $1 +
				' prod_cc=' + $2 + ' clean_cc=' + $3 + ' (guest-timing skew, no data diff)'); },
				g_ls_cc_only, g_ls_compared, (int)pctx.cycle_counter, (int)ctx.cycle_counter);
	} else if (clean_threw) {
		g_ls_skip_threw++;
	} else if (intc_fired) {
		g_ls_skip_int++;
	} else if (prod_mmio || clean_mmio) {
		g_ls_skip_mmio++;
		static u32 mmio_diff_logged = 0;
		if (mmio_diff_logged < 5) { mmio_diff_logged++;
			EM_ASM({ console.log('[LOCKSTEP] mmio-slice diff skipped (regs=' + ($0?'OK':'DIFF') +
				' writes=' + ($1?'OK':'DIFF') + ') @slice ' + $2); }, match, wmatch, g_ls_compared); }
	} else if (wmatch || wprefix) {
		// BOUNDARY SKEW: the write-stream overlap is identical (wmatch: identical
		// throughout; wprefix: one run got a few writes further) and registers
		// reflect the same loop at different progress points. Both runs execute
		// correct semantics — the slice boundary landed differently because the
		// cycle CHARGING differed (budget-interp charges 1/instr vs compiled
		// blocks' coarser guest_cycles). prod_interp directly attributes it.
		// This is the guest-timing-warp signal; it must NOT halt the data hunt —
		// real corruption shows as a mismatched entry WITHIN the overlap.
		g_ls_skew++;
		if (g_ls_skew <= 10 || (g_ls_skew & 0xFF) == 0)
			EM_ASM({ console.log('[LOCKSTEP-SKEW] #' + $0 + ' @slice ' + $1 +
				' entry=0x' + ($2>>>0).toString(16) +
				' pc prod=0x' + ($3>>>0).toString(16) + ' clean=0x' + ($4>>>0).toString(16) +
				' cc prod=' + $5 + ' clean=' + $6 +
				' writes prod=' + $7 + ' clean=' + $8 +
				' prod_interp=' + $9 + ' (overlap identical)'); },
				g_ls_skew, g_ls_compared, ((const Sh4Context*)g_ls_pre)->pc,
				pctx.pc, ctx.pc, (int)pctx.cycle_counter, (int)ctx.cycle_counter,
				(u32)g_ls_prod_writes.size(), (u32)g_val_writes.size(), prod_interp);
	} else {
		// ===== REAL divergence: differing WRITES in a comparable slice — DUMP + HALT =====
		g_lockstep_halt = true;
		EM_ASM({ console.error('================ [LOCKSTEP] DIVERGENCE — HALTING ================'); });
		EM_ASM({ console.error('[LOCKSTEP] slice #' + $0 + ' (matched ' + $1 + ' before this)' +
			' entry_pc=0x' + ($2>>>0).toString(16) + ' prod_interp=' + $3); },
			g_ls_compared, g_ls_matched, ((const Sh4Context*)g_ls_pre)->pc, prod_interp);
		if (!match)
			EM_ASM({ console.error('[LOCKSTEP] REG DIFF ' + UTF8ToString($0) +
				(($1 >= 0) ? ('[' + $1 + ']') : '') + ' prod=0x' + ($2>>>0).toString(16) +
				' clean=0x' + ($3>>>0).toString(16)); }, diff_name, diff_idx, pv, cv);
		EM_ASM({ console.error('[LOCKSTEP] writes: prod=' + $0 + ' clean=' + $1 +
			' first_diff_idx=' + $2); },
			(u32)g_ls_prod_writes.size(), (u32)g_val_writes.size(), wdiff);
		// Dump the write-stream neighborhood around the first difference.
		{
			size_t start = (wdiff > 4) ? (size_t)(wdiff - 4) : 0;
			for (size_t i = start; i < start + 12; i++) {
				u32 pa = 0, ps = 0, pvv = 0, ca = 0, cs = 0, cvv = 0;
				bool hp = i < g_ls_prod_writes.size(), hc = i < g_val_writes.size();
				if (hp) { pa = g_ls_prod_writes[i].addr; ps = g_ls_prod_writes[i].size; pvv = g_ls_prod_writes[i].new_val; }
				if (hc) { ca = g_val_writes[i].addr; cs = g_val_writes[i].size; cvv = g_val_writes[i].new_val; }
				if (!hp && !hc) break;
				EM_ASM({ console.error('[LOCKSTEP]   w[' + $0 + '] prod ' +
					($1 ? ('0x' + ($2>>>0).toString(16) + ' sz' + $3 + ' =0x' + ($4>>>0).toString(16)) : '—') +
					' | clean ' +
					($5 ? ('0x' + ($6>>>0).toString(16) + ' sz' + $7 + ' =0x' + ($8>>>0).toString(16)) : '—')); },
					(u32)i, hp, pa, ps, pvv, hc, ca, cs, cvv);
			}
		}
		// Register dump (prod vs clean) + recent production block PCs.
		for (int i = 0; i < 16; i++)
			EM_ASM({ console.error('[LOCKSTEP]   r[' + $0 + '] prod=0x' + ($1>>>0).toString(16) +
				' clean=0x' + ($2>>>0).toString(16)); }, i, pctx.r[i], ctx.r[i]);
		EM_ASM({ console.error('[LOCKSTEP]   pc prod=0x' + ($0>>>0).toString(16) + ' clean=0x' + ($1>>>0).toString(16) +
			' | pr prod=0x' + ($2>>>0).toString(16) + ' clean=0x' + ($3>>>0).toString(16) +
			' | cc prod=' + $4 + ' clean=' + $5); },
			pctx.pc, ctx.pc, pctx.pr, ctx.pr, pctx.cycle_counter, ctx.cycle_counter);
#ifndef JIT_PROD_BUILD
		{
			u32 head = g_pc_trace_head;
			u32 cnt = head < 24 ? head : 24;
			for (u32 t = 0; t < cnt; t++) {
				u32 idx = (head - cnt + t) & PC_TRACE_MASK;
				EM_ASM({ console.error('[LOCKSTEP]   prodPC[' + $0 + ']=0x' + ($1>>>0).toString(16)); },
					(int)t - (int)cnt, g_pc_trace[idx]);
			}
		}
#endif
		EM_ASM({ console.error('================ [LOCKSTEP] END DUMP — emulation frozen ================'); });
	}
	// Execution continues from the CLEAN state (live ctx) — good baseline.
}
#endif  // LOCKSTEP_DIFF_BUILD

#ifndef JIT_PROD_BUILD
// Called from the freeze watchdog — dumps the last 64 dispatched PCs to
// console in chronological order. We dump 64 (not 256) to keep the log
// readable; the full 256 is available via the ring if needed.
extern "C" void EMSCRIPTEN_KEEPALIVE fly_dump_pc_trace() {
	u32 head = g_pc_trace_head;
	u32 count = head < 64 ? head : 64;
	EM_ASM({
		if (!window._flyLog) window._flyLog = [];
		window._flyLog.push('[FREEZE] PC trace (last ' + $0 + ' blocks):');
	}, count);
	for (u32 i = 0; i < count; i++) {
		u32 idx = (head - count + i) & PC_TRACE_MASK;
		u32 pc = g_pc_trace[idx];
		EM_ASM({
			window._flyLog.push('[FREEZE]   #' + $0 + ' pc=0x' + ($1>>>0).toString(16));
		}, (u32)(i - count), pc);
	}
}

// Called once on the first Sh4Ex_IllegalInstr of the session. Captures the
// current PC trace into a separate buffer that won't get overwritten by the
// fault loop. Called from sh4_interrupts.cpp Do_Exception().
extern "C" void EMSCRIPTEN_KEEPALIVE fly_snapshot_first_exc(u32 epc, u32 evn) {
	if (g_first_exc_pc_trace_head != 0) return;  // already captured
	g_first_exc_epc = epc;
	g_first_exc_evn = evn;
	memcpy(g_first_exc_pc_trace, g_pc_trace, sizeof(g_pc_trace));
	memcpy(g_first_exc_pc_trace_spc, g_pc_trace_spc, sizeof(g_pc_trace_spc));
	memcpy(g_first_exc_pc_trace_sr, g_pc_trace_sr, sizeof(g_pc_trace_sr));
	g_first_exc_pc_trace_head = g_pc_trace_head;
	if (g_first_exc_pc_trace_head == 0) g_first_exc_pc_trace_head = 1; // ensure non-zero sentinel
	// Capture register state BEFORE the handler runs. These are the values
	// the faulting block set up — one of rN is the bad jump target (epc).
	for (int i = 0; i < 16; i++) g_first_exc_r[i] = Sh4cntx.r[i];
	for (int i = 0; i < 8; i++)  g_first_exc_r_bank[i] = Sh4cntx.r_bank[i];
	g_first_exc_pr   = Sh4cntx.pr;
	g_first_exc_gbr  = Sh4cntx.gbr;
	g_first_exc_mach = (u32)(Sh4cntx.mac.full >> 32);
	g_first_exc_macl = (u32)(Sh4cntx.mac.full & 0xFFFFFFFF);
	g_first_exc_sr   = Sh4cntx.sr.getFull();
	g_first_exc_spc  = Sh4cntx.spc;
	g_first_exc_ssr  = Sh4cntx.ssr;
	g_first_exc_sgr  = Sh4cntx.sgr;
	g_first_exc_vbr  = Sh4cntx.vbr;
}

// Dump the first-exception PC trace. Shows the blocks dispatched right before
// the first illegal-instruction exception of the session — i.e., the block
// that made the bad jump.
extern "C" void EMSCRIPTEN_KEEPALIVE fly_dump_first_exc_trace() {
	if (g_first_exc_pc_trace_head == 0) {
		EM_ASM({
			if (!window._flyLog) window._flyLog = [];
			window._flyLog.push('[FREEZE] no first-exception snapshot (no illegal exc fired?)');
		});
		return;
	}
	u32 head = g_first_exc_pc_trace_head;
	u32 count = head < 64 ? head : 64;
	EM_ASM({
		if (!window._flyLog) window._flyLog = [];
		window._flyLog.push('[FREEZE] First-exc snapshot: epc=0x'
			+ ($0>>>0).toString(16)
			+ ' evn=0x' + ($1>>>0).toString(16)
			+ ', last ' + $2 + ' blocks leading to it:');
	}, g_first_exc_epc, g_first_exc_evn, count);
	for (u32 i = 0; i < count; i++) {
		u32 idx = (head - count + i) & PC_TRACE_MASK;
		u32 pc = g_first_exc_pc_trace[idx];
		u32 spc = g_first_exc_pc_trace_spc[idx];
		u32 sr = g_first_exc_pc_trace_sr[idx];
		EM_ASM({
			var sr = $3 >>> 0;
			window._flyLog.push('[FREEZE]   #' + $0
				+ ' pc=0x' + ($1>>>0).toString(16).padStart(8,'0')
				+ ' spc=0x' + ($2>>>0).toString(16).padStart(8,'0')
				+ ' [MD=' + ((sr>>30)&1)
				+ ' RB=' + ((sr>>29)&1)
				+ ' BL=' + ((sr>>28)&1) + ']');
		}, (u32)(i - count), pc, spc, sr);
	}

	// Register state at the moment of first fault. One of rN == epc reveals
	// which register held the bad jump target.
	EM_ASM({
		window._flyLog.push('[FREEZE] Registers at first fault:');
	});
	for (int i = 0; i < 16; i += 4) {
		EM_ASM({
			window._flyLog.push('[FREEZE]   r' + $0 + '=0x' + ($1>>>0).toString(16).padStart(8,'0')
				+ ' r' + ($0+1) + '=0x' + ($2>>>0).toString(16).padStart(8,'0')
				+ ' r' + ($0+2) + '=0x' + ($3>>>0).toString(16).padStart(8,'0')
				+ ' r' + ($0+3) + '=0x' + ($4>>>0).toString(16).padStart(8,'0'));
		}, i, g_first_exc_r[i], g_first_exc_r[i+1], g_first_exc_r[i+2], g_first_exc_r[i+3]);
	}
	EM_ASM({
		window._flyLog.push('[FREEZE]   pr=0x' + ($0>>>0).toString(16)
			+ ' gbr=0x' + ($1>>>0).toString(16)
			+ ' mach=0x' + ($2>>>0).toString(16)
			+ ' macl=0x' + ($3>>>0).toString(16));
	}, g_first_exc_pr, g_first_exc_gbr, g_first_exc_mach, g_first_exc_macl);
	// SR + other special regs, so we know which bank was active, whether
	// the game was privileged, and what saved-exc state was in place.
	EM_ASM({
		var sr = $0 >>> 0;
		window._flyLog.push('[FREEZE]   sr=0x' + sr.toString(16).padStart(8,'0')
			+ ' [MD=' + ((sr>>30)&1)
			+ ' RB=' + ((sr>>29)&1)
			+ ' BL=' + ((sr>>28)&1)
			+ ' IMASK=' + ((sr>>4)&0xF) + ']');
	}, g_first_exc_sr);
	EM_ASM({
		window._flyLog.push('[FREEZE]   spc=0x' + ($0>>>0).toString(16)
			+ ' ssr=0x' + ($1>>>0).toString(16)
			+ ' sgr=0x' + ($2>>>0).toString(16)
			+ ' vbr=0x' + ($3>>>0).toString(16));
	}, g_first_exc_spc, g_first_exc_ssr, g_first_exc_sgr, g_first_exc_vbr);
	// Opposite bank's r0-r7 (at snapshot's RB, these are the *other* bank).
	EM_ASM({
		window._flyLog.push('[FREEZE]   bank-other r0-7: 0x'
			+ ($0>>>0).toString(16).padStart(8,'0') + ' '
			+ ($1>>>0).toString(16).padStart(8,'0') + ' '
			+ ($2>>>0).toString(16).padStart(8,'0') + ' '
			+ ($3>>>0).toString(16).padStart(8,'0') + ' '
			+ ($4>>>0).toString(16).padStart(8,'0') + ' '
			+ ($5>>>0).toString(16).padStart(8,'0') + ' '
			+ ($6>>>0).toString(16).padStart(8,'0') + ' '
			+ ($7>>>0).toString(16).padStart(8,'0'));
	}, g_first_exc_r_bank[0], g_first_exc_r_bank[1], g_first_exc_r_bank[2], g_first_exc_r_bank[3],
	   g_first_exc_r_bank[4], g_first_exc_r_bank[5], g_first_exc_r_bank[6], g_first_exc_r_bank[7]);

	// Dump the SH4 instructions of the last pre-fault block. Read 64 16-bit
	// words (128 bytes) — enough to cover any realistic block including the
	// terminating branch. These are the actual instructions the JIT compiled
	// (and whose behavior produced the bad jump target).
	if (g_first_exc_pc_trace_head > 0) {
		u32 last_idx = (g_first_exc_pc_trace_head - 1) & PC_TRACE_MASK;
		u32 last_pc = g_first_exc_pc_trace[last_idx];
		u32 phys = last_pc & 0x1FFFFFFF;
		if ((phys >> 26) == 3) { // area 3 = main RAM
			EM_ASM({
				window._flyLog.push('[FREEZE] SH4 instructions at faulting block (0x'
					+ ($0>>>0).toString(16) + '):');
			}, last_pc);
			u32 ram_off = phys & 0x00FFFFFF;
			for (u32 i = 0; i < 64; i++) {
				u16 w = *(u16*)&mem_b[ram_off + i * 2];
				EM_ASM({
					window._flyLog.push('[FREEZE]   [0x' + ($0>>>0).toString(16)
						+ '] = 0x' + ($1>>>0).toString(16).padStart(4,'0'));
				}, last_pc + i * 2, (u32)w);
			}
		}

		// Dump the compiled SHIL ops for the faulting block. This is what the
		// JIT actually executed — compare against a proper decode of the raw
		// SH4 bytes above to determine whether the block was miscompiled, or
		// compiled correctly but fed bad input by an earlier block.
		auto blk_it = blockByVaddr.find(last_pc);
		if (blk_it != blockByVaddr.end()) {
			RuntimeBlockInfo* blk = blk_it->second;
			EM_ASM({
				window._flyLog.push('[FREEZE] SHIL oplist for block 0x'
					+ ($0>>>0).toString(16) + ': '
					+ $1 + ' ops, sh4_sz=' + $2
					+ ' bcls=' + $3
					+ ' branch=0x' + ($4>>>0).toString(16)
					+ ' next=0x' + ($5>>>0).toString(16)
					+ ' has_jcond=' + $6);
			}, last_pc, (u32)blk->oplist.size(), blk->sh4_code_size,
			   (u32)blk->BlockType, blk->BranchBlock, blk->NextBlock,
			   blk->has_jcond ? 1 : 0);
			for (u32 i = 0; i < blk->oplist.size() && i < 64; i++) {
				const shil_opcode& op = blk->oplist[i];
				EM_ASM({
					window._flyLog.push('[FREEZE]   [' + $0 + ']'
						+ ' shop=' + $1
						+ ' rd.t=' + $2 + ',off=0x' + ($3>>>0).toString(16)
						+ ' rs1.t=' + $4 + ',off=0x' + ($5>>>0).toString(16)
						+ ' rs2.t=' + $6 + ',off=0x' + ($7>>>0).toString(16)
						+ ' sz=' + $8);
				}, i, (int)op.op,
				   (int)op.rd.type,  op.rd.is_reg()  ? op.rd.reg_offset()  : op.rd._imm,
				   (int)op.rs1.type, op.rs1.is_reg() ? op.rs1.reg_offset() : op.rs1._imm,
				   (int)op.rs2.type, op.rs2.is_reg() ? op.rs2.reg_offset() : op.rs2._imm,
				   op.size);
			}
		} else {
			EM_ASM({
				window._flyLog.push('[FREEZE] NO COMPILED BLOCK in blockByVaddr for 0x'
					+ ($0>>>0).toString(16) + ' (evicted or never registered)');
			}, last_pc);
		}
	}

	// Dump the WRITER block identified by MEM-WATCH — the one that corrupts
	// code memory with function pointers. See flylog4 for evidence.
	{
		const u32 writer_pc = 0x8c0084f0;
		auto wit = blockByVaddr.find(writer_pc);
		if (wit != blockByVaddr.end()) {
			RuntimeBlockInfo* wblk = wit->second;
			EM_ASM({
				window._flyLog.push('[FREEZE] WRITER BLOCK 0x'
					+ ($0>>>0).toString(16) + ': '
					+ $1 + ' ops, sh4_sz=' + $2
					+ ' bcls=' + $3
					+ ' branch=0x' + ($4>>>0).toString(16)
					+ ' next=0x' + ($5>>>0).toString(16));
			}, writer_pc, (u32)wblk->oplist.size(), wblk->sh4_code_size,
			   (u32)wblk->BlockType, wblk->BranchBlock, wblk->NextBlock);
			for (u32 i = 0; i < wblk->oplist.size() && i < 64; i++) {
				const shil_opcode& op = wblk->oplist[i];
				EM_ASM({
					window._flyLog.push('[FREEZE]   WOP[' + $0 + ']'
						+ ' shop=' + $1
						+ ' rd.t=' + $2 + ',off=0x' + ($3>>>0).toString(16)
						+ ' rs1.t=' + $4 + ',off=0x' + ($5>>>0).toString(16)
						+ ' rs2.t=' + $6 + ',off=0x' + ($7>>>0).toString(16)
						+ ' rs3.t=' + $8 + ',off=0x' + ($9>>>0).toString(16)
						+ ' sz=' + $10);
				}, i, (int)op.op,
				   (int)op.rd.type,  op.rd.is_reg()  ? op.rd.reg_offset()  : op.rd._imm,
				   (int)op.rs1.type, op.rs1.is_reg() ? op.rs1.reg_offset() : op.rs1._imm,
				   (int)op.rs2.type, op.rs2.is_reg() ? op.rs2.reg_offset() : op.rs2._imm,
				   (int)op.rs3.type, op.rs3.is_reg() ? op.rs3.reg_offset() : op.rs3._imm,
				   op.size);
			}
			// Raw SH4 bytes of writer block (128 bytes)
			u32 wphys = writer_pc & 0x1FFFFFFF;
			if ((wphys >> 26) == 3) {
				u32 woff = wphys & RAM_MASK;
				EM_ASM({ window._flyLog.push('[FREEZE] WRITER BLOCK raw SH4 bytes:'); });
				for (u32 i = 0; i < 64; i++) {
					u16 w = *(u16*)&mem_b[(woff + i * 2) & RAM_MASK];
					EM_ASM({
						window._flyLog.push('[FREEZE]   WB[0x' + ($0>>>0).toString(16)
							+ '] = 0x' + ($1>>>0).toString(16).padStart(4,'0'));
					}, writer_pc + i * 2, (u32)w);
				}
			}
		}
	}

	// Dump bytes at the FAULT EPC itself — this is where the illegal
	// instruction is. If bytes are all zeros, this address is uninitialized
	// (game was expected to load code here but didn't, or wrong target).
	// If bytes look like valid code but an early byte is illegal, it's
	// probably data-treated-as-code (wrong jump target).
	if (g_first_exc_epc != 0) {
		u32 epc_phys = g_first_exc_epc & 0x1FFFFFFF;
		if ((epc_phys >> 26) == 3) {
			u32 epc_ram = (epc_phys - 32) & 0x00FFFFFF;  // 32 bytes before EPC
			EM_ASM({
				window._flyLog.push('[FREEZE] RAM around EPC 0x'
					+ ($0>>>0).toString(16)
					+ ' (32 bytes before + 96 after):');
			}, g_first_exc_epc);
			for (u32 i = 0; i < 64; i++) {
				u32 addr_in_ram = (epc_ram + i * 2) & RAM_MASK;
				u16 w = *(u16*)&mem_b[addr_in_ram];
				u32 sh4_addr = (g_first_exc_epc - 32) + i * 2;
				EM_ASM({
					window._flyLog.push('[FREEZE]   [0x' + ($0>>>0).toString(16)
						+ '] = 0x' + ($1>>>0).toString(16).padStart(4,'0'));
				}, sh4_addr, (u32)w);
			}
		}
	}
}

// Dump the first-dispatch ring. Given a target PC, this walks the ring and
// prints (target, caller) pairs. If target_pc is non-zero, only prints the
// matching entry. Otherwise prints all (up to 256).
extern "C" void EMSCRIPTEN_KEEPALIVE fly_dump_new_pcs(u32 target_pc) {
	u32 head = g_newpc_ring_head;
	u32 count = head < NEWPC_RING_SIZE ? head : NEWPC_RING_SIZE;
	if (target_pc != 0) {
		EM_ASM({
			if (!window._flyLog) window._flyLog = [];
			window._flyLog.push('[FREEZE] First-dispatch lookup for 0x'
				+ ($0>>>0).toString(16) + ':');
		}, target_pc);
		bool found = false;
		for (u32 i = 0; i < count; i++) {
			u32 idx = (head - count + i) & NEWPC_RING_MASK;
			if (g_newpc_ring_pc[idx] == target_pc) {
				found = true;
				EM_ASM({
					window._flyLog.push('[FREEZE]   first-caller of 0x'
						+ ($0>>>0).toString(16) + ' was 0x'
						+ ($1>>>0).toString(16));
				}, g_newpc_ring_pc[idx], g_newpc_ring_prev[idx]);
			}
		}
		if (!found) {
			EM_ASM({
				window._flyLog.push('[FREEZE]   not found in ring (evicted or never recorded)');
			});
		}
	} else {
		EM_ASM({
			if (!window._flyLog) window._flyLog = [];
			window._flyLog.push('[FREEZE] First-dispatch ring (' + $0 + ' entries):');
		}, count);
		for (u32 i = 0; i < count; i++) {
			u32 idx = (head - count + i) & NEWPC_RING_MASK;
			EM_ASM({
				window._flyLog.push('[FREEZE]   new pc=0x' + ($0>>>0).toString(16)
					+ ' caller=0x' + ($1>>>0).toString(16));
			}, g_newpc_ring_pc[idx], g_newpc_ring_prev[idx]);
		}
	}
}
#endif

#else
static int wasm_compile_block(const u8*, u32, u32) { return 0; }
static void wasm_compile_block_async(const u8*, u32, u32, u32, u32) {}
static int wasm_compile_block_batch(const u8*, u32, const u32*, u32, u32*) { return 0; }
static int wasm_get_block_idx(u32) { return 0; }
static int wasm_compile_chain(const u8*, u32, u32) { return 0; }
static int wasm_execute_chain(u32, u32, u32) { return 0; }
static int wasm_has_chain(u32) { return 0; }
static void wasm_clear_chains() {}
static void wasm_remove_chain(u32) {}
static void wasm_compile_chain_async(const u8*, u32, u32) {}
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
static int clean_dispatch_loop(u32, u32) { return 0; }
#endif

// ============================================================
// Build a complete WASM module for one compiled block
// ============================================================

// Forward declaration — defined below after block module builder.
static void emitFlushAllUnconditional(WasmModuleBuilder& b, const RegCache& cache);

static void emitBlockFuncBody(WasmModuleBuilder& b, RuntimeBlockInfo* block);

static bool buildBlockModule(WasmModuleBuilder& b, RuntimeBlockInfo* block) {
	b.emitHeader();

	// Type section: 3 function signatures
	// Type 0: (i32, i32) -> void — block function (ctx_ptr, ram_base)
	// Type 1: (i32) -> i32       — read8/16/32
	// Type 2: (i32, i32) -> void — write8/16/32, ifb, shil_fb
	b.emitTypeSection(5);
	{
		u8 p0[] = { WASM_TYPE_I32, WASM_TYPE_I32 };
		b.emitFuncType(p0, 2, nullptr, 0);

		u8 p1[] = { WASM_TYPE_I32 };
		u8 r1[] = { WASM_TYPE_I32 };
		b.emitFuncType(p1, 1, r1, 1);

		u8 p2[] = { WASM_TYPE_I32, WASM_TYPE_I32 };
		b.emitFuncType(p2, 2, nullptr, 0);
		u8 p3[] = { WASM_TYPE_I32 };
		b.emitFuncType(p3, 1, nullptr, 0);                 // type 3: (i32)->void — sq_pref
		u8 p4[] = { WASM_TYPE_I32, WASM_TYPE_I32, WASM_TYPE_I32 };
		u8 r4[] = { WASM_TYPE_I64 };
		b.emitFuncType(p4, 3, r4, 1);                      // type 4: (i32,i32,i32)->i64 — div ops
	}
	b.endSection();

	// Import section: 1 memory + 8 functions
	b.emitImportSection(13);
	b.emitImportMemory("env", "memory", 0);
	b.emitImportFunc("env", "read8",   1);
	b.emitImportFunc("env", "read16",  1);
	b.emitImportFunc("env", "read32",  1);
	b.emitImportFunc("env", "write8",  2);
	b.emitImportFunc("env", "write16", 2);
	b.emitImportFunc("env", "write32", 2);
	b.emitImportFunc("env", "ifb",     2);
	b.emitImportFunc("env", "shil_fb", 2);
	b.emitImportFunc("env", "sq_pref", 3);
	b.emitImportFunc("env", "div32u",  4);
	b.emitImportFunc("env", "div32s",  4);
	b.emitImportFunc("env", "div1",    4);
	b.endSection();

	// Function section: 1 defined function (type 0)
	u32 typeIdx = 0;
	b.emitFunctionSection(1, &typeIdx);

	// Export section: export block function as "b" (func idx 8)
	b.emitExportSection("b", WIMPORT_COUNT);

	// Code section
	b.setHintFuncIndexBase(WIMPORT_COUNT);
	b.beginCodeSection(1);
	emitBlockFuncBody(b, block);
	b.endSection();

	return true;
}

// Batch module: N independent block functions in ONE Module (2026-07-18).
// ~95% of a small module's sync-compile cost is per-MODULE fixed overhead,
// so promoting a transition working set as one multi-function module turns
// "~1ms x N blocks across many frames" into one ~1-2ms hit.
static bool buildBatchModule(WasmModuleBuilder& b,
                              const std::vector<RuntimeBlockInfo*>& blocks) {
	b.emitHeader();

	b.emitTypeSection(5);
	{
		u8 p0[] = { WASM_TYPE_I32, WASM_TYPE_I32 };
		b.emitFuncType(p0, 2, nullptr, 0);
		u8 p1[] = { WASM_TYPE_I32 };
		u8 r1[] = { WASM_TYPE_I32 };
		b.emitFuncType(p1, 1, r1, 1);
		u8 p2[] = { WASM_TYPE_I32, WASM_TYPE_I32 };
		b.emitFuncType(p2, 2, nullptr, 0);
		u8 p3[] = { WASM_TYPE_I32 };
		b.emitFuncType(p3, 1, nullptr, 0);
		u8 p4[] = { WASM_TYPE_I32, WASM_TYPE_I32, WASM_TYPE_I32 };
		u8 r4[] = { WASM_TYPE_I64 };
		b.emitFuncType(p4, 3, r4, 1);
	}
	b.endSection();

	b.emitImportSection(13);
	b.emitImportMemory("env", "memory", 0);
	b.emitImportFunc("env", "read8",   1);
	b.emitImportFunc("env", "read16",  1);
	b.emitImportFunc("env", "read32",  1);
	b.emitImportFunc("env", "write8",  2);
	b.emitImportFunc("env", "write16", 2);
	b.emitImportFunc("env", "write32", 2);
	b.emitImportFunc("env", "ifb",     2);
	b.emitImportFunc("env", "shil_fb", 2);
	b.emitImportFunc("env", "sq_pref", 3);
	b.emitImportFunc("env", "div32u",  4);
	b.emitImportFunc("env", "div32s",  4);
	b.emitImportFunc("env", "div1",    4);
	b.endSection();

	std::vector<u32> typeIdxs(blocks.size(), 0);
	b.emitFunctionSection((u32)blocks.size(), typeIdxs.data());
	b.emitExportSectionMulti("b", WIMPORT_COUNT, (u32)blocks.size());

	b.setHintFuncIndexBase(WIMPORT_COUNT);
	b.beginCodeSection((u32)blocks.size());
	for (auto* blk : blocks)
		emitBlockFuncBody(b, blk);
	b.endSection();

	return true;
}

// One block's complete function body (locals + prologue + ops + exit) —
// shared by buildBlockModule (1 function) and buildBatchModule (N functions).
static void emitBlockFuncBody(WasmModuleBuilder& b, RuntimeBlockInfo* block) {
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
#ifndef JIT_PROD_BUILD
	if (is_idle_loop) prof_idle_loops_detected++;
#endif

	b.beginFuncBody();

	// Locals: 5 fixed i32 scratch + N cached register i32s + 1 i64 scratch
	u32 i32Count = LOCAL_FIXED_I32_COUNT + cache.localCount();
	u32 i64Count = 1;
	u32 groupCounts[2] = { i32Count, i64Count };
	u8 groupTypes[2] = { WASM_TYPE_I32, WASM_TYPE_I64 };
	b.emitLocals(2, groupCounts, groupTypes);
	cache._tmp64LocalIdx = 2 + i32Count;  // i64 local after all i32s

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

	// Exception-abort address: bake the address of g_ifb_exception_pending
	// into the WASM as a constant so the block can check it after each
	// fallback call and abort if an exception was thrown mid-block.
	// Without this, post-exception ops execute and corrupt memory state
	// (the root cause of the Sonic/VT/FMV-cluster blank-canvas bug).
	u32 excFlagAddr = (u32)(uintptr_t)&g_ifb_exception_pending;

	// Wrap the op sequence + block exit in a block. If a fallback call
	// sets g_ifb_exception_pending, br $body skips remaining ops and the
	// unconditional flush after the block writes all cached regs safely.
	b.op_block();  // $body — br(0) exits to after the block

	// Emit each SHIL op with register cache
	for (u32 i = 0; i < block->oplist.size(); i++) {
		shil_opcode& op = block->oplist[i];
		if (!emitShilOp(b, op, block, i, cache)) {
#ifndef JIT_PROD_BUILD
			prof_fallback_ops_compiled++;
#endif
			// Unhandled op — flush, call fallback, reload
			emitFlushAll(b, cache);
			b.op_i32_const((s32)block->vaddr);
			b.op_i32_const((s32)i);
			b.op_call(WIMPORT_SHIL_FB);
			emitReloadAll(b, cache);

			// Check if the fallback set g_ifb_exception_pending.
			// If so, abort the rest of the block — remaining ops must
			// NOT execute (they'd write to wrong memory addresses since
			// the SH4 state is now in exception-handler mode).
			b.op_i32_const((s32)excFlagAddr);
			b.op_i32_load8_u(0);  // load the bool (1 byte, zero-extended)
			b.op_br_if(0);        // br $body → skip to after the block
		} else {
#ifndef JIT_PROD_BUILD
			prof_native_ops_compiled++;
#endif
		}
	}

	// Epilogue: block exit reads sr.T/jdyn from cached locals
	emitBlockExit(b, block, cache);

	b.op_end();  // end $body block

	// Epilogue: writeback ALL cached registers unconditionally.
	// Must be unconditional because if br_if skipped some ops, the
	// compile-time dirty flags don't reflect which ops actually ran.
	emitFlushAllUnconditional(b, cache);

	// Idle loop fast-forward: SOFT version
	// Instead of zeroing cycle_counter (which fires all scheduler events
	// instantly), cap it at 32 cycles. The idle loop still fast-forwards
	// through most of the timeslice but preserves scheduler event timing
	// to within ~32 cycles precision.
	// Emits: if (cycle_counter > 32) cycle_counter = 32;
	if (is_idle_loop) {
		auto emitSoftFastForward = [&]() {
			b.op_local_get(LOCAL_CTX);
			b.op_i32_load(ctx_off::CYCLE_COUNTER);
			b.op_i32_const(32);
			b.op_i32_gt_s();
			b.op_if();
			b.op_local_get(LOCAL_CTX);
			b.op_i32_const(32);
			b.op_i32_store(ctx_off::CYCLE_COUNTER);
			b.op_end();
		};

		if (bcls == BET_CLS_Static) {
			// Unconditional self-loop: always fast-forward
			emitSoftFastForward();
		} else {
			// Conditional self-loop: fast-forward only when branching back
			b.op_local_get(LOCAL_CTX);
			b.op_i32_load(ctx_off::PC);
			b.op_i32_const((s32)block->vaddr);
			b.op_i32_eq();
			b.op_if();
			emitSoftFastForward();
			b.op_end();
		}
	}

	b.endFuncBody();
}

// ============================================================
// Multi-block module: chain connected blocks into one WASM module
// ============================================================

static constexpr int MULTIBLOCK_MAX = 8;

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

	// ★ IDLE-LOOP EXCLUSION (2026-07-17, chain-shadow finding): single-block
	// modules emit the idle soft fast-forward (cap cycle_counter at 32) in
	// their epilogue; buildMultiBlockModule does NOT. An idle link spinning
	// in-module runs a different cc trajectory than the capped single block
	// — the chain-shadow farm measured exactly this (304/6656 pc diffs,
	// chain running further than the reference). Idle loops gain nothing
	// from chaining (memory-bound waits); keep their verified single-block
	// semantics by never chaining from or into them.
	auto fly_is_idle = [](RuntimeBlockInfo* blk) {
		u32 c = BET_GET_CLS(blk->BlockType);
		if (c == BET_CLS_Static && blk->BlockType != BET_StaticIntr
			&& blk->BranchBlock == blk->vaddr)
			return true;
		if (c == BET_CLS_COND
			&& (blk->BranchBlock == blk->vaddr || blk->NextBlock == blk->vaddr))
			return true;
		return false;
	};
	// ★ SQ/PREF EXCLUSION (2026-07-17, post-lockup): pref blocks submit SQ
	// bursts to the TA — global device state the shadow farm structurally
	// CANNOT validate (unrepeatable side effects; dev_skipped class). JGR's
	// live lockup was a wait loop starving next to the unvalidated SQ chain
	// region. Rule: only chain what the differential can certify.
	auto fly_has_pref = [](RuntimeBlockInfo* blk) {
		for (auto& sop : blk->oplist)
			if (sop.op == shop_pref)
				return true;
		return false;
	};
	if (fly_is_idle(entry) || fly_has_pref(entry))
		return chain;   // length 1 — caller compiles single-block only

	// BFS over BranchBlock (taken target) of every block already in the
	// chain. Accepts BET_CLS_Static (single target) and BET_CLS_COND
	// (taken target only — fall-through exits the module). Rejects dynamic
	// branches and StaticIntr. Bounded by MULTIBLOCK_MAX.
	//
	// Conservative variant: we do NOT chain the COND fall-through path.
	// buildMultiBlockModule's "both targets in chain" routing variant routes
	// based on a runtime ctx.pc comparison with an else-fallthrough — if a
	// SHIL fallback or exception sets ctx.pc to an unexpected value, the
	// else path silently executes the wrong block and poisons SH4 state.
	// The "branch target in chain" variant has an explicit pc equality check
	// that safely exits if ctx.pc doesn't match, so restricting to taken
	// target avoids the buggy variant entirely.
	//
	// Previous linear discover accepted only BET_CLS_Static which is ~5 %
	// of Shenmue blocks — the rest end in conditional branches, so chains
	// never formed and every block ran as a standalone WebAssembly.Module.
	// Allowing COND's taken path recovers the common hot loop pattern.
	for (size_t i = 0; i < chain.size() && (int)chain.size() < MULTIBLOCK_MAX; i++) {
		RuntimeBlockInfo* current = chain[i];
		u32 bcls = BET_GET_CLS(current->BlockType);
		if (bcls != BET_CLS_Static && bcls != BET_CLS_COND) continue;
		if (current->BlockType == BET_StaticIntr) continue;

		u32 target = current->BranchBlock;
		if (target == 0xFFFFFFFF || target == 0) continue;
		if (target == entry->vaddr) continue;  // self-loop — let outer dispatch handle it

		// Skip if already in chain
		bool dup = false;
		for (auto* b : chain) {
			if (b->vaddr == target) { dup = true; break; }
		}
		if (dup) continue;

		auto it = blockByVaddr.find(target);
		if (it == blockByVaddr.end()) continue;
		if (fly_is_idle(it->second)) continue;   // see idle-loop exclusion above
		if (fly_has_pref(it->second)) continue;  // see SQ/pref exclusion above

		chain.push_back(it->second);
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
                                   const std::vector<RuntimeBlockInfo*>& chain,
                                   u32* guard_cells = nullptr) {
	// Exception-abort address — same constant as in buildBlockModule.
	u32 excFlagAddr = (u32)(uintptr_t)&g_ifb_exception_pending;
	// Ops with single-block-only inline contracts (pref) take the fallback
	// path inside chains — see g_emitting_chain in wasm_emit.h.
	g_emitting_chain = true;

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

	// Extra local for dispatch index (after fixed scratch + cache locals)
	u32 LOCAL_NEXT_IDX = 2 + LOCAL_FIXED_I32_COUNT + cache.localCount();

	b.emitHeader();

	// Type section: same 3 types
	b.emitTypeSection(5);
	{
		u8 p0[] = { WASM_TYPE_I32, WASM_TYPE_I32 };
		b.emitFuncType(p0, 2, nullptr, 0);
		u8 p1[] = { WASM_TYPE_I32 };
		u8 r1[] = { WASM_TYPE_I32 };
		b.emitFuncType(p1, 1, r1, 1);
		u8 p2[] = { WASM_TYPE_I32, WASM_TYPE_I32 };
		b.emitFuncType(p2, 2, nullptr, 0);
		u8 p3[] = { WASM_TYPE_I32 };
		b.emitFuncType(p3, 1, nullptr, 0);                 // type 3: (i32)->void — sq_pref
		u8 p4[] = { WASM_TYPE_I32, WASM_TYPE_I32, WASM_TYPE_I32 };
		u8 r4[] = { WASM_TYPE_I64 };
		b.emitFuncType(p4, 3, r4, 1);                      // type 4: (i32,i32,i32)->i64 — div ops
	}
	b.endSection();

	// Import section
	b.emitImportSection(13);
	b.emitImportMemory("env", "memory", 0);
	b.emitImportFunc("env", "read8",   1);
	b.emitImportFunc("env", "read16",  1);
	b.emitImportFunc("env", "read32",  1);
	b.emitImportFunc("env", "write8",  2);
	b.emitImportFunc("env", "write16", 2);
	b.emitImportFunc("env", "write32", 2);
	b.emitImportFunc("env", "ifb",     2);
	b.emitImportFunc("env", "shil_fb", 2);
	b.emitImportFunc("env", "sq_pref", 3);
	b.emitImportFunc("env", "div32u",  4);
	b.emitImportFunc("env", "div32s",  4);
	b.emitImportFunc("env", "div1",    4);
	b.endSection();

	u32 typeIdx = 0;
	b.emitFunctionSection(1, &typeIdx);
	b.emitExportSection("b", WIMPORT_COUNT);

	b.setHintFuncIndexBase(WIMPORT_COUNT);
	b.beginCodeSection(1);
	b.beginFuncBody();

	// Locals: 5 fixed i32 scratch + N cached regs + 1 dispatch index + 1 i64 scratch
	u32 i32Count = LOCAL_FIXED_I32_COUNT + cache.localCount() + 1;
	u32 i64Count = 1;
	u32 groupCounts[2] = { i32Count, i64Count };
	u8 groupTypes[2] = { WASM_TYPE_I32, WASM_TYPE_I64 };
	b.emitLocals(2, groupCounts, groupTypes);
	cache._tmp64LocalIdx = 2 + i32Count;  // i64 local after all i32s

	// Prologue: load all cached registers
	for (auto& [offset, entry] : cache.entries) {
		b.op_local_get(LOCAL_CTX);
		b.op_i32_load(offset);
		b.op_local_set(entry.wasmLocal);
	}

	// Initialize dispatch index = 0 (entry block)
	b.op_i32_const(0);
	b.op_local_set(LOCAL_NEXT_IDX);

#ifndef JIT_PROD_BUILD
	// Dev counter: chain entry (absorption-ratio numerator lives per-link below)
	{
		extern u32 g_fly_chain_enters;
		u32 ceAddr = (u32)(uintptr_t)&g_fly_chain_enters;
		b.op_i32_const((s32)ceAddr);
		b.op_i32_const((s32)ceAddr);
		b.op_i32_load(0);
		b.op_i32_const(1);
		b.op_i32_add();
		b.op_i32_store(0);
	}
#endif

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

		// SMC guard for interior blocks: verify full block bytes haven't
		// changed. Entry block (i==0) is checked by c_dispatch_loop via
		// hashRamBlock. Only guard RAM blocks (area 3: 0x0C/0x8C/0xAC).
		//
		// Inlines a rotl5+xor hash over sh4_code_size bytes, matching the
		// C++ hashRamBlock() used at compile time. Replaces the prior
		// first-opcode-only guard, which missed inner-byte SMC (hazard
		// confirmed by Layer 1b test smc_inner_byte).
		if (i > 0) {
			u32 phys = blk->vaddr & 0x1FFFFFFF;
			if ((phys >> 26) == 3) {
				u32 n_u16 = blk->sh4_code_size / 2;
				if (n_u16 == 0) n_u16 = 1;
				u32 expectedHash = hashRamBlock(blk->vaddr, n_u16);
				u32 ramOffset = phys & RAM_MASK;

				// ★ PAGE-GENERATION GATING (2026-07-18): interiors used to
				// full-hash on EVERY traversal — measured ~500K halfwords/frame
				// on DOA2, 15-20x the (already gen-gated) dispatch path. Same
				// mechanism as c_dispatch_loop: skip the hash while the covered
				// pages' generation sum is unchanged, with a 1/64 forced-hash
				// insurance tick per member (staggered at init). guard_cells ==
				// nullptr (shadow builds) keeps the old always-hash behavior.
				bool gated = (guard_cells != nullptr);
				u32 cellAddr = 0;
				if (gated) {
					u32 pg0 = ramOffset >> 12;
					u32 pg1 = (ramOffset + n_u16 * 2 - 1) >> 12;
					u32 pgAddr0 = (u32)(uintptr_t)&g_fly_page_gen[pg0];
					cellAddr = (u32)(uintptr_t)&guard_cells[i * 2];
#if FLY_SMC_INSURANCE_TICK
					u32 tickAddr = (u32)(uintptr_t)&guard_cells[i * 2 + 1];

					// tick++
					b.op_i32_const((s32)tickAddr);
					b.op_i32_const((s32)tickAddr);
					b.op_i32_load(0);
					b.op_i32_const(1);
					b.op_i32_add();
					b.op_i32_store(0);
#endif

					// TMP2 = current pgen sum over covered pages
					b.op_i32_const((s32)pgAddr0);
					b.op_i32_load(0);
					if (pg1 != pg0) {
						u32 pgAddr1 = (u32)(uintptr_t)&g_fly_page_gen[pg1];
						b.op_i32_const((s32)pgAddr1);
						b.op_i32_load(0);
						b.op_i32_add();
					}
					b.op_local_set(LOCAL_TMP2);

					// need_hash = (TMP2 != cell) [ | ((tick & 63) == 0) when
					// the insurance tick is compiled in ]
					b.op_local_get(LOCAL_TMP2);
					b.op_i32_const((s32)cellAddr);
					b.op_i32_load(0);
					b.op_i32_ne();
#if FLY_SMC_INSURANCE_TICK
					b.op_i32_const((s32)tickAddr);
					b.op_i32_load(0);
					b.op_i32_const(63);
					b.op_i32_and();
					b.op_i32_eqz();
					b.op_i32_or();
#endif
					// ★ BRANCH HINT: need_hash fires on gen change only (tick
					// retired) — rarer than ever.
					b.hintNextBranchUnlikely();
					b.op_if();   // hash only when needed; adds one br depth
				}

				// h = 0
				b.op_i32_const(0);
				b.op_local_set(LOCAL_TMP);

				// Unrolled: for each halfword, h = rotl5(h) ^ ram[ramOffset + h_i*2]
				for (u32 h_i = 0; h_i < n_u16; h_i++) {
					// rotl5(h) = (h << 5) | (h >> 27)
					b.op_local_get(LOCAL_TMP);
					b.op_i32_const(5);
					b.op_i32_shl();
					b.op_local_get(LOCAL_TMP);
					b.op_i32_const(27);
					b.op_i32_shr_u();
					b.op_i32_or();
					// xor ram[ramOffset + h_i*2]
					b.op_local_get(LOCAL_RAM);
					b.op_i32_const((s32)(ramOffset + h_i * 2));
					b.op_i32_add();
					b.op_i32_load16_u(0);
					b.op_i32_xor();
					b.op_local_set(LOCAL_TMP);
				}

#ifndef JIT_PROD_BUILD
				// Dev counter: chain-interior hash workload (invisible to the
				// C-side g_fly_hash_words — this is the WASM-side tax)
				{
					extern u32 g_fly_chain_hash_words;
					u32 chwAddr = (u32)(uintptr_t)&g_fly_chain_hash_words;
					b.op_i32_const((s32)chwAddr);
					b.op_i32_const((s32)chwAddr);
					b.op_i32_load(0);
					b.op_i32_const((s32)n_u16);
					b.op_i32_add();
					b.op_i32_store(0);
				}
#endif

				// If computed hash != expected, br $exit (SMC detected).
				// Inside the gated if, $exit is one level deeper.
				b.op_local_get(LOCAL_TMP);
				b.op_i32_const((s32)expectedHash);
				b.op_i32_ne();
				// ★ BRANCH HINT: SMC hit = block invalidation, rare by design.
				b.hintNextBranchUnlikely();
				b.op_br_if(gated ? 3 : 2);  // br $exit
				if (gated) {
					// Hash matched — commit the new pgen baseline
					b.op_i32_const((s32)cellAddr);
					b.op_local_get(LOCAL_TMP2);
					b.op_i32_store(0);
					b.op_end();   // close the need-hash if
				}
			}
		}

#if FLY_CHAIN_SHADOW
		// Itinerary trace (shadow builds): record this link index so a
		// divergence dump shows exactly where the two paths forked.
		{
			extern u32 g_cs_trace[128];
			extern u32 g_cs_trace_n;
			u32 trAddr = (u32)(uintptr_t)&g_cs_trace[0];
			u32 tnAddr = (u32)(uintptr_t)&g_cs_trace_n;
			b.op_i32_const((s32)tnAddr);
			b.op_i32_load(0);
			b.op_i32_const(128);
			b.op_i32_lt_u();
			b.op_if();
			// g_cs_trace[n] = i
			b.op_i32_const((s32)trAddr);
			b.op_i32_const((s32)tnAddr);
			b.op_i32_load(0);
			b.op_i32_const(4);
			b.op_i32_mul();
			b.op_i32_add();
			b.op_i32_const((s32)i);
			b.op_i32_store(0);
			// g_cs_trace_n++
			b.op_i32_const((s32)tnAddr);
			b.op_i32_const((s32)tnAddr);
			b.op_i32_load(0);
			b.op_i32_const(1);
			b.op_i32_add();
			b.op_i32_store(0);
			b.op_end();
		}
#endif

#ifndef JIT_PROD_BUILD
		// Dev counter: member-block execution inside the chain
		{
			extern u32 g_fly_chain_links;
			u32 clAddr = (u32)(uintptr_t)&g_fly_chain_links;
			b.op_i32_const((s32)clAddr);
			b.op_i32_const((s32)clAddr);
			b.op_i32_load(0);
			b.op_i32_const(1);
			b.op_i32_add();
			b.op_i32_store(0);
		}
#endif

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
#ifndef JIT_PROD_BUILD
				prof_fallback_ops_compiled++;
#endif
				emitFlushAll(b, cache);
				b.op_i32_const((s32)blk->vaddr);
				b.op_i32_const((s32)j);
				b.op_call(WIMPORT_SHIL_FB);
				emitReloadAll(b, cache);

				// Exception-abort: if shil_fb set the pending flag,
				// skip remaining ops and exit the multi-block module.
				// br(2) from inside if-body exits: if → loop → block $exit.
				b.op_i32_const((s32)excFlagAddr);
				b.op_i32_load8_u(0);
				b.op_br_if(2);  // br $exit
			} else {
#ifndef JIT_PROD_BUILD
				prof_native_ops_compiled++;
#endif
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
				// Both targets in chain. ★ ROUTING FIX (2026-07-17, chain
				// campaign): the old else-arm routed to NextBlock WITHOUT
				// verifying ctx.pc — any third pc value silently executed
				// the wrong block (audit suspect, wrong-block state
				// corruption = the temporal freeze class). Every route is
				// now equality-verified; an unmatched pc exits the module
				// so the outer dispatch handles it.
				b.op_local_get(LOCAL_CTX);
				b.op_i32_load(ctx_off::PC);
				b.op_i32_const((s32)blk->BranchBlock);
				b.op_i32_eq();
				b.op_if();  // inner if: $exit=br(3), $dispatch=br(2), outer if=br(1)
				b.op_i32_const((s32)branchTarget->second);
				b.op_local_set(LOCAL_NEXT_IDX);
				b.op_br(2);  // br $dispatch (from inner if: depth 2)
				b.op_end();
				b.op_local_get(LOCAL_CTX);
				b.op_i32_load(ctx_off::PC);
				b.op_i32_const((s32)blk->NextBlock);
				b.op_i32_eq();
				b.op_if();  // inner if
				b.op_i32_const((s32)nextTarget->second);
				b.op_local_set(LOCAL_NEXT_IDX);
				b.op_br(2);  // br $dispatch (from inner if: depth 2)
				b.op_end();
				b.op_br(2);  // br $exit — pc matched neither target
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

	g_emitting_chain = false;
	return true;
}

// ============================================================
// Region module (design v0, 2026-07-18): ONE function, br_table dispatch
// over ALL member blocks of a hot page range. Entry index arrives via the
// g_region_entry_idx linear-memory cell (written by the dispatcher before
// call). ONE page-generation sum guard per loop iteration replaces ALL
// per-member interior SMC guards. Emission mirrors the farm-hardened chain
// builder: unified RegCache, equality-verified COND routing, per-op
// exception-abort contract, unconditional flush on every exit path.
//
// Depth map, body k of N (after block k's end, inside block k+1):
//   enclosing: b_{k+1}(0) .. b_{N-1}(N-2-k), loop(N-1-k), exit(N-k)
//   dispatch = N-1-k    exit = N-k    (+1 inside an if)
// br_table position (inside b0): body j = depth j, default(exit) = N+1.
// ============================================================

u32 g_region_entry_idx = 0;   // dispatcher writes the entry block's index here

#ifndef JIT_PROD_BUILD
// Step-3 scoping (2026-07-21): why do LIVE regions exit? Counters bumped
// IN-MODULE (dev emission only) at each exit path; [REGION-EXIT] prints
// window deltas. The DYN share is step-3's absorbable ceiling; the
// ret/call/jump split sizes shadow-return-stack vs inline-cache benefit.
// 0=cc 1=interrupt 2=guard 3=static-out/self-loop 4=cond-unmatched
// 5=DYN-RET (rts — shadow return stack) 6=DYN-CALL (jsr @rn — IC + ret-hint
// push) 7=DYN-JUMP (jmp @rn — pure IC) 8=other (StaticIntr/DynamicIntr)
// (exception-abort exits stay untagged — rare, shows as entries-minus-sum)
u32 g_region_exit_ct[10];
#endif

static bool buildRegionModule(WasmModuleBuilder& b,
                               const std::vector<RuntimeBlockInfo*>& members,
                               u32 first_page, u32 last_page,
                               u32* gen_baseline_cell,
                               u32* ic_cells, u32 rslot,
                               u32* ras_cells, const u32* pc_table) {
	const u32 N = (u32)members.size();
	if (N < 2 || N > 4096)
		return false;
	(void)first_page; (void)last_page;   // range now guarded via write-watch cells
	u32 excFlagAddr = (u32)(uintptr_t)&g_ifb_exception_pending;
	// pref's inline contract is single-block-only — same fb routing as chains
	g_emitting_chain = true;

#ifndef JIT_PROD_BUILD
	// Bump g_region_exit_ct[reason] in-module (addr twice: store wants
	// [addr, value]; value = load(addr)+1).
	auto emitExitTick = [&](u32 reason) {
		u32 a = (u32)(uintptr_t)&g_region_exit_ct[reason < 10 ? reason : 9];
		b.op_i32_const((s32)a);
		b.op_i32_const((s32)a);
		b.op_i32_load(0);
		b.op_i32_const(1);
		b.op_i32_add();
		b.op_i32_store(0);
	};
	auto emitCellTick = [&](u32* cell) {
		u32 a = (u32)(uintptr_t)cell;
		b.op_i32_const((s32)a);
		b.op_i32_const((s32)a);
		b.op_i32_load(0);
		b.op_i32_const(1);
		b.op_i32_add();
		b.op_i32_store(0);
	};
#endif
	// RAS push: if (sp < 16) { ras[sp] = hintIdx; sp++ }. No branches out —
	// depth-neutral; safe to emit before any routing. A push whose call
	// leaves the region is simply lost at the next entry's sp reset.
	u32 rasSpAddr  = (u32)(uintptr_t)ras_cells;            // [0] = sp
	u32 rasBaseAddr = (u32)(uintptr_t)(ras_cells ? ras_cells + 1 : nullptr);
	auto emitRasPush = [&](u32 hintIdx) {
		b.op_i32_const((s32)rasSpAddr);
		b.op_i32_load(0);
		b.op_i32_const(16);
		b.op_i32_lt_u();
		b.op_if();
		// ras[sp] = hintIdx
		b.op_i32_const((s32)rasBaseAddr);
		b.op_i32_const((s32)rasSpAddr);
		b.op_i32_load(0);
		b.op_i32_const(2);
		b.op_i32_shl();
		b.op_i32_add();
		b.op_i32_const((s32)hintIdx);
		b.op_i32_store(0);
		// sp++
		b.op_i32_const((s32)rasSpAddr);
		b.op_i32_const((s32)rasSpAddr);
		b.op_i32_load(0);
		b.op_i32_const(1);
		b.op_i32_add();
		b.op_i32_store(0);
		b.op_end();
	};

	RegCache cache;
	for (auto* blk : members)
		cache.scanBlock(blk);

	std::unordered_map<u32, u32> pcToIdx;
	for (u32 i = 0; i < N; i++)
		pcToIdx[members[i]->vaddr] = i;

	u32 LOCAL_NEXT_IDX = 2 + LOCAL_FIXED_I32_COUNT + cache.localCount();

	b.emitHeader();
	b.emitTypeSection(5);
	{
		u8 p0[] = { WASM_TYPE_I32, WASM_TYPE_I32 };
		b.emitFuncType(p0, 2, nullptr, 0);
		u8 p1[] = { WASM_TYPE_I32 };
		u8 r1[] = { WASM_TYPE_I32 };
		b.emitFuncType(p1, 1, r1, 1);
		u8 p2[] = { WASM_TYPE_I32, WASM_TYPE_I32 };
		b.emitFuncType(p2, 2, nullptr, 0);
		u8 p3[] = { WASM_TYPE_I32 };
		b.emitFuncType(p3, 1, nullptr, 0);
		u8 p4[] = { WASM_TYPE_I32, WASM_TYPE_I32, WASM_TYPE_I32 };
		u8 r4[] = { WASM_TYPE_I64 };
		b.emitFuncType(p4, 3, r4, 1);
	}
	b.endSection();
	b.emitImportSection(13);
	b.emitImportMemory("env", "memory", 0);
	b.emitImportFunc("env", "read8",   1);
	b.emitImportFunc("env", "read16",  1);
	b.emitImportFunc("env", "read32",  1);
	b.emitImportFunc("env", "write8",  2);
	b.emitImportFunc("env", "write16", 2);
	b.emitImportFunc("env", "write32", 2);
	b.emitImportFunc("env", "ifb",     2);
	b.emitImportFunc("env", "shil_fb", 2);
	b.emitImportFunc("env", "sq_pref", 3);
	b.emitImportFunc("env", "div32u",  4);
	b.emitImportFunc("env", "div32s",  4);
	b.emitImportFunc("env", "div1",    4);
	b.endSection();
	u32 typeIdx = 0;
	b.emitFunctionSection(1, &typeIdx);
	b.emitExportSection("b", WIMPORT_COUNT);
	b.setHintFuncIndexBase(WIMPORT_COUNT);
	b.beginCodeSection(1);
	b.beginFuncBody();

	u32 i32Count = LOCAL_FIXED_I32_COUNT + cache.localCount() + 1;
	u32 i64Count = 1;
	u32 groupCounts[2] = { i32Count, i64Count };
	u8 groupTypes[2] = { WASM_TYPE_I32, WASM_TYPE_I64 };
	b.emitLocals(2, groupCounts, groupTypes);
	cache._tmp64LocalIdx = 2 + i32Count;

	// Prologue: unified register loads
	for (auto& [offset, entry] : cache.entries) {
		b.op_local_get(LOCAL_CTX);
		b.op_i32_load(offset);
		b.op_local_set(entry.wasmLocal);
	}
	// Entry index from the dispatcher's cell
	{
		u32 eiAddr = (u32)(uintptr_t)&g_region_entry_idx;
		b.op_i32_const((s32)eiAddr);
		b.op_i32_load(0);
		b.op_local_set(LOCAL_NEXT_IDX);
	}
	// RAS sp reset — every invocation starts with an empty shadow stack
	// (no cross-entry state; exception aborts can never leak stale routing)
	if (ras_cells) {
		b.op_i32_const((s32)rasSpAddr);
		b.op_i32_const(0);
		b.op_i32_store(0);
	}

	b.op_block();   // $exit
	b.op_loop();    // $dispatch

	// Cycle counter check → exit(1)
	b.op_local_get(LOCAL_CTX);
	b.op_i32_load(ctx_off::CYCLE_COUNTER);
	b.op_i32_const(0);
	b.op_i32_le_s();
#ifndef JIT_PROD_BUILD
	b.op_if(); emitExitTick(0); b.op_br(2); b.op_end();
#else
	b.op_br_if(1);
#endif
	// Interrupt check → exit(1)
	b.op_local_get(LOCAL_CTX);
	b.op_i32_load(0x16C);
#ifndef JIT_PROD_BUILD
	b.op_if(); emitExitTick(1); b.op_br(2); b.op_end();
#else
	b.op_br_if(1);
#endif
	// Range write guard per iteration → exit(1) on change. Exit semantics
	// identical to the v0 8-page gen-sum, but the sum is precomputed on the
	// C write path (fly_ram_written bumps gen_cell[1] on in-range writes),
	// so the guard is cur != baseline: 2 loads instead of 8 loads + 7 adds.
	// This guard-cost fix alone flipped the matched DOA2 dev A/B from
	// emu_ms +5.6% (v0, net-negative) to -4.9% (2026-07-19).
	// C-side maintenance revalidates members and updates the baseline cell.
	{
		u32 curAddr  = (u32)(uintptr_t)(gen_baseline_cell + 1);
		u32 cellAddr = (u32)(uintptr_t)gen_baseline_cell;
		b.op_i32_const((s32)curAddr);
		b.op_i32_load(0);
		b.op_i32_const((s32)cellAddr);
		b.op_i32_load(0);
		b.op_i32_ne();
#ifndef JIT_PROD_BUILD
		b.op_if(); emitExitTick(2); b.op_br(2); b.op_end();
#else
		b.op_br_if(1);
#endif
	}

	// N nested blocks + br_table
	for (u32 k = 0; k < N; k++)
		b.op_block();
	b.op_local_get(LOCAL_NEXT_IDX);
	{
		std::vector<u32> depths(N);
		for (u32 j = 0; j < N; j++) depths[j] = j;
		b.op_br_table(depths.data(), N, N + 1);   // default → $exit
	}

	for (u32 k = 0; k < N; k++) {
		b.op_end();   // close block k → body k position
		RuntimeBlockInfo* blk = members[k];
		u32 D_DISP = N - 1 - k;
		u32 D_EXIT = N - k;

		for (auto& [offset, entry] : cache.entries)
			entry.dirty = true;

		// cycle_counter -= guest_cycles
		b.op_local_get(LOCAL_CTX);
		b.op_local_get(LOCAL_CTX);
		b.op_i32_load(ctx_off::CYCLE_COUNTER);
		b.op_i32_const((s32)blk->guest_cycles);
		b.op_i32_sub();
		b.op_i32_store(ctx_off::CYCLE_COUNTER);

		for (u32 j = 0; j < blk->oplist.size(); j++) {
			shil_opcode& op = blk->oplist[j];
			if (!emitShilOp(b, op, blk, j, cache)) {
				emitFlushAll(b, cache);
				b.op_i32_const((s32)blk->vaddr);
				b.op_i32_const((s32)j);
				b.op_call(WIMPORT_SHIL_FB);
				emitReloadAll(b, cache);
				b.op_i32_const((s32)excFlagAddr);
				b.op_i32_load8_u(0);
				b.op_br_if(D_EXIT);
			}
		}

		emitBlockExit(b, blk, cache);

		u32 bcls_blk = BET_GET_CLS(blk->BlockType);
		if (bcls_blk == BET_CLS_Static && blk->BlockType != BET_StaticIntr) {
			// RAS push at static calls whose return hint lands in-region
			// (decoder: BET_StaticCall → NextBlock is the ret hint)
			if (ras_cells && blk->BlockType == BET_StaticCall) {
				auto rh = pcToIdx.find(blk->NextBlock);
				if (rh != pcToIdx.end())
					emitRasPush(rh->second);
			}
			auto target = pcToIdx.find(blk->BranchBlock);
			if (target != pcToIdx.end() && blk->BranchBlock != blk->vaddr) {
				b.op_i32_const((s32)target->second);
				b.op_local_set(LOCAL_NEXT_IDX);
				b.op_br(D_DISP);
			} else {
				// self-loops exit (no soft-FF inside regions, v1) — outer
				// dispatch applies the idle cap; out-of-region also exits
#ifndef JIT_PROD_BUILD
				emitExitTick(3);
#endif
				b.op_br(D_EXIT);
			}
		} else if (bcls_blk == BET_CLS_COND) {
			auto branchTarget = pcToIdx.find(blk->BranchBlock);
			auto nextTarget = pcToIdx.find(blk->NextBlock);
			bool selfB = (blk->BranchBlock == blk->vaddr);
			bool selfN = (blk->NextBlock == blk->vaddr);
			if (branchTarget != pcToIdx.end() && !selfB) {
				b.op_local_get(LOCAL_CTX);
				b.op_i32_load(ctx_off::PC);
				b.op_i32_const((s32)blk->BranchBlock);
				b.op_i32_eq();
				b.op_if();
				b.op_i32_const((s32)branchTarget->second);
				b.op_local_set(LOCAL_NEXT_IDX);
				b.op_br(D_DISP + 1);
				b.op_end();
			}
			if (nextTarget != pcToIdx.end() && !selfN) {
				b.op_local_get(LOCAL_CTX);
				b.op_i32_load(ctx_off::PC);
				b.op_i32_const((s32)blk->NextBlock);
				b.op_i32_eq();
				b.op_if();
				b.op_i32_const((s32)nextTarget->second);
				b.op_local_set(LOCAL_NEXT_IDX);
				b.op_br(D_DISP + 1);
				b.op_end();
			}
#ifndef JIT_PROD_BUILD
			emitExitTick(4);
#endif
			b.op_br(D_EXIT);   // unmatched pc (or self-loop arm) exits
		} else {
			// Step-3 stage 2: RAS push at dynamic calls whose return hint
			// lands in-region (decoder: BET_DynamicCall → NextBlock is the
			// ret hint). Emitted before the IC probe so the hint is pushed
			// whether the call routes in-region or exits.
			if (ras_cells && blk->BlockType == BET_DynamicCall) {
				auto rh = pcToIdx.find(blk->NextBlock);
				if (rh != pcToIdx.end())
					emitRasPush(rh->second);
			}
			// Step-3 stage 2: RAS pop at rts. Pop is a PREDICTION —
			// verified against ctx.pc via the baked pc_table before
			// routing (chain/COND routing law); a stale or foreign entry
			// can only mispredict into the normal exit, never misexecute.
			if (ras_cells && pc_table && blk->BlockType == BET_DynamicRet) {
				u32 pcTabAddr = (u32)(uintptr_t)pc_table;
				b.op_i32_const((s32)rasSpAddr);
				b.op_i32_load(0);
				b.op_if();                       // sp != 0 → +1 depth
				// sp--
				b.op_i32_const((s32)rasSpAddr);
				b.op_i32_const((s32)rasSpAddr);
				b.op_i32_load(0);
				b.op_i32_const(1);
				b.op_i32_sub();
				b.op_i32_store(0);
				// LOCAL_NEXT_IDX = ras[sp]
				b.op_i32_const((s32)rasBaseAddr);
				b.op_i32_const((s32)rasSpAddr);
				b.op_i32_load(0);
				b.op_i32_const(2);
				b.op_i32_shl();
				b.op_i32_add();
				b.op_i32_load(0);
				b.op_local_set(LOCAL_NEXT_IDX);
				// verify pc_table[idx] == ctx.pc → route
				b.op_i32_const((s32)pcTabAddr);
				b.op_local_get(LOCAL_NEXT_IDX);
				b.op_i32_const(2);
				b.op_i32_shl();
				b.op_i32_add();
				b.op_i32_load(0);
				b.op_local_get(LOCAL_CTX);
				b.op_i32_load(ctx_off::PC);
				b.op_i32_eq();
				b.op_if();                       // match → +2 depth
#ifndef JIT_PROD_BUILD
				emitCellTick(&g_region_ras_ct[0]);
#endif
				b.op_br(D_DISP + 2);
				b.op_end();
#ifndef JIT_PROD_BUILD
				emitCellTick(&g_region_ras_ct[1]);
#endif
				b.op_end();
				// fall through to the normal exit (empty stack or mispredict)
			}
			// Step-3 stage 1: monomorphic IC at dyn call/jump sites.
			// emitBlockExit already stored ctx.pc = jdyn; the probe only
			// ROUTES on pc equality (chain/COND routing law) — a stale
			// pair can only mispredict into the normal exit, never
			// misexecute. cached_pc starts 0 (never a valid pc) → first
			// dispatch misses, mailbox posts, C fills, next dispatch hits.
			if (ic_cells
			    && (blk->BlockType == BET_DynamicCall
			        || blk->BlockType == BET_DynamicJump)) {
				u32 pcAddr  = (u32)(uintptr_t)&ic_cells[k * 2];
				u32 idxAddr = (u32)(uintptr_t)&ic_cells[k * 2 + 1];
				b.op_i32_const((s32)pcAddr);
				b.op_i32_load(0);
				b.op_local_get(LOCAL_CTX);
				b.op_i32_load(ctx_off::PC);
				b.op_i32_eq();
				b.op_if();
#ifndef JIT_PROD_BUILD
				{
					u32 h = (u32)(uintptr_t)&g_region_ic_ct[0];
					b.op_i32_const((s32)h);
					b.op_i32_const((s32)h);
					b.op_i32_load(0);
					b.op_i32_const(1);
					b.op_i32_add();
					b.op_i32_store(0);
				}
#endif
				b.op_i32_const((s32)idxAddr);
				b.op_i32_load(0);
				b.op_local_set(LOCAL_NEXT_IDX);
				b.op_br(D_DISP + 1);
				b.op_end();
				// miss: post the mailbox for the C-side fill
				b.op_i32_const((s32)(uintptr_t)&g_region_dyn_miss);
				b.op_i32_const((s32)((rslot << 16) | k));
				b.op_i32_store(0);
#ifndef JIT_PROD_BUILD
				{
					u32 m = (u32)(uintptr_t)&g_region_ic_ct[1];
					b.op_i32_const((s32)m);
					b.op_i32_const((s32)m);
					b.op_i32_load(0);
					b.op_i32_const(1);
					b.op_i32_add();
					b.op_i32_store(0);
				}
#endif
			}
#ifndef JIT_PROD_BUILD
			emitExitTick(blk->BlockType == BET_DynamicRet  ? 5
			           : blk->BlockType == BET_DynamicCall ? 6
			           : blk->BlockType == BET_DynamicJump ? 7 : 8);
#endif
			b.op_br(D_EXIT);   // dynamic / StaticIntr / other
		}
	}

	b.op_end();   // end loop $dispatch
	b.op_end();   // end block $exit

	emitFlushAllUnconditional(b, cache);
	b.endFuncBody();
	b.endSection();
	g_emitting_chain = false;
	return true;
}

// ============================================================
// Region pipeline tick — discovery, plateau/lock, registration.
// ★ EXTRACTED from the dev telemetry window (2026-07-21): the window is
// compiled out of JIT_PROD_BUILD, so PROD builds with FLY_REGIONS_LIVE=1
// never built a single region — both eyes builds were no-ops and the
// felt verdicts were void. This function is PROD-ACTIVE (called at window
// cadence from the mainloop when regions are enabled); all logging inside
// stays dev-gated.
// ============================================================
static void fly_region_pipeline_tick()
{
				{
					// Region discovery dry-run: what would we compile right now?
					static u32 pe_prev[4096];
					u32 wdelta[4096];
					for (int pi = 0; pi < 4096; pi++) {
						wdelta[pi] = g_fly_page_execs[pi] - pe_prev[pi];
						pe_prev[pi] = g_fly_page_execs[pi];
					}
					FlyRegionCandidate rc = fly_discover_region(wdelta);
					if (rc.first_page != 0xFFFFFFFF) {
						u32 members = 0;
						for (auto& kv : blockByVaddr) {
							u32 bpg = (kv.first & 0x1FFFFFFF & RAM_MASK) >> 12;
							if (bpg >= rc.first_page && bpg <= rc.last_page) members++;
						}
#ifndef JIT_PROD_BUILD
						EM_ASM({ console.log('[REGION-SCOPE] range=0x' + $0.toString(16) +
							'-0x' + $1.toString(16) +
							' pages=' + ($1 - $0 + 1) +
							' member_blocks=' + $2 +
							' coverage_pct=' + ($4 ? ($3 * 100 / $4 | 0) : 0)); },
							rc.first_page, rc.last_page, members,
							(double)rc.covered, (double)rc.total);
#endif
						// DRY-COMPILE (dark launch): build + validate the region
						// module through the browser's wasm validator, then
						// discard. Never primed, never executed — emission
						// correctness (depths/types) proven with zero game risk.
						// Re-fire whenever the member set has grown 2x past the
						// last validated size (first run fired at 116 of 551 —
						// the working set was still decoding).
						// ★ MULTI-REGION PLATEAU (2026-07-23): real fight content
						// spans MULTIPLE page clusters (DOA2 attract: 0xfd-0x101
						// + more; measured coverage 35-68% under single-lock
						// newest-wins). Track plateaus PER RANGE in a small
						// table; a range registers when ITS plateau stabilizes
						// (±10% x3 windows, farm-run-8 law unchanged). Discovery
						// is self-interleaving: a registered cluster's counted
						// heat collapses ~20x (regions absorb dispatches), so
						// the next window's candidate is the next uncovered
						// cluster — the old flip-flop, harvested. Retirement is
						// OVERLAP-ONLY (see below), live regions capped at 3.
						struct FlyRegTrack {
							u32 rkey = 0;
							u32 prev = 0;
							u32 stable = 0;
							u32 last_size = 0;
							u32 miss_streak = 0;
						};
						static FlyRegTrack fly_tracks[4];
						u32 fly_rkey = (rc.first_page << 16) | rc.last_page;
						FlyRegTrack* trk = nullptr;
						for (auto& t : fly_tracks)
							if (t.rkey == fly_rkey) { trk = &t; break; }
						if (!trk) {
							// claim an empty slot, else evict the stalest track
							for (auto& t : fly_tracks)
								if (t.rkey == 0) { trk = &t; break; }
							if (!trk) {
								trk = &fly_tracks[0];
								for (auto& t : fly_tracks)
									if (t.miss_streak > trk->miss_streak) trk = &t;
							}
							*trk = FlyRegTrack();
							trk->rkey = fly_rkey;
						}
						// age every other track; refresh this one
						for (auto& t : fly_tracks)
							if (t.rkey != 0 && &t != trk)
								t.miss_streak++;
						trk->miss_streak = 0;
						{
							u32 mlo = trk->prev - trk->prev / 10;
							u32 mhi = trk->prev + trk->prev / 10;
							if (trk->prev > 0 && (u32)members >= mlo && (u32)members <= mhi)
								trk->stable++;
							else
								trk->stable = 0;
							trk->prev = (u32)members;
						}
						bool fly_range_hit = true;   // per-track: candidate is its own lock
						u32 fly_reg_stable = trk->stable;
						u32 fly_reg_prev = trk->prev;
						u32 fly_reg_range = fly_rkey;
						u32 fly_reg_last_size = trk->last_size;
#ifndef JIT_PROD_BUILD
						EM_ASM({ console.log('[REG-STATE] rkey=0x' + ($0>>>0).toString(16) +
							' locked=0x' + ($1>>>0).toString(16) + ' hit=' + $2 +
							' stable=' + $3 + ' prev=' + $4 + ' members=' + $5 +
							' last=' + $6 + ' cov=' + $7); },
							fly_rkey, fly_reg_range, fly_range_hit ? 1 : 0,
							fly_reg_stable, fly_reg_prev, (u32)members,
							fly_reg_last_size,
							rc.total ? (u32)(rc.covered * 100 / rc.total) : 0);
#endif
#if (FLY_REGIONS_LIVE || FLY_REGION_SHADOW) && !defined(JIT_PROD_BUILD)
						{
							// Step-3 scoping: exit-reason deltas per report window.
							static u32 re_last[10];
							u32 red[9];
							for (int ri = 0; ri < 9; ri++) {
								red[ri] = g_region_exit_ct[ri] - re_last[ri];
								re_last[ri] = g_region_exit_ct[ri];
							}
							EM_ASM({ console.log('[REGION-EXIT] cc=' + $0 + ' int=' + $1 +
								' guard=' + $2 + ' static=' + $3 + ' cond=' + $4 +
								' RET=' + $5 + ' CALL=' + $6 + ' JMP=' + $7 +
								' other=' + $8); },
								red[0], red[1], red[2], red[3], red[4], red[5],
								red[6], red[7], red[8]);
							static u32 ic_last[2], ras_last[2];
							u32 ich = g_region_ic_ct[0] - ic_last[0];
							u32 icm = g_region_ic_ct[1] - ic_last[1];
							u32 rah = g_region_ras_ct[0] - ras_last[0];
							u32 ram = g_region_ras_ct[1] - ras_last[1];
							ic_last[0] = g_region_ic_ct[0];
							ic_last[1] = g_region_ic_ct[1];
							ras_last[0] = g_region_ras_ct[0];
							ras_last[1] = g_region_ras_ct[1];
							EM_ASM({ console.log('[REGION-DYN] ic_hit=' + $0 +
								' ic_miss=' + $1 + ' ic_rate=' +
								($0 + $1 ? (100 * $0 / ($0 + $1)).toFixed(1) : '0') +
								'% ras_hit=' + $2 + ' ras_mispred=' + $3 + ' ras_rate=' +
								($2 + $3 ? (100 * $2 / ($2 + $3)).toFixed(1) : '0') + '%'); },
								ich, icm, rah, ram);
						}
#endif
						// Same-content guard: if a live region already covers this
						// exact range with a member count within 10%, re-registering
						// buys nothing (a track eviction resets last_size, which
						// would otherwise churn identical rebuilds).
						bool fly_dup_live = false;
						u32 fly_live_regions = 0;
						for (const auto& lr : g_regions) {
							if (lr.table_idx == 0) continue;
							fly_live_regions++;
							if (lr.first_page == rc.first_page && lr.last_page == rc.last_page
							    && (u32)lr.members.size() + (u32)lr.members.size() / 10 >= (u32)members
							    && (u32)members + (u32)members / 10 >= (u32)lr.members.size())
								fly_dup_live = true;
						}
						// Overlap-only retirement: count how many live regions this
						// candidate would retire; the post-registration live count
						// must stay within the cap of 3.
						u32 fly_overlap_count = 0;
						for (const auto& lr : g_regions)
							if (lr.table_idx != 0
							    && !(rc.last_page < lr.first_page || rc.first_page > lr.last_page))
								fly_overlap_count++;
						bool fly_cap_ok = (fly_live_regions - fly_overlap_count) < 3;
						// Coverage gate relaxes once regions are live: registered
						// clusters' counted heat collapses ~20x, so later
						// candidates only ever see a diffuse RESIDUAL histogram —
						// 60% share is unreachable there (measured: a 1036-member
						// plateau stuck 28 windows at cov 33-37). Absolute heat
						// still gates via members>=100 + plateau stability.
						if (fly_range_hit && !fly_dup_live && fly_cap_ok
						    && members >= 100 && fly_reg_stable >= 3
						    && ((u32)members > fly_reg_last_size + fly_reg_last_size / 10
						        || fly_reg_last_size == 0)
						    && rc.total > 0
						    && rc.covered * 100 / rc.total >= (fly_live_regions > 0 ? 25 : 60)) {
							trk->last_size = (u32)members;
							for (u32 ori = 0; ori < (u32)g_regions.size(); ori++) {
								RegionInfo& fr2 = g_regions[ori];
								if (fr2.table_idx != 0
								    && !(rc.last_page < fr2.first_page || rc.first_page > fr2.last_page))
									fly_region_invalidate(ori);
							}
							// Membership filters (chain laws): no idle self-loops
							// (no in-region soft-FF), no pref/SQ blocks
							// (unvalidatable by differential), decode-freshness
							// (never bake a stale decode).
							std::vector<RuntimeBlockInfo*> mem;
							u32 f_idle = 0, f_pref = 0, f_stale = 0;
							for (auto& kv : blockByVaddr) {
								u32 bpg = (kv.first & 0x1FFFFFFF & RAM_MASK) >> 12;
								if (bpg < rc.first_page || bpg > rc.last_page)
									continue;
								RuntimeBlockInfo* rb2 = kv.second;
								u32 bc2 = BET_GET_CLS(rb2->BlockType);
								bool idle2 = (bc2 == BET_CLS_Static
								      && rb2->BlockType != BET_StaticIntr
								      && rb2->BranchBlock == kv.first)
								    || (bc2 == BET_CLS_COND
								      && (rb2->BranchBlock == kv.first
								          || rb2->NextBlock == kv.first));
								if (idle2) { f_idle++; continue; }
#if !FLY_REGIONS_LIVE
								// SHADOW-ONLY exclusion: the differential cannot
								// validate SQ device state (chain-farm law). LIVE
								// regions INCLUDE pref members — their emission
								// rides the chain-validated fb-pref contract
								// (g_emitting_chain) + eyes.
								bool haspref2 = false;
								for (auto& sop2 : rb2->oplist)
									if (sop2.op == shop_pref) { haspref2 = true; break; }
								if (haspref2) { f_pref++; continue; }
#endif
								auto smc2 = g_block_smc.find(kv.first);
								if (smc2 == g_block_smc.end()) { f_stale++; continue; }
								if (smc2->second.sz != 0
								    && hashRamBlock(kv.first, smc2->second.sz) != smc2->second.hash)
									{ f_stale++; continue; }
								mem.push_back(rb2);
							}
#ifndef JIT_PROD_BUILD
							EM_ASM({ console.log('[REGION-FILTER] raw=' + $0 +
								' kept=' + $1 + ' idle=' + $2 + ' pref=' + $3 +
								' stale=' + $4); },
								(u32)members, (u32)mem.size(), f_idle, f_pref, f_stale);
#endif
							std::sort(mem.begin(), mem.end(),
								[](RuntimeBlockInfo* x, RuntimeBlockInfo* y) { return x->vaddr < y->vaddr; });
							u32* cell = (u32*)calloc(2, 4);   // [0]=baseline [1]=cur (write-watch)
							// IC pairs: one {cached_pc, cached_idx} per member site
							u32* icc = (u32*)calloc(mem.size() * 2, 4);
							// RAS ([0]=sp, [1..16]) + idx→vaddr table for pop verify
							u32* rasc = (u32*)calloc(17, 4);
							u32* pctab = (u32*)calloc(mem.size(), 4);
							if (pctab)
								for (u32 pi = 0; pi < (u32)mem.size(); pi++)
									pctab[pi] = mem[pi]->vaddr;
							WasmModuleBuilder rb;
							double rt0 = emscripten_get_now();
							bool built = buildRegionModule(rb, mem, rc.first_page, rc.last_page, cell,
								icc, (u32)g_regions.size(), rasc, pctab);
							double buildMs = emscripten_get_now() - rt0;
							if (built) {
								const auto& rbytes = rb.getBytes();
								u32 rkey = 0xFFFF0000u + (u32)g_regions.size();
								double ct0 = emscripten_get_now();
								int ridx = wasm_compile_chain(rbytes.data(), (u32)rbytes.size(), rkey);
								double compMs = emscripten_get_now() - ct0;
#ifndef JIT_PROD_BUILD
								EM_ASM({ console.log('[REGION-COMPILE] ok=' + $0 + ' members=' + $1 +
									' bytes=' + $2 + ' build_ms=' + (+$3.toFixed(1)) +
									' compile_ms=' + (+$4.toFixed(1))); },
									ridx > 0 ? 1 : 0, (u32)mem.size(), (u32)rbytes.size(),
									buildMs, compMs);
#endif
#if FLY_REGIONS_LIVE || FLY_REGION_SHADOW
								if (ridx > 0) {
									// Register + index members. LIVE: prime heads
									// with their dense entry index. SHADOW: clear
									// member slots instead — their next dispatch
									// misses into the differential hook. Baseline
									// and cur cells start equal (0); the write
									// watch bumps cur from registration onward
									// (members freshness-checked just above).
									RegionInfo rinfo;
									rinfo.table_idx = (u32)ridx;
									rinfo.first_page = rc.first_page;
									rinfo.last_page = rc.last_page;
									rinfo.gen_cell = cell;
									rinfo.ic_cells = icc;
									rinfo.ras_cells = rasc;
									rinfo.pc_table = pctab;
									u32 rslot = (u32)g_regions.size();
									for (u32 mi = 0; mi < (u32)mem.size(); mi++) {
										rinfo.members.push_back({ mem[mi]->vaddr, mem[mi],
											(u32)mem[mi]->oplist.size(), mem[mi]->guest_cycles,
											(u32)mem[mi]->BlockType });
										g_region_member_of[mem[mi]->vaddr] = rslot + 1;
#if FLY_REGIONS_LIVE
										primeDispatchEntry(mem[mi]->vaddr,
											mem[mi]->sh4_code_size, (u32)ridx, mi);
#else
										u32 mk = (mem[mi]->vaddr >> 1) & JIT_TABLE_MASK;
										if (jit_dispatch_pc[mk] == mem[mi]->vaddr) {
											jit_dispatch_table[mk] = 0; jit_dispatch_pc[mk] = 0;
											jit_dispatch_hash[mk] = 0; jit_dispatch_sz[mk] = 0;
											jit_dispatch_arg[mk] = 0;
										}
#endif
									}
									g_regions.push_back(std::move(rinfo));
									cell = nullptr;   // ownership moved
									icc = nullptr;    // ownership moved
									rasc = nullptr;   // ownership moved
									pctab = nullptr;  // ownership moved
									fly_region_watch_rebuild();
								}
#else
								if (ridx > 0)
									wasm_remove_chain(rkey);   // dry: validate + discard
#endif
							} else {
#ifndef JIT_PROD_BUILD
								EM_ASM({ console.log('[REGION-COMPILE] build FAILED members=' + $0); },
									(u32)mem.size());
#endif
							}
							free(cell);
							free(icc);
							free(rasc);
							free(pctab);
						}
					}
				}
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
#if defined(__EMSCRIPTEN__) && FLY_RELEASE_BUILD
		// Release artifact: JS-side console-quiet flag for the EM_JS bodies
		// (preprocessor cannot strip lines inside EM_JS string bodies).
		EM_ASM({ Module._flyQ = 1; });
#endif
#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
		EM_ASM({ console.log('[rec_wasm] WasmDynarec::init() — Phase 2 WASM JIT'); });
#endif
		fly_init();
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

		#if FLY_CHAINS_ANY
		// Member being replaced — owning chains are now stale. Kill them
		// BEFORE the new registration (the shadow-farm-proven invariant).
		fly_chain_invalidate(block->vaddr);
		#endif
		blockByVaddr[block->vaddr] = block;

		// SMC fingerprint — full-block hash stored via primeDispatchEntry
		// below, after table_idx is known. No placeholder write here.

#if EXECUTOR_MODE == 6 || EXECUTOR_MODE == 7 || EXECUTOR_MODE == 8
		// Build WASM modules when using WASM execution or JIT-vs-ref shadow.
		// Mode 8 (HYBRID) also needs real WASM blocks — it executes them via
		// wasm_execute_block through the cpp dispatch loop (c_dispatch_loop bypassed).
		WasmModuleBuilder builder;

		// Try multi-block: chain statically-connected blocks.
		// Interior blocks have inline SMC guards (direct RAM read + compare).
		//
		// Mode 7 shadow comparison must use SINGLE-block compilation:
		// wasm_execute_block runs the whole multi-block module (N iterations
		// with chain loop-back), while wasm_exec_shil_fb only runs ONE block's
		// ops. The iteration-count mismatch produces false-positive divergences.
#if EXECUTOR_MODE == 7 || 1  // SPOT-CHECK: force single-block (chaining off) — verify games run crash-free
		auto chain = std::vector<RuntimeBlockInfo*>{};
		buildBlockModule(builder, block);
#else
		auto chain = discoverChain(block);
		if (chain.size() >= 2) {
			buildMultiBlockModule(builder, chain);
#ifndef JIT_PROD_BUILD
			prof_multiblock_modules++;
			prof_multiblock_total_blocks += (u32)chain.size();
#endif
		} else {
			buildBlockModule(builder, block);
		}
#endif

		const auto& bytes = builder.getBytes();
		if (g_compile_defer) {
			// Over the per-frame compile budget: hand the bytes to the async
			// compiler and skip the synchronous WebAssembly.compile. The block
			// is registered in blockByVaddr (above) with no WASM behind it, so
			// dispatch runs it via the SHIL fallback until the promise
			// resolves and drainCompileQueue() promotes it.
			u32 smc_hash = 0, smc_nw = 0;
			u32 phys = block->vaddr & 0x1FFFFFFF;
			if ((phys >> 26) == 3) {
				u32 nw = block->sh4_code_size / 2;
				if (nw == 0) nw = 1;
				if (nw > 0xFFFF) nw = 0xFFFF;
				smc_nw = nw;
				smc_hash = hashRamBlock(block->vaddr, nw);
#ifndef JIT_PROD_BUILD
				fly_track_code(block->vaddr, smc_hash);
#endif
				// Deferred blocks never reach primeDispatchEntry, but the
				// MISS-SMC check keys off g_block_smc — without this entry a
				// deferred block is invisible to ALL SMC machinery and keeps
				// executing stale decoded SHIL after the game overwrites its
				// code (wedged JGR at a load transition in budget-test run).
				g_block_smc[block->vaddr] = { smc_hash, (u16)smc_nw };
			}
#if FLY_BRIDGE_SHADOW || FLY_WRITE_PARITY
			// Shadow build: compile synchronously but DON'T prime the table —
			// the block's first found-miss dispatch runs the differential
			// (both executables exist, block still routed through the miss
			// path), then primes. No async (a drain-side promotion would
			// prime before the shadow could run). Write-parity uses the
			// identical interception mechanism.
			wasm_compile_block(bytes.data(), (u32)bytes.size(), block->vaddr);
#else
			if (g_compile_batch) {
				// Queue for this frame's single multi-function Module —
				// flushBlockBatch() re-emits the body and primes. The block
				// bridges until then. (The bytes built above are discarded;
				// byte-build is the cheap part.)
				g_batch_blocks.push_back({ block->vaddr, block });
				g_batch_pending.insert(block->vaddr);
			} else {
				wasm_compile_block_async(bytes.data(), (u32)bytes.size(),
				                         block->vaddr, smc_hash, smc_nw);
			}
#endif
			g_defer_count++;
		} else {
			double fly_compile_t0 = emscripten_get_now();
			int table_idx = wasm_compile_block(bytes.data(), (u32)bytes.size(), block->vaddr);
			double fly_compile_ms = emscripten_get_now() - fly_compile_t0;
			FLY_EVT(FLY_EVT_BLOCK_COMPILE,
			        block->vaddr,
			        (u32)bytes.size(),
			        (u32)(fly_compile_ms * 1000.0),
			        (u32)chain.size());

			if (table_idx > 0) {
				// Prime dispatch metadata — table_idx, pc guard, full-block hash + size
				primeDispatchEntry(block->vaddr, block->sh4_code_size, (u32)table_idx);
				compiledCount++;
#ifndef JIT_PROD_BUILD
				{
					u32 phys = block->vaddr & 0x1FFFFFFF;
					if ((phys >> 26) == 3) {
						u32 nw = block->sh4_code_size / 2;
						if (nw == 0) nw = 1;
						if (nw > 0xFFFF) nw = 0xFFFF;
						fly_track_code(block->vaddr, hashRamBlock(block->vaddr, nw));
					}
				}
#endif
// (chain discovery lives solely in the mainloop sweep driver)
			} else {
				failCount++;
			}
		}
#else
		compiledCount++;
#endif

		// Dummy code pointer for block manager (4 bytes per block)
		block->code = (DynarecCodeEntryPtr)codeBuffer->get();
		block->host_code_size = 4;
		if (codeBuffer->getFreeSpace() >= 4)
			codeBuffer->advance(4);

#if defined(__EMSCRIPTEN__) && !FLY_RELEASE_BUILD
		// Always log first few compilations + periodic updates (even in prod)
		if (compiledCount <= 5 || (compiledCount % 500 == 0)) {
			EM_ASM({ console.log('[rec_wasm] compiled=' + $0 + ' fail=' + $1 +
				' pc=0x' + ($2>>>0).toString(16) + ' ops=' + $3); },
				compiledCount, failCount, block->vaddr,
				(int)block->oplist.size());
		}
#endif
	}

	// Promote async-compiled blocks: pull resolved entries from JS
	// (Module._flyReady) and prime the dispatch table, dropping any whose
	// RuntimeBlockInfo was evicted or whose RAM changed since decode.
	// Promotion is instantiation-rate-bound (off-thread), never budget-bound.
	void drainCompileQueue()
	{
#ifdef __EMSCRIPTEN__
		u32 promoted = 0;
		for (;;) {
			int idx = EM_ASM_INT({
				var q = Module._flyReady;
				if (!q || q.length === 0) return -1;
				var e = q.shift();
				Module._flyRpc = e.pc;
				Module._flyRhash = e.hash;
				Module._flyRnw = e.nw;
				return e.idx;
			});
			if (idx <= 0)
				break;
			u32 pc   = (u32)EM_ASM_INT({ return Module._flyRpc | 0; });
			u32 hash = (u32)EM_ASM_INT({ return Module._flyRhash | 0; });
			u32 nw   = (u32)EM_ASM_INT({ return Module._flyRnw | 0; });
			if (blockByVaddr.find(pc) == blockByVaddr.end()
			    || (nw > 0 && hashRamBlock(pc, nw) != hash)) {
				// Evicted or stale since decode — purge the JS cache entry so
				// the normal miss/recompile machinery takes over.
				wasm_remove_block(pc);
				#if FLY_CHAINS_ANY
				fly_chain_invalidate(pc);
				#endif
				g_defer_drop_count++;
				continue;
			}
			primeDispatchEntry(pc, nw * 2, (u32)idx);
			compiledCount++;
			g_drain_count++;
			promoted++;
		}
#ifndef JIT_PROD_BUILD
		if (promoted) {
			static u32 drain_log = 0;
			if (drain_log < 20 || (drain_log & 0x3F) == 0)
				EM_ASM({ console.log('[COMPILE-Q] promoted=' + $0 + ' (total ' + $1 +
					') deferred=' + $2 + ' dropped=' + $3); },
					promoted, g_drain_count, g_defer_count, g_defer_drop_count);
			drain_log++;
		}
#endif
#endif
	}

	// Flush the frame's queued hot misses as ONE sync-compiled multi-function
	// Module (per-module overhead paid once for the whole batch). Validation:
	// pointer identity in blockByVaddr + SMC hash vs live RAM — no deref of
	// possibly-freed blocks. Capped per flush; overflow carries (bridges one
	// more frame).
	u32 g_batch_flushed = 0, g_batch_dropped = 0;   // dev telemetry
	void flushBlockBatch()
	{
#ifdef __EMSCRIPTEN__
		if (g_batch_blocks.empty())
			return;
		const size_t CAP = 512;
		size_t take_n = g_batch_blocks.size() < CAP ? g_batch_blocks.size() : CAP;
		std::vector<RuntimeBlockInfo*> live;
		std::vector<u32> pcs;
		live.reserve(take_n);
		pcs.reserve(take_n);
		for (size_t i = 0; i < take_n; i++) {
			u32 pc = g_batch_blocks[i].first;
			RuntimeBlockInfo* blk = g_batch_blocks[i].second;
			g_batch_pending.erase(pc);
			auto it = blockByVaddr.find(pc);
			if (it == blockByVaddr.end() || it->second != blk) { g_batch_dropped++; continue; }
			auto smcit = g_block_smc.find(pc);
			if (smcit != g_block_smc.end() && smcit->second.sz != 0
			    && hashRamBlock(pc, smcit->second.sz) != smcit->second.hash) {
				g_batch_dropped++;
				continue;
			}
			live.push_back(blk);
			pcs.push_back(pc);
		}
		g_batch_blocks.erase(g_batch_blocks.begin(), g_batch_blocks.begin() + take_n);
		if (live.empty())
			return;
		WasmModuleBuilder bb;
		buildBatchModule(bb, live);
		const auto& bytes = bb.getBytes();
		std::vector<u32> outIdx(live.size(), 0);
#ifndef JIT_PROD_BUILD
		double bt0 = emscripten_get_now();
#endif
		int n = wasm_compile_block_batch(bytes.data(), (u32)bytes.size(),
		                                 pcs.data(), (u32)live.size(), outIdx.data());
		if (n > 0) {
			for (size_t i = 0; i < live.size(); i++) {
				if (outIdx[i] > 0) {
					primeDispatchEntry(pcs[i], live[i]->sh4_code_size, outIdx[i]);
					compiledCount++;
					g_batch_flushed++;
				}
			}
		}
#ifndef JIT_PROD_BUILD
		{
			static u32 batch_log = 0;
			if (batch_log < 20 || (batch_log & 0x1F) == 0)
				EM_ASM({ console.log('[BATCH] flushed=' + $0 + ' bytes=' + $1 +
					' ms=' + (+$2.toFixed(2)) + ' carried=' + $3 + ' total=' + $4); },
					(u32)live.size(), (u32)bytes.size(),
					emscripten_get_now() - bt0,
					(u32)g_batch_blocks.size(), g_batch_flushed);
			batch_log++;
		}
#endif
#endif
	}

	// Promote async-compiled chains (mirror of drainCompileQueue): validate
	// each ready chain's member fingerprints AND decode-freshness against live
	// RAM at promotion time — the async window between byte-build and resolve
	// is exactly where the staleness law bites. Stale/evicted chains are
	// dropped (module unregistered, guard_cells freed) and the sweep may
	// rediscover them fresh.
	void drainChainQueue()
	{
#if defined(__EMSCRIPTEN__) && FLY_CHAINS_ANY && FLY_CHAINS_LIVE
		// Reap failed compiles first (free their pending cells)
		for (;;) {
			u32 fpc = (u32)EM_ASM_INT({
				var q = Module._flyChainFail;
				if (!q || q.length === 0) return 0;
				return q.shift() | 0;
			});
			if (!fpc)
				break;
			auto pit = g_chain_pending.find(fpc);
			if (pit != g_chain_pending.end()) {
				free(pit->second.guard_cells);
				g_chain_pending.erase(pit);
			}
		}
		for (;;) {
			int idx = EM_ASM_INT({
				var q = Module._flyChainReady;
				if (!q || q.length === 0) return -1;
				var e = q.shift();
				Module._flyRpc = e.pc;
				return e.idx;
			});
			if (idx <= 0)
				break;
			u32 head = (u32)EM_ASM_INT({ return Module._flyRpc | 0; });
			auto pit = g_chain_pending.find(head);
			if (pit == g_chain_pending.end()) {
				// Cache reset or duplicate — nothing owns this module
				wasm_remove_chain(head);
				continue;
			}
			PendingChain& pend = pit->second;
			bool ok = true;
			{
				ChainInfo probe;
				probe.members = pend.members;
				if (fly_chain_is_stale(probe))
					ok = false;
				probe.guard_cells = nullptr;   // borrowed members only
			}
			if (ok) {
				for (const ChainMemberFp& m : pend.members) {
					u32 phys = m.pc & 0x1FFFFFFF;
					if ((phys >> 26) != 3) continue;
					auto smcit = g_block_smc.find(m.pc);
					if (smcit == g_block_smc.end()) { ok = false; break; }
					if (smcit->second.sz != 0
					    && hashRamBlock(m.pc, smcit->second.sz) != smcit->second.hash) {
						ok = false;
						break;
					}
				}
			}
			if (!ok) {
				wasm_remove_chain(head);
				free(pend.guard_cells);
				g_chain_pending.erase(pit);
				g_fly_chq_drop++;
				continue;
			}
			ChainInfo ci;
			ci.table_idx = (u32)idx;
			ci.members = std::move(pend.members);
			ci.guard_cells = pend.guard_cells;
			for (auto& m : ci.members)
				g_chain_member_index[m.pc].push_back(head);
			u32 head_size = pend.head_size;
			g_chain_by_head[head] = std::move(ci);
			g_chain_pending.erase(pit);
			g_fly_chq_prom++;
#ifndef JIT_PROD_BUILD
			g_fly_chains_built++;
#endif
			// Prime the head straight to the chain module. Head-slot SMC hash
			// covers the head; interior links have in-module (gen-gated)
			// guards; member replacement kills the chain via fly_chain_invalidate.
			primeDispatchEntry(head, head_size, (u32)idx);
		}
#endif
	}

	void mainloop(void* cntx) override
	{
		// Sh4Interpreter::Instance is now set in Sh4Recompiler's constructor
		// (ngen.h) — the redundant top-of-mainloop workaround was removed.
		fly_init();
		g_frame_start_ms = emscripten_get_now();
#ifndef JIT_PROD_BUILD
		{
			extern double g_fly_rp[10];
			for (int i = 0; i < 10; i++) g_fly_rp_snap[i] = g_fly_rp[i];
			g_fly_guest_snap = sh4_sched_now64();
		}
#endif
		g_defer_execs.clear();
		g_bridged_this_frame = 0;
#ifndef JIT_PROD_BUILD
		g_fly_shil_ms = 0;
		g_fly_shil_count = 0;
#endif
		// Async compiles resolve between rAF frames — promote them before
		// emulating so this frame dispatches them natively.
		drainCompileQueue();
		drainChainQueue();
		flushBlockBatch();
#if FLY_REGIONS_LIVE || FLY_REGION_SHADOW
		// Region pipeline at report-window cadence (~120 frames) — PROD-ACTIVE.
		// (The old call site lived inside the dev telemetry window; prod
		// builds never discovered a region. 2026-07-21.)
		{
			static u32 fly_rp_frame = 0;
			if ((++fly_rp_frame % 120) == 0)
				fly_region_pipeline_tick();
		}
#endif
#if FLY_REGIONS_LIVE
		// Region gen-baseline maintenance: a changed write-watch cur cell
		// means the in-module guard is exiting every iteration. Revalidate
		// members against live RAM: clean → update the baseline (region
		// resumes); stale → invalidate (staleness law).
		for (u32 ri = 0; ri < (u32)g_regions.size(); ri++) {
			RegionInfo& r = g_regions[ri];
			if (r.table_idx == 0 || !r.gen_cell)
				continue;
			u32 sum = r.gen_cell[1];   // cur, maintained by fly_ram_written
			if (sum == *r.gen_cell)
				continue;
			// CONTENT identity, not raw pointer (farm-certified path): an
			// identical re-decode gets a new RuntimeBlockInfo* but the same
			// code — keep the region. Genuine SMC changes the RAM hash and
			// invalidates. (Raw-pointer here would kill LIVE regions on the
			// first benign re-decode, exactly as it did in shadow run 13.)
			bool ok = true;
			for (const ChainMemberFp& m : r.members) {
				auto bit2 = blockByVaddr.find(m.pc);
				if (bit2 == blockByVaddr.end()
				    || (u32)bit2->second->oplist.size() != m.nops
				    || bit2->second->guest_cycles != m.gcycles
				    || (u32)bit2->second->BlockType != m.btype) { ok = false; break; }
				auto s2 = g_block_smc.find(m.pc);
				if (s2 == g_block_smc.end()) { ok = false; break; }
				if (s2->second.sz != 0
				    && hashRamBlock(m.pc, s2->second.sz) != s2->second.hash) { ok = false; break; }
			}
			if (ok)
				*r.gen_cell = sum;
			else
				fly_region_invalidate(ri);
		}
#endif
#if FLY_CHAINS_ANY
		// Chain sweep driver: chains rarely form at first compile (successors
		// not yet compiled) and slot re-prime killed the recompiles they used
		// to piggyback on. Sweep compiled blocks a few per frame, discover +
		// compile chains OFF the hot path. Shadow mode clears the head slot
		// (differential fires at the found-miss); LIVE mode primes the head
		// directly to the chain module.
#if FLY_CHAINS_LIVE
		// ASYNC + ADAPTIVE BURST (2026-07-18): chain compiles moved off-thread
		// (wasm_compile_chain_async → drainChainQueue promotion), so the old
		// 6/frame sync-jank throttle is gone. Budgets adapt: BURST while the
		// working set is un-chained (DOA2 measured ~7000 chains needed per
		// fight-mode switch — 20-30s at the old throttle), trickle when caught
		// up. A wall-time cap bounds main-thread byte-building either way.
		{
			static std::vector<u32> cs_sweep;
			static size_t cs_pos = 0;
			static u32 cs_frame = 0;
			static bool cs_burst = false;
			if ((cs_frame++ & 0x1F) == 0 && cs_pos >= cs_sweep.size()) {
				cs_sweep.clear();
				cs_sweep.reserve(blockByVaddr.size());
				for (auto& kv : blockByVaddr)
					cs_sweep.push_back(kv.first);
				cs_pos = 0;
			}
			int cs_scan_budget  = cs_burst ? 512 : 64;
			int cs_build_budget = cs_burst ? 64  : 8;
			const double cs_ms_cap = cs_burst ? 2.0 : 0.5;
			double cs_t0 = emscripten_get_now();
			int cs_built_this = 0;
			while (cs_scan_budget-- > 0 && cs_build_budget > 0 && cs_pos < cs_sweep.size()) {
				if (emscripten_get_now() - cs_t0 > cs_ms_cap)
					break;
				u32 v = cs_sweep[cs_pos++];
				if (g_chain_by_head.count(v) || g_chain_pending.count(v)) continue;
				auto bi = blockByVaddr.find(v);
				if (bi == blockByVaddr.end() || !wasm_has_block(v)) continue;
				auto schain = discoverChain(bi->second);
				if (schain.size() < 2) continue;
				// ★ DECODE-FRESHNESS GUARD (2026-07-17, post-lockup): a member
				// whose code changed while it sat undispatched holds a silently
				// stale decode — and priming the chain baselines its SMC hash
				// on CURRENT RAM, permanently validating the stale code. (The
				// shadow farm was blind to this: both sides shared the same
				// blockByVaddr decode.) Chain only blocks whose recorded
				// compile-time hash still matches live RAM.
				{
					bool fresh = true;
					for (auto* cblk : schain) {
						u32 phys = cblk->vaddr & 0x1FFFFFFF;
						if ((phys >> 26) != 3) continue;
						auto smcit = g_block_smc.find(cblk->vaddr);
						if (smcit == g_block_smc.end()) { fresh = false; break; }
						if (smcit->second.sz == 0) continue;
						if (hashRamBlock(cblk->vaddr, smcit->second.sz) != smcit->second.hash) {
							fresh = false;
							break;
						}
					}
					if (!fresh) continue;
				}
				// Page-gen guard cells: baseline = pgen sum at compile time (the
				// freshness guard above just validated the code against live RAM,
				// so this baseline is anchored to verified bytes). Tick staggered
				// by member index so forced hashes spread across dispatches.
				u32* gcells = (u32*)calloc(schain.size() * 2, sizeof(u32));
				if (gcells) {
					for (size_t gi = 0; gi < schain.size(); gi++) {
						RuntimeBlockInfo* cblk = schain[gi];
						u32 gphys = cblk->vaddr & 0x1FFFFFFF;
						if ((gphys >> 26) == 3) {
							u32 gro = gphys & RAM_MASK;
							u32 gn16 = cblk->sh4_code_size / 2;
							if (gn16 == 0) gn16 = 1;
							u32 gp0 = gro >> 12, gp1 = (gro + gn16 * 2 - 1) >> 12;
							gcells[gi * 2] = g_fly_page_gen[gp0]
								+ (gp1 != gp0 ? g_fly_page_gen[gp1] : 0);
						}
						gcells[gi * 2 + 1] = (u32)gi;
					}
				}
				WasmModuleBuilder cb;
				if (!buildMultiBlockModule(cb, schain, gcells)) { free(gcells); continue; }
				const auto& cbytes = cb.getBytes();
				wasm_compile_chain_async(cbytes.data(), (u32)cbytes.size(), v);
				PendingChain pend;
				pend.guard_cells = gcells;
				pend.head_size = bi->second->sh4_code_size;
				for (auto* cblk : schain)
					pend.members.push_back({ cblk->vaddr, cblk,
						(u32)cblk->oplist.size(), cblk->guest_cycles,
						(u32)cblk->BlockType });
				g_chain_pending[v] = std::move(pend);
				cs_build_budget--;
				cs_built_this++;
			}
			// Burst hysteresis: a fully-consumed build budget means more work
			// is flowing than the trickle absorbs; zero built = caught up.
			if (cs_built_this >= (cs_burst ? 48 : 8)) cs_burst = true;
			else if (cs_built_this == 0) cs_burst = false;
		}
#elif FLY_CHAIN_SHADOW
		// SHADOW FARM PATH — the original sync pipeline, kept intact so
		// differential results stay comparable across campaigns. (Not compiled
		// in region-farm builds: chain machinery must stay fully idle there.)
		{
			static std::vector<u32> cs_sweep;
			static size_t cs_pos = 0;
			static u32 cs_frame = 0;
			if ((cs_frame++ & 0xFF) == 0 && cs_pos >= cs_sweep.size()) {
				cs_sweep.clear();
				cs_sweep.reserve(blockByVaddr.size());
				for (auto& kv : blockByVaddr)
					cs_sweep.push_back(kv.first);
				cs_pos = 0;
			}
			int cs_budget = 32;
			int cs_compile_budget = 6;
			while (cs_budget-- > 0 && cs_compile_budget > 0 && cs_pos < cs_sweep.size()) {
				u32 v = cs_sweep[cs_pos++];
				if (g_chain_by_head.count(v)) continue;
				auto bi = blockByVaddr.find(v);
				if (bi == blockByVaddr.end() || !wasm_has_block(v)) continue;
				auto schain = discoverChain(bi->second);
				if (schain.size() < 2) continue;
				// Decode-freshness guard (see live path for rationale)
				{
					bool fresh = true;
					for (auto* cblk : schain) {
						u32 phys = cblk->vaddr & 0x1FFFFFFF;
						if ((phys >> 26) != 3) continue;
						auto smcit = g_block_smc.find(cblk->vaddr);
						if (smcit == g_block_smc.end()) { fresh = false; break; }
						if (smcit->second.sz == 0) continue;
						if (hashRamBlock(cblk->vaddr, smcit->second.sz) != smcit->second.hash) {
							fresh = false;
							break;
						}
					}
					if (!fresh) continue;
				}
				u32* gcells = (u32*)calloc(schain.size() * 2, sizeof(u32));
				if (gcells) {
					for (size_t gi = 0; gi < schain.size(); gi++) {
						RuntimeBlockInfo* cblk = schain[gi];
						u32 gphys = cblk->vaddr & 0x1FFFFFFF;
						if ((gphys >> 26) == 3) {
							u32 gro = gphys & RAM_MASK;
							u32 gn16 = cblk->sh4_code_size / 2;
							if (gn16 == 0) gn16 = 1;
							u32 gp0 = gro >> 12, gp1 = (gro + gn16 * 2 - 1) >> 12;
							gcells[gi * 2] = g_fly_page_gen[gp0]
								+ (gp1 != gp0 ? g_fly_page_gen[gp1] : 0);
						}
						gcells[gi * 2 + 1] = (u32)gi;
					}
				}
				WasmModuleBuilder cb;
				if (!buildMultiBlockModule(cb, schain, gcells)) { free(gcells); continue; }
				const auto& cbytes = cb.getBytes();
				int cidx = wasm_compile_chain(cbytes.data(), (u32)cbytes.size(), v);
				if (cidx <= 0) { free(gcells); continue; }
				ChainInfo ci;
				ci.table_idx = (u32)cidx;
				ci.guard_cells = gcells;
				for (auto* cblk : schain)
					ci.members.push_back({ cblk->vaddr, cblk,
						(u32)cblk->oplist.size(), cblk->guest_cycles,
						(u32)cblk->BlockType });
				for (auto& m : ci.members)
					g_chain_member_index[m.pc].push_back(v);
				g_chain_by_head[v] = std::move(ci);
				cs_compile_budget--;
#ifndef JIT_PROD_BUILD
				{
					static u32 fly_chains_built = 0;
					fly_chains_built++;
					g_fly_chains_built++;
					if ((fly_chains_built & 0xFF) == 0)
						EM_ASM({ console.log('[CHAINS] built=' + $0 + ' invalidated=' + $1 +
							' active=' + $2); },
							fly_chains_built, g_chain_invalidated,
							(u32)g_chain_by_head.size());
				}
#endif
				u32 ck = (v >> 1) & JIT_TABLE_MASK;
				if (jit_dispatch_pc[ck] == v) {
					jit_dispatch_table[ck] = 0; jit_dispatch_pc[ck] = 0;
					jit_dispatch_hash[ck] = 0; jit_dispatch_sz[ck] = 0;
				}
			}
		}
#endif  // FLY_CHAINS_LIVE
#endif
#if LOCKSTEP_DIFF_BUILD
		// Once the halt latch fires, FREEZE for real: the host keeps calling
		// mainloop per rAF, which previously kept emulating uncompared slices
		// and drifted the captured divergence state.
		if (g_lockstep_halt) {
			if (sh4ctx) sh4ctx->CpuRunning = false;
			return;
		}
#endif
		static u32 s_fly_frame_idx = 0;
		u32 fly_frame_idx = s_fly_frame_idx++;
		FLY_EVT(FLY_EVT_FRAME_BEGIN, fly_frame_idx, 0, 0, 0);
#if FLY_THROTTLE_FRAME_MS > 0
		double fly_throttle_t0 = emscripten_get_now();  // ISOLATION: pad frame to ~9fps
#endif
#ifdef __EMSCRIPTEN__
		// Live A/B dispatch toggle — set Module._useCleanDispatch=1 in the console.
		g_use_clean_dispatch = (EM_ASM_INT({
			return (typeof Module !== 'undefined' && Module._useCleanDispatch) ? 1 : 0;
		}) != 0);
		// (async-compile deferral is build-baked — see FLY_DEFER_ON)
#if FLY_VARIANT_B
		// Variant B: the (modified) clean loop IS the dispatcher for this build.
		g_use_clean_dispatch = true;
#endif
#endif

		// (per-frame cache flush removed — stale block theory disproven)

#if defined(__EMSCRIPTEN__)
		static int mainloop_count = 0;
		mainloop_count++;
#if !FLY_RELEASE_BUILD
		if (mainloop_count <= 3) {
			EM_ASM({ console.log('[rec_wasm] mainloop #' + $0 + ' cache=' + $1); },
				mainloop_count, wasm_cache_size());
		}
#endif
#endif
		u32 blockExecs = 0;
		u32 interpExecs = 0;
		u32 timeslices = 0;
		u32 compilesThisFrame = 0;
		// Dispatch stats — hoisted out of JIT_PROD_BUILD gate so fly_instrument
		// can emit them. These are per-frame u32 counters — trivial cost.
		u32 exit_ts_total = 0, exit_miss_total = 0, exit_int_total = 0;
		u32 miss_had_block = 0;
		u32 dispatch_zero_blocks = 0;
		double compileTimeThisFrame = 0;
		// Compile budget: was 8 ms (~130 compiles). Scene transitions need
		// 500–1000 new blocks; exhausting the budget sent the mainloop into
		// single-instruction interp fallback (measured 7.3M interp ops in a
		// single 375 ms frame). Raised to 50 ms — a single transition frame
		// may spike to ~50 ms, but every following frame stays cached and
		// clean instead of the multi-second interp spiral. Max frame cost
		// is strictly better than the alternative.
		const double COMPILE_TIME_BUDGET_MS = 50.0;
#ifndef JIT_PROD_BUILD
		double ml_start = emscripten_get_now();
		u32 fb_count_start = g_shil_fb_call_count;
		double compile_ms_start = wasm_prof_compile_ms();
#endif

		do {
			try {
#ifndef JIT_PROD_BUILD
					double emu_t0 = emscripten_get_now();
#endif
					u32 ctx_ptr = (u32)(uintptr_t)sh4ctx;
					u32 ram_ptr = (u32)(uintptr_t)&mem_b[0];

#ifndef JIT_PROD_BUILD
					u32 exit_ts_complete = 0, exit_miss = 0;
#endif
#if FORCE_CPP_DISPATCH
					// DIAGNOSTIC: Pure C++ block execution (no WASM blocks).
					// Same mainloop structure as JIT, but blocks run via C++ SHIL interpreter.
					while (sh4ctx->cycle_counter > 0) {
						u32 pc = sh4ctx->pc;
						dispTrace(pc);  // interpreter dispatch block-entry trace
#if EXECUTOR_MODE == 8 && !defined(JIT_PROD_BUILD)
						// HYBRID crash probes (mirror c_dispatch_loop's, which mode 8
						// bypasses). Tells us whether mode 8 ALSO reaches the 0x108
						// garbage-vector crash and HOW (pr=0 RTS-to-null vs mis-
						// delivered exception), plus whether it's running a STALE
						// (self-modified) block that plain dispatch never invalidates.
						{
							static u32 hyb_prev_pc = 0;
							static u32 hyb_last_pr = 0xFFFFFFFFu;
							// PR transition nonzero -> 0 (RTS-to-null setup)
							if (sh4ctx->pr == 0 && hyb_last_pr != 0 && hyb_last_pr != 0xFFFFFFFFu) {
								static u32 hpz = 0;
								if (hpz < 30) { hpz++;
									EM_ASM({ console.error('[HYB-PR-ZERO] #' + $0 + ' pr 0x' + ($1>>>0).toString(16) +
										'->0 set by block 0x' + ($2>>>0).toString(16) + ' now pc=0x' +
										($3>>>0).toString(16) + ' r15=0x' + ($4>>>0).toString(16)); },
										hpz, hyb_last_pr, hyb_prev_pc, pc, sh4ctx->r[15]); }
							}
							hyb_last_pr = sh4ctx->pr;
							// Low-PC (exception-vector / garbage) detector
							if (pc < 0x00010000u) {
								static u32 hlp = 0;
								if (hlp < 6) { hlp++;
									EM_ASM({ console.error('[HYB-LOWPC] #' + $0 + ' pc=0x' + ($1>>>0).toString(16) +
										' from block 0x' + ($2>>>0).toString(16) +
										' vbr=0x' + ($3>>>0).toString(16) + ' spc=0x' + ($4>>>0).toString(16) +
										' ssr=0x' + ($5>>>0).toString(16) + ' pr=0x' + ($6>>>0).toString(16) +
										' sr=0x' + ($7>>>0).toString(16)); },
										hlp, pc, hyb_prev_pc, sh4ctx->vbr, sh4ctx->spc,
										sh4ctx->ssr, sh4ctx->pr, sh4ctx->sr.status); }
							}
							// SMC INVALIDATION (mirrors c_dispatch_loop's check so mode 8 is
							// a FAIR isolation of the dispatch mechanism only): compare live
							// RAM hash vs the compile-time hash and, on mismatch, evict +
							// recompile the block from live RAM. Without this, mode 8 runs
							// stale self-modified code and crashes for a reason mode 6 does
							// NOT have (mode 6 HAS SMC detection) — confounding the test.
							{
								u32 phys = pc & 0x1FFFFFFF;
								if ((phys >> 26) == 3) {
									u32 key = (pc >> 1) & JIT_TABLE_MASK;
									u16 sz = jit_dispatch_sz[key];
									if (sz > 0 && jit_dispatch_pc[key] == pc) {
										u32 h = hashRamBlock(pc, sz);
										if (h != jit_dispatch_hash[key]) {
											static u32 hsmc = 0;
											if (hsmc < 30) { hsmc++;
												EM_ASM({ console.log('[HYB-SMC-EVICT] #' + $0 + ' pc=0x' + ($1>>>0).toString(16) +
													' compiled=0x' + ($2>>>0).toString(16) + ' live=0x' + ($3>>>0).toString(16) +
													' sz=' + $4 + ' (evict+recompile, matching mode 6 SMC)'); },
													hsmc, pc, jit_dispatch_hash[key], h, sz); }
											// Evict so the find below misses → recompiles from live RAM.
											jit_dispatch_table[key] = 0; jit_dispatch_pc[key] = 0;
											jit_dispatch_hash[key] = 0; jit_dispatch_sz[key] = 0;
											auto se = blockByVaddr.find(pc);
											if (se != blockByVaddr.end()) blockByVaddr.erase(se);
											wasm_remove_block(pc);
											#if FLY_CHAINS_ANY
											fly_chain_invalidate(pc);
											#endif
										}
									}
								}
							}
							hyb_prev_pc = pc;
						}
#endif
						auto it = blockByVaddr.find(pc);
						if (it == blockByVaddr.end()) {
							rdv_FailedToFindBlock(pc);
							it = blockByVaddr.find(pc);
						}
						if (it == blockByVaddr.end()) {
							// Can't find/compile block — interpret one instruction
							sh4ctx->pc = pc + 2;
							u16 rawOp = IReadMem16(pc);
							if (sh4ctx->sr.FD == 1 && OpDesc[rawOp]->IsFloatingPoint())
								throw SH4ThrownException(pc, Sh4Ex_FpuDisabled);
							OpPtr[rawOp](sh4ctx, rawOp);
							sh4ctx->cycle_counter -= 1;
							interpExecs++;
#if LOCKSTEP_DIFF_BUILD
							g_ls_interp_execs++;
#endif
						} else {
							RuntimeBlockInfo* block = it->second;
#if EXECUTOR_MODE == 7 || EXECUTOR_MODE == 0 || EXECUTOR_MODE == 8
							// EXECUTOR_MODE 7 = differential validator (JIT vs ref,
							// halt on divergence). EXECUTOR_MODE 0 = pure OpPtr
							// interpreter (JIT disabled). EXECUTOR_MODE 8 = HYBRID
							// (real WASM JIT block via this cpp dispatch, bypassing
							// c_dispatch_loop). All run via cpp_execute_block.
							// (g_val_halt is only set in mode 7.)
							cpp_execute_block(block);
							if (g_val_halt) { sh4ctx->CpuRunning = false; break; }
#if 0  // ---- superseded inline mode-7 (kept for reference) ----
							Sh4Context& ctx = *sh4ctx;
							alignas(16) static u8 jit7_pre[sizeof(Sh4Context)];
							memcpy(jit7_pre, &ctx, sizeof(Sh4Context));

							// Run JIT normally (writem forced to shil_fb via
							// FLY_FORCE_FALLBACK_MASK). Writes actually apply AND
							// are logged for comparison via g_shil_log_writes.
							g_shil_writes.clear();
							g_shil_log_writes = true;
							u32 ctx_ptr = (u32)(uintptr_t)&ctx;
							u32 ram_ptr = (u32)(uintptr_t)&mem_b[0];
							ctx.cycle_counter -= block->guest_cycles;
							g_ifb_exception_pending = false;
							int trap = wasm_execute_block(block->vaddr, ctx_ptr, ram_ptr);
							g_shil_log_writes = false;

							// Save JIT's writes + register state
							static std::vector<ShilWriteEntry> jit_writes;
							jit_writes = g_shil_writes;
							alignas(16) static u8 jit7_post[sizeof(Sh4Context)];
							memcpy(jit7_post, &ctx, sizeof(Sh4Context));

							// Restore pre-state for ref (registers). Memory
							// has JIT's writes, which ref will overwrite.
							memcpy(&ctx, jit7_pre, sizeof(Sh4Context));
							g_shil_writes.clear();
							g_shil_log_writes = true;
							ctx.cycle_counter -= block->guest_cycles;
							g_ifb_exception_pending = false;
							for (u32 i = 0; i < block->oplist.size(); i++)
								wasm_exec_shil_fb(block->vaddr, i);

							g_shil_log_writes = false;
							// Save ref's writes
							static std::vector<ShilWriteEntry> ref_writes;
							ref_writes = g_shil_writes;

							applyBlockExitCpp(block);
							if (g_ifb_exception_pending) {
								Do_Exception(g_ifb_exception_epc, g_ifb_exception_expEvn);
								g_ifb_exception_pending = false;
							}

							// Compare write logs: JIT vs ref
							static u32 write_mismatch_count = 0;
							if (!trap && write_mismatch_count < 100) {
								bool writes_match = (jit_writes.size() == ref_writes.size());
								u32 first_diff = 0;
								if (writes_match) {
									for (u32 wi = 0; wi < jit_writes.size(); wi++) {
										if (jit_writes[wi].addr != ref_writes[wi].addr ||
										    jit_writes[wi].val_lo != ref_writes[wi].val_lo ||
										    jit_writes[wi].size != ref_writes[wi].size) {
											writes_match = false;
											first_diff = wi;
											break;
										}
									}
								}
								if (!writes_match) {
									write_mismatch_count++;
									EM_ASM({
										console.log('[SHADOW-JIT-WRITE] MISMATCH #' + $0 +
											' pc=0x' + ($1>>>0).toString(16) +
											' jit_writes=' + $2 + ' ref_writes=' + $3 +
											' first_diff_idx=' + $4);
									}, write_mismatch_count, block->vaddr,
									   (u32)jit_writes.size(), (u32)ref_writes.size(), first_diff);
									if (write_mismatch_count <= 10) {
										u32 maxW = std::max(jit_writes.size(), ref_writes.size());
										for (u32 wi = 0; wi < std::min(maxW, (u32)10); wi++) {
											u32 ja = wi < jit_writes.size() ? jit_writes[wi].addr : 0xDEAD;
											u32 jv = wi < jit_writes.size() ? jit_writes[wi].val_lo : 0;
											u32 js = wi < jit_writes.size() ? jit_writes[wi].size : 0;
											u32 ra = wi < ref_writes.size() ? ref_writes[wi].addr : 0xDEAD;
											u32 rv = wi < ref_writes.size() ? ref_writes[wi].val_lo : 0;
											u32 rs = wi < ref_writes.size() ? ref_writes[wi].size : 0;
											EM_ASM({
												console.log('[SHADOW-JIT-WRITE] [' + $0 + '] ' +
													'jit: addr=0x' + ($1>>>0).toString(16) + ' val=0x' + ($2>>>0).toString(16) + ' sz=' + $3 +
													' | ref: addr=0x' + ($4>>>0).toString(16) + ' val=0x' + ($5>>>0).toString(16) + ' sz=' + $6);
											}, wi, ja, jv, js, ra, rv, rs);
										}
									}
								}
							}

							// Compare JIT vs ref if JIT ran
							static u32 jit7_match = 0;
							static u32 jit7_mismatch = 0;
							static u32 jit7_skip = 0;
							if (trap) {
								jit7_skip++;
								if (jit7_skip <= 5 || (jit7_skip & 0x3FF) == 0) {
									EM_ASM({ console.log('[SHADOW-JIT-SKIP] #' + $0 +
										' blk=' + $1 + ' pc=0x' + ($2>>>0).toString(16)); },
										jit7_skip, g_wasm_block_count, block->vaddr);
								}
							} else {
								const Sh4Context& jit_ctx = *(const Sh4Context*)jit7_post;
								const Sh4Context& ref_ctx = ctx;
								bool match = true;
								int diff_idx = -1;
								const char* diff_name = "";
								u32 jit_v = 0, ref_v = 0;
#define JIT7_CMP(field, name) \
	if (match && jit_ctx.field != ref_ctx.field) { \
		match = false; diff_name = name; \
		jit_v = (u32)jit_ctx.field; ref_v = (u32)ref_ctx.field; \
	}
#define JIT7_CMP_ARR(arr, count, name) \
	if (match) for (int _i = 0; _i < (count); _i++) { \
		if (*(const u32*)&jit_ctx.arr[_i] != *(const u32*)&ref_ctx.arr[_i]) { \
			match = false; diff_name = name; diff_idx = _i; \
			jit_v = *(const u32*)&jit_ctx.arr[_i]; \
			ref_v = *(const u32*)&ref_ctx.arr[_i]; \
			break; \
		} \
	}
								JIT7_CMP(pc, "pc")
								JIT7_CMP_ARR(r, 16, "r")
								JIT7_CMP(sr.T, "sr.T")
								JIT7_CMP(sr.status, "sr.status")
								JIT7_CMP_ARR(fr, 16, "fr")
								JIT7_CMP_ARR(xf, 16, "xf")
								JIT7_CMP(mac.l, "mac.l")
								JIT7_CMP(mac.h, "mac.h")
								JIT7_CMP(pr, "pr")
								JIT7_CMP(fpscr.full, "fpscr")
								JIT7_CMP(gbr, "gbr")
								JIT7_CMP(fpul, "fpul")
								JIT7_CMP_ARR(r_bank, 8, "r_bank")
								JIT7_CMP(vbr, "vbr")
								JIT7_CMP(ssr, "ssr")
								JIT7_CMP(spc, "spc")
								JIT7_CMP(sgr, "sgr")
								JIT7_CMP(dbr, "dbr")
#undef JIT7_CMP
#undef JIT7_CMP_ARR
								if (match) {
									jit7_match++;
								} else {
									jit7_mismatch++;
									if (jit7_mismatch <= 500) {
										EM_ASM({ console.log('[SHADOW-JIT] MISMATCH #' + $0 +
											' blk=' + $1 + ' pc=0x' + ($2>>>0).toString(16) +
											' diff=' + UTF8ToString($3) +
											(($4 >= 0) ? ('[' + $4 + ']') : '') +
											' jit=0x' + ($5>>>0).toString(16) +
											' ref=0x' + ($6>>>0).toString(16) +
											' nops=' + $7); },
											jit7_mismatch, g_wasm_block_count, block->vaddr,
											diff_name, diff_idx, jit_v, ref_v,
											(u32)block->oplist.size());

										// Diagnostic: address for first readm/writem op.
										// RAM region = real bug; MMIO (0xFF*) = artifact.
										{
											const Sh4Context& pre_ctx = *(const Sh4Context*)jit7_pre;
											if (!block->oplist.empty()) {
												const shil_opcode& op0 = block->oplist[0];
												if ((op0.op == shop_readm || op0.op == shop_writem) && op0.rs1.is_reg()) {
													u32 base = *(const u32*)((const u8*)&pre_ctx + op0.rs1.reg_offset());
													u32 offset = 0;
													if (op0.rs3.is_imm()) offset = op0.rs3._imm;
													else if (op0.rs3.is_reg())
														offset = *(const u32*)((const u8*)&pre_ctx + op0.rs3.reg_offset());
													u32 addr = base + offset;
													u32 phys = addr & 0x1FFFFFFF;
													const char* region =
														((phys >> 26) == 3) ? "RAM_area3" :
														((addr >= 0xFF000000u) ? "MMIO_FF" :
														((phys >> 26) == 0) ? "BIOS_area0" :
														((phys >> 26) == 7) ? "internal_area7" :
														"other");
													EM_ASM({ console.log("[SHADOW-JIT-ADDR] rs1.base=0x" + ($0>>>0).toString(16)
														+ " rs3.off=0x" + ($1>>>0).toString(16)
														+ " addr=0x" + ($2>>>0).toString(16)
														+ " region=" + UTF8ToString($3)); },
														base, offset, addr, region);
												}
											}
										}
										// Cap raised to 500 — expect ~5 residual mismatches post
										// multi-block-disable. We want the ops dumped for ALL of them
										// to compare JIT emission against canonical C++ impl.
										if (jit7_mismatch <= 500) {
											for (u32 i = 0; i < block->oplist.size() && i < 40; i++) {
												auto& sop = block->oplist[i];
												EM_ASM({ console.log('[SHADOW-JIT-OP] [' + $0 +
													'] shop=' + $1 +
													' rd.t=' + $2 + ',off=0x' + ($3>>>0).toString(16) +
													' rs1.t=' + $4 + ',off=0x' + ($5>>>0).toString(16) +
													' rs2.t=' + $6 + ',off=0x' + ($7>>>0).toString(16) +
													' rs3.t=' + $9 + ',off=0x' + ($10>>>0).toString(16) +
													' size=' + $8); },
													i, (int)sop.op,
													(int)sop.rd.type,  sop.rd.is_reg()  ? sop.rd.reg_offset()  : sop.rd._imm,
													(int)sop.rs1.type, sop.rs1.is_reg() ? sop.rs1.reg_offset() : sop.rs1._imm,
													(int)sop.rs2.type, sop.rs2.is_reg() ? sop.rs2.reg_offset() : sop.rs2._imm,
													sop.size,
													(int)sop.rs3.type, sop.rs3.is_reg() ? sop.rs3.reg_offset() : sop.rs3._imm);
											}
										}
									}
								}
							}
							if (((jit7_match + jit7_mismatch) & 0xFFF) == 0) {
								EM_ASM({ console.log('[SHADOW-JIT-OK] matches=' + $0 +
									' mismatches=' + $1 + ' skipped=' + $2); },
									jit7_match, jit7_mismatch, jit7_skip);
							}
#endif  // ---- end superseded inline mode-7 ----
#else
							// Default CPP dispatch: run via SHIL interpreter
							sh4ctx->cycle_counter -= block->guest_cycles;
							g_ifb_exception_pending = false;
							for (u32 i = 0; i < block->oplist.size(); i++)
								wasm_exec_shil_fb(block->vaddr, i);
							applyBlockExitCpp(block);
							if (g_ifb_exception_pending) {
								Do_Exception(g_ifb_exception_epc, g_ifb_exception_expEvn);
								g_ifb_exception_pending = false;
							}
#endif
							blockExecs++;
							g_wasm_block_count++;
							blockExecCount[pc]++;
						}
						if (sh4ctx->interrupt_pend)
							UpdateINTC();
					}
					exit_ts_complete++;
#else
				  if (g_use_clean_dispatch) {
						// CLEAN dispatch (mode-8 path) — runs the whole timeslice inline.
						int nblocks = clean_dispatch_loop(ctx_ptr, ram_ptr);
						blockExecs += nblocks;
						g_wasm_block_count += nblocks;
				  } else {
#if LOCKSTEP_DIFF_BUILD
					// Snapshot ctx + start write logging — the whole production
					// timeslice below (c_dispatch_loop + miss handler) runs logged,
					// then lockstep_compare re-runs it via clean_dispatch_loop and
					// binary-compares. See harness above clean_dispatch_loop.
					lockstep_begin();
#endif
					while (sh4ctx->cycle_counter > 0) {
#ifndef JIT_PROD_BUILD
						// Capture memory at watched address BEFORE c_dispatch_loop
						// so we can detect changes made INSIDE c_dispatch_loop vs
						// changes made OUTSIDE (miss handler).
						u32 ml_pre_target = *(u32*)&mem_b[0x0C0D9324 & RAM_MASK];
#endif
						int nblocks = c_dispatch_loop(ctx_ptr, ram_ptr);
						blockExecs += nblocks;
						g_wasm_block_count += nblocks;
						if (nblocks == 0) dispatch_zero_blocks++;

#ifndef JIT_PROD_BUILD
						u32 ml_post_dispatch_target = *(u32*)&mem_b[0x0C0D9324 & RAM_MASK];
						if (ml_post_dispatch_target != ml_pre_target) {
							static u32 ml_log_count_a = 0;
							if (ml_log_count_a < 20) {
								ml_log_count_a++;
								EM_ASM({
									if (!window._flyLog) window._flyLog = [];
									window._flyLog.push('[ML-WATCH-A] #' + $0
										+ ' INSIDE_DISPATCH result=' + $1
										+ ' miss_pc=0x' + ($2>>>0).toString(16)
										+ ' nblocks=' + $3
										+ ' 0x8c0d9324: 0x' + ($4>>>0).toString(16)
										+ ' -> 0x' + ($5>>>0).toString(16));
								}, ml_log_count_a, g_dispatch_result, g_dispatch_miss_pc,
								   nblocks, ml_pre_target, ml_post_dispatch_target);
							}
						}
#endif

						if (g_dispatch_result == 0) {
#ifndef JIT_PROD_BUILD
							exit_ts_complete++;
#endif
							exit_ts_total++;
							break;  // timeslice complete
						} else if (g_dispatch_result == 1) {
#ifndef JIT_PROD_BUILD
							exit_miss++;
#endif
							exit_miss_total++;
							u32 miss_pc = g_dispatch_miss_pc;

							// ★ MISS-PATH SMC CHECK (2026-06-11): c_dispatch_loop only
							// checks self-modifying code on table HITS. A block reached
							// via a hash COLLISION lands here and would otherwise run its
							// STALE compiled version (wrong code → wrong display list →
							// graphical glitch). The clean (mode-8) dispatch SMC-checks
							// every block and shows no glitches; this closes the gap.
							// If the block's RAM changed since compile, evict it so it
							// recompiles fresh from live RAM below.
							{
								auto smc_it = g_block_smc.find(miss_pc);
								if (smc_it != g_block_smc.end() && smc_it->second.sz > 0) {
									u32 phys = miss_pc & 0x1FFFFFFF;
									if ((phys >> 26) == 3) {
										u32 live = hashRamBlock(miss_pc, smc_it->second.sz);
										if (live != smc_it->second.hash) {
											auto be = blockByVaddr.find(miss_pc);
											if (be != blockByVaddr.end()) blockByVaddr.erase(be);
											wasm_remove_block(miss_pc);
											#if FLY_CHAINS_ANY
											fly_chain_invalidate(miss_pc);
											#endif
											g_block_smc.erase(miss_pc);
											u32 k = (miss_pc >> 1) & JIT_TABLE_MASK;
											if (jit_dispatch_pc[k] == miss_pc) {
												jit_dispatch_table[k] = 0; jit_dispatch_pc[k] = 0;
												jit_dispatch_hash[k] = 0; jit_dispatch_sz[k] = 0;
											}
#ifndef JIT_PROD_BUILD
											static u32 misssmc = 0;
											if (misssmc < 30) { misssmc++;
												EM_ASM({ console.log('[MISS-SMC] #' + $0 + ' pc=0x' + ($1>>>0).toString(16) +
													' stale block on collision path — recompiling fresh'); },
													misssmc, miss_pc); }
#endif
										}
									}
								}
							}

							auto it = blockByVaddr.find(miss_pc);
							if (it != blockByVaddr.end()) {
								// Block IS compiled but NOT in dispatch table
								// (hash collision or eviction). Execute via the REAL
								// WASM JIT block (still in _wasmBlockCache, keyed by PC
								// so it survives table-key collisions), then recompile
								// to restore the dispatch entry.
								//
								// ROOT-CAUSE FIX (2026-06-11): this path previously ran
								// the SHIL interpreter (wasm_exec_shil_fb +
								// applyBlockExitCpp). That fallback's exit/op handling
								// DIVERGES from the WASM JIT, corrupting control flow on
								// every collision/eviction miss → garbage jumps → reboot
								// loops (Skies et al). The mode-8 hybrid, which runs the
								// WASM JIT for EVERY block (never SHIL), boots Skies
								// clean — proving the SHIL miss path was the bug. So run
								// the JIT block here too. The block self-charges
								// guest_cycles in its prologue (do NOT pre-charge).
								miss_had_block++;
								// ★ FPSCR-ORDER FIX (2026-07-16): recompile BEFORE
								// executing. RuntimeBlockInfo::Setup samples LIVE
								// Sh4cntx.fpscr (driver.cpp:187) and the decoder bakes
								// fpscr.SZ/PR into the code (fmov 32-vs-64 decode,
								// decoder.cpp:950). fschg flips decode state statically
								// mid-block (decoder.cpp:318), so a block with an odd
								// fschg count leaves live fpscr.SZ OPPOSITE its entry
								// value. The old execute-THEN-recompile order sampled
								// that post-execution fpscr, baking wrong-size fmovs
								// into the replacement block — silent vertex/matrix
								// corruption on every later dispatch (the JGR glitch
								// class). Compile-first samples the correct entry
								// fpscr — the exact order Variant B ran glitch-free.
								// rdv sets Sh4cntx.pc = miss_pc, which is a no-op here
								// because the block hasn't run yet (no saved_pc dance).
								// ★ SLOT RE-PRIME (2026-07-16): if the block is already
								// compiled (collision evicted only its table slot),
								// restore the slot from the cached table index — no
								// recompile at all. This removes the constant collision
								// recompile churn (baseline: 48K compiles for a 21K-block
								// working set) and, under deferral, prevents collision
								// slots from being stuck in permanent per-dispatch
								// mainloop round-trips (priming used to happen only via
								// the rdv recompile, which the budget skips).
								bool fly_shadow_ran = false;
#if FLY_CHAIN_SHADOW
								// Chain-shadow hook: head slot was cleared at chain
								// compile; its first found-miss lands here. Run the
								// differential (reference result stays live), then
								// fall through to the normal re-prime below.
								if (it->second && wasm_has_block(miss_pc)
								    && g_chain_by_head.count(miss_pc)
								    && g_cs_done.insert(miss_pc).second) {
									sh4ctx->pc = miss_pc;
									fly_chain_shadow(miss_pc, ctx_ptr, ram_ptr);
									fly_shadow_ran = true;   // skip exec below; deliver pending + int check
								}
#endif
#if FLY_REGION_SHADOW
								// Region-shadow hook: member slots were cleared at
								// region registration; first found-miss per pc runs
								// the differential (reference result stays live).
								if (!fly_shadow_ran && it->second && wasm_has_block(miss_pc)
								    && g_region_member_of.count(miss_pc)
								    && g_rs_done.insert(miss_pc).second) {
									sh4ctx->pc = miss_pc;
									fly_region_shadow(miss_pc, ctx_ptr, ram_ptr);
									fly_shadow_ran = true;
								}
#endif
								{
									int cached_idx = wasm_get_block_idx(miss_pc);
									if (cached_idx > 0) {
#if FLY_BRIDGE_SHADOW
										// The one dispatch where both executables exist and
										// the block still routes through the miss path: run
										// the differential (once per pc), then prime. Bridge
										// result stays live (production-faithful).
										if (it->second && g_bs_done.insert(miss_pc).second) {
											sh4ctx->pc = miss_pc;
											fly_bridge_shadow(it->second, ctx_ptr, ram_ptr);
											fly_shadow_ran = true;
										}
#endif
#if FLY_WRITE_PARITY
										// Write-parity differential (same interception
										// point): reference first, prod-shape JIT second
										// and LIVE. Once per pc, then prime.
										if (it->second && g_wp_done.insert(miss_pc).second) {
											sh4ctx->pc = miss_pc;
											fly_write_parity_shadow(it->second, ctx_ptr, ram_ptr);
											fly_shadow_ran = true;
										}
#endif
										primeDispatchEntry(miss_pc, it->second->sh4_code_size, (u32)cached_idx);
									} else {
										// Deferred (no WASM yet). Hot blocks sync-compile
										// inline under FLY_SYNC_COMPILE_BUDGET_MS; all
										// other misses queue for the frame's batched sync
										// compile (flushBlockBatch) and bridge until then.
										bool force_hot = (++g_defer_execs[miss_pc] > FLY_HOT_DEFER_THRESHOLD);
										// ★ FORCE-HOT OUTRANKS THE QUEUE (2026-07-18): v1 let
										// the batch-pending guard swallow hot blocks — hot
										// loops bridged entire frames and chain formation
										// degraded (JGR 73%→64% absorption). A hot queued
										// block sync-compiles NOW; its stale queue entry is
										// dropped at flush by the pointer-identity check.
										// ★ STORM BUDGET LIFT (2026-07-23): in a storm frame the
										// 2ms sync budget is exhausted by the first hot blocks at
										// a transition, leaving the actual load-loop blocks (the
										// 2.1M-exec grinders) bridging forever. Once the frame is
										// storming it is already multi-hundred-ms — lift the budget
										// so the real working set lands natively.
										// ★ STORM BUDGET: 50ms IS THE FLOOR (2026-07-24): tuning
										// it to 15ms was measured and is a DEAD END — with the
										// budget starved the cold load-loop blocks never land
										// natively, and the (pacer-capped, ~33ms) guest slice
										// grinds through the bridge instead: 138K-307K bridged
										// execs / 48-92ms shil in one frame → 204-279ms walls,
										// worse than the pre-fix baseline. Compiling the working
										// set in ONE ~50ms gulp is strictly cheaper than bridging
										// it for even a single extra frame (~20ms saved vs
										// 150-200ms lost). Do not lower this again.
										double fly_sync_budget = (g_bridged_this_frame > FLY_STORM_BRIDGED_THRESHOLD)
										                         ? 50.0 : FLY_SYNC_COMPILE_BUDGET_MS;
										if (!g_defer_runtime
										    || (force_hot && compileTimeThisFrame < fly_sync_budget)) {
											double t0 = emscripten_get_now();
											rdv_FailedToFindBlock(miss_pc);
											compilesThisFrame++;
											compileTimeThisFrame += (emscripten_get_now() - t0);
										} else if (g_batch_pending.count(miss_pc)) {
											// Queued (and not hot) — bridge below; skipping
											// rdv also kills re-decode churn.
										} else if (!fly_frame_over_budget()
										           || g_bridged_this_frame > FLY_STORM_BRIDGED_THRESHOLD) {
											// ★ STORM (2026-07-23): past the bridged threshold
											// this frame is grinding a cold working set through
											// the interpreter (measured 0.4-2.3M bridged execs /
											// frame at transitions = the 0.5-2.1s freezes). The
											// over-budget guard exists to bound decode cost in
											// healthy frames; in a storm the tradeoff inverts —
											// queue regardless so the flush below can land the
											// working set natively THIS frame.
											g_compile_defer = true;
											g_compile_batch = true;
											double t0 = emscripten_get_now();
											rdv_FailedToFindBlock(miss_pc);
											g_compile_batch = false;
											g_compile_defer = false;
											compilesThisFrame++;
											compileTimeThisFrame += (emscripten_get_now() - t0);
										}
										// ★ STORM FLUSH: mid-frame sync batch compile once the
										// queue is deep — normal frames never reach this depth;
										// a storm reaches it within a few thousand bridges. A
										// few ms of sync Module compile beats hundreds of ms of
										// continued bridging in the same (multi-vblank) frame.
										if (g_batch_blocks.size() >= FLY_STORM_FLUSH_DEPTH)
											flushBlockBatch();
									}
								}
								it = blockByVaddr.find(miss_pc);  // rdv may have replaced the block
								RuntimeBlockInfo* block = it != blockByVaddr.end() ? it->second : nullptr;
								bool fly_bridged = false;
								if (fly_shadow_ran) {
									// Shadowed above — bridge result is live; pending
									// exception (if any) delivered below.
									fly_bridged = true;
								} else if ((sh4ctx->pc = miss_pc, g_ifb_exception_pending = false, wasm_has_block(miss_pc))) {
									wasm_execute_block(miss_pc, ctx_ptr, ram_ptr);
								} else if (block) {
									// Deferred block awaiting async promotion — whole-block
									// SHIL bridge (see fly_bridge_execute).
									// NOTE: cpp_execute_block is NOT usable here — in
									// EXECUTOR_MODE 6 it calls wasm_execute_block, which
									// traps on a not-yet-compiled pc.
#ifndef JIT_PROD_BUILD
									double shil_t0 = emscripten_get_now();
#endif
									fly_bridge_execute(block);
									fly_bridged = true;
									g_bridged_this_frame++;
#ifndef JIT_PROD_BUILD
									g_fly_shil_ms += emscripten_get_now() - shil_t0;
									g_fly_shil_count++;
#endif
								} else {
									// Recompile failed AND no cached WASM — interpret one
									// instruction to stay alive.
									sh4ctx->pc = miss_pc + 2;
									u16 rawOp = IReadMem16(miss_pc);
									if (sh4ctx->sr.FD == 1 && OpDesc[rawOp]->IsFloatingPoint())
										throw SH4ThrownException(miss_pc, Sh4Ex_FpuDisabled);
									OpPtr[rawOp](sh4ctx, rawOp);
									sh4ctx->cycle_counter -= 1;
								}
								if (g_ifb_exception_pending) {
									Do_Exception(g_ifb_exception_epc, g_ifb_exception_expEvn);
									g_ifb_exception_pending = false;
								}
								// ★ v6 (2026-07-17): bridged blocks must match the
								// dispatch loop's per-block interrupt check — without
								// this, interrupts raised during a bridged block are
								// delivered one block late (systematic timing skew
								// during compile storms, prime suspect for the v5
								// soak-time graphics divergence).
								if (fly_bridged && sh4ctx->interrupt_pend)
									UpdateINTC();
								blockExecs++;
								g_wasm_block_count++;
							} else {
								// Block not compiled yet — compile it.
								// ★ NO-BUDGET (2026-06-12): the COMPILE_TIME_BUDGET_MS gate
								// and its over-budget single-instruction interp fallback are
								// REMOVED. The lockstep differential proved that interp
								// fallback was the ONLY behavioral difference between this
								// path and the glitch-free clean dispatch: it charges 1
								// cycle/instruction (~3x the compiled-block rate), warping
								// guest timing exactly at scene-transition compile storms —
								// where the JGR glitches live. Every skew event had nonzero
								// prod_interp; zero data divergence found. Compile-always
								// matches clean's behavior; the cost is longer synchronous
								// compile hitches at scene loads (known, separate perf work).
								// ★ COMPILE-BUDGET (2026-07-16): that "separate perf work".
								// Over budget, decode still happens now (correct entry
								// fpscr) but the synchronous WebAssembly.compile is
								// deferred to drainCompileQueue(); the block executes via
								// the SHIL fallback below, charging block->guest_cycles —
								// guest timing identical to compiled execution, unlike the
								// old interp fallback.
								double t0 = emscripten_get_now();
								// Batch-first (2026-07-18): fresh blocks queue for the
								// frame's single batched sync Module instead of paying
								// ~1ms per-module sync overhead each — the transition
								// slog was hundreds of frames of exactly that. Over
								// frame budget they fall to plain async.
								g_compile_defer = g_defer_runtime;
								g_compile_batch = g_defer_runtime && !fly_frame_over_budget();
								rdv_FailedToFindBlock(miss_pc);
								g_compile_batch = false;
								g_compile_defer = false;
								compilesThisFrame++;
								it = blockByVaddr.find(miss_pc);
								compileTimeThisFrame += (emscripten_get_now() - t0);
								if (it != blockByVaddr.end()) {
									// Compiled — execute via SHIL interpreter
									RuntimeBlockInfo* block = it->second;
									sh4ctx->pc = miss_pc;
									g_ifb_exception_pending = false;
									bool fly_cold_bridged = false;
										if (wasm_has_block(miss_pc)) {
											wasm_execute_block(miss_pc, ctx_ptr, ram_ptr); // root-cause fix: WASM not SHIL
										} else {
											// Deferred cold block — whole-block SHIL bridge
											// (see fly_bridge_execute).
#ifndef JIT_PROD_BUILD
											double shil_t0 = emscripten_get_now();
#endif
											fly_bridge_execute(block);
											fly_cold_bridged = true;
#ifndef JIT_PROD_BUILD
											g_fly_shil_ms += emscripten_get_now() - shil_t0;
											g_fly_shil_count++;
#endif
										}
									if (g_ifb_exception_pending) {
										Do_Exception(g_ifb_exception_epc, g_ifb_exception_expEvn);
										g_ifb_exception_pending = false;
									}
									// ★ v6: per-block interrupt check for bridged blocks
									// (see found-branch comment).
									if (fly_cold_bridged && sh4ctx->interrupt_pend)
										UpdateINTC();
									blockExecs++;
									g_wasm_block_count++;
								} else {
									// Compilation failed — interpret one instruction
									sh4ctx->pc = miss_pc + 2;
									u16 rawOp = IReadMem16(miss_pc);
									if (sh4ctx->sr.FD == 1 && OpDesc[rawOp]->IsFloatingPoint())
										throw SH4ThrownException(miss_pc, Sh4Ex_FpuDisabled);
									OpPtr[rawOp](sh4ctx, rawOp);
									sh4ctx->cycle_counter -= 1;
									interpExecs++;
#if LOCKSTEP_DIFF_BUILD
							g_ls_interp_execs++;
#endif
								}
							}
						} else if (g_dispatch_result == 3) {
							// Interrupt pending
							exit_int_total++;
#if LOCKSTEP_DIFF_BUILD
							g_ls_intc_calls++;  // in-slice interrupt → slice not halt-eligible
#endif
							UpdateINTC();
						}
#ifndef JIT_PROD_BUILD
						// Detect if memory changed AFTER miss-handler / interrupt
						// handler — attributes the write to the mainloop action.
						u32 ml_post_action_target = *(u32*)&mem_b[0x0C0D9324 & RAM_MASK];
						if (ml_post_action_target != ml_post_dispatch_target) {
							static u32 ml_log_count_b = 0;
							if (ml_log_count_b < 20) {
								ml_log_count_b++;
								EM_ASM({
									if (!window._flyLog) window._flyLog = [];
									window._flyLog.push('[ML-WATCH-B] #' + $0
										+ ' AFTER_MISS_HANDLER result=' + $1
										+ ' miss_pc=0x' + ($2>>>0).toString(16)
										+ ' 0x8c0d9324: 0x' + ($3>>>0).toString(16)
										+ ' -> 0x' + ($4>>>0).toString(16));
								}, ml_log_count_b, g_dispatch_result, g_dispatch_miss_pc,
								   ml_post_dispatch_target, ml_post_action_target);
							}
						}
#endif
					}
#if LOCKSTEP_DIFF_BUILD
					// Production slice done — undo its RAM writes, re-run the same
					// slice via clean_dispatch_loop, compare regs + write streams.
					// Execution continues from the CLEAN result. Halt latch freezes
					// the emulator at the first pure-RAM divergence.
					lockstep_compare(ctx_ptr, ram_ptr);
					if (g_lockstep_halt) {
						sh4ctx->CpuRunning = false;
					}
#endif
				  }  // end else (production c_dispatch_loop path)
#endif

#ifndef JIT_PROD_BUILD
					// Debug: log dispatch loop exit reasons (first 3 mainloops)
					if (mainloop_count <= 3 && timeslices < 5) {
						EM_ASM({ console.log('[dispatch-debug] ts#' + $0 + ': cc_before=' + $1 + ' blocks=' + $2 + ' exit_ts=' + $3 + ' exit_miss=' + $4); },
							timeslices, sh4ctx->cycle_counter, blockExecs, exit_ts_complete, exit_miss);
					}
					double emu_t1 = emscripten_get_now();
					prof_emulation_ms += (emu_t1 - emu_t0);
#endif

					sh4ctx->cycle_counter += SH4_TIMESLICE;
					timeslices++;

#ifndef JIT_PROD_BUILD
					double sys_t0 = emscripten_get_now();
#endif
					UpdateSystem_INTC();
#ifndef JIT_PROD_BUILD
					double sys_t1 = emscripten_get_now();
					prof_system_ms += (sys_t1 - sys_t0);
#endif
					// TIMESLICE_END removed: fires 50k/sec in per-slice position
					// and duplicates FRAME_END's aggregate (blockExecs/interpExecs/
					// timeslices). Re-add as a sampled event if intra-frame detail
					// is needed.

				} catch (const SH4ThrownException& ex) {
					Do_Exception(ex.epc, ex.expEvn);
					sh4ctx->cycle_counter += 5;
				} catch (...) {
					// WASM trap (out-of-bounds, unreachable, type mismatch).
					// Invalidate the block at current PC and fall back to interpreter.
					u32 trap_pc = sh4ctx->pc;
					u32 trap_key = (trap_pc >> 1) & JIT_TABLE_MASK;
#if !FLY_RELEASE_BUILD
					// ALWAYS log traps (even in prod) — critical for debugging
					static u32 trap_log_count = 0;
					trap_log_count++;
					if (trap_log_count <= 100) {
						EM_ASM({ console.error('[JIT-TRAP #' + $0 + '] pc=0x' + ($1>>>0).toString(16) +
							' key=' + $2 + ' — invalidating block, falling back to interpreter'); },
							trap_log_count, trap_pc, trap_key);
					} else if (trap_log_count == 101) {
						EM_ASM({ console.error('[JIT-TRAP] suppressing further trap logs (100+ traps!)'); });
					}
#endif
					// Evict from dispatch table
					jit_dispatch_table[trap_key] = 0;
					jit_dispatch_pc[trap_key] = 0;
					// Evict from block maps
					auto it = blockByVaddr.find(trap_pc);
					if (it != blockByVaddr.end()) {
						blockByVaddr.erase(it);
					}
					jit_dispatch_hash[trap_key] = 0;
					jit_dispatch_sz[trap_key] = 0;
					// Interpret one instruction to advance past the trap.
					// OpPtr can throw SH4ThrownException (e.g., illegal
					// instruction at a garbage PC after a trap cascade).
					// Without this inner try/catch, the throw becomes a
					// nested exception inside catch(...) that escapes the
					// mainloop entirely → emulator dies permanently.
					sh4ctx->pc = trap_pc + 2;
					u16 rawOp = IReadMem16(trap_pc);
					try {
						if (sh4ctx->sr.FD == 1 && OpDesc[rawOp]->IsFloatingPoint())
							throw SH4ThrownException(trap_pc, Sh4Ex_FpuDisabled);
						OpPtr[rawOp](sh4ctx, rawOp);
					} catch (const SH4ThrownException& inner_ex) {
						Do_Exception(inner_ex.epc, inner_ex.expEvn);
						sh4ctx->cycle_counter += 5;
					}
					sh4ctx->cycle_counter -= 1;
					interpExecs++;
#if LOCKSTEP_DIFF_BUILD
							g_ls_interp_execs++;
#endif
			}
		} while (sh4ctx->CpuRunning);

		// Per-frame dispatch stats — critical for distinguishing hash-collision
		// thrash (miss_had_block high) from cold compile (compilesThisFrame high).
		FLY_EVT(FLY_EVT_FRAME_STATS,
		        miss_had_block,
		        exit_miss_total,
		        compilesThisFrame,
		        dispatch_zero_blocks);

		FLY_EVT(FLY_EVT_FRAME_END, fly_frame_idx, blockExecs, interpExecs, timeslices);
#ifdef __EMSCRIPTEN__
		// Render-phase timer [8]: the SH4 mainloop window (this function,
		// FRAME_BEGIN→END) — splits emu.render() into mainloop vs glue.
		{
			extern double g_fly_rp[10];
			g_fly_rp[8] += emscripten_get_now() - g_frame_start_ms;
		}
#endif

		sh4ctx->CpuRunning = false;

#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
		// Accumulate per-frame metrics for accurate profiling
		{
			double ml_elapsed = emscripten_get_now() - ml_start;
			double compile_ms = wasm_prof_compile_ms() - compile_ms_start;
			u32 fb_calls = g_shil_fb_call_count - fb_count_start;

			prof_wall_ms += ml_elapsed;
			prof_compile_ms_acc += compile_ms;
			prof_block_execs_acc += blockExecs;
			prof_timeslices_acc += timeslices;
			prof_fb_calls_acc += fb_calls;
			prof_interp_acc += interpExecs;
			prof_report_frames++;
		}

		// Profiling dump — every 120 mainloop calls (~2-4s depending on multi-frame)
		if (mainloop_count % 120 == 1 && prof_report_frames > 0) {
			int n = prof_report_frames;
			double total = prof_wall_ms;
			double emu = prof_emulation_ms;
			double sys = prof_system_ms;
			double compile = prof_compile_ms_acc;
			double other = total - emu - sys;
			u32 blks = prof_block_execs_acc;
			u32 ts = prof_timeslices_acc;
			u32 fbCalls = prof_fb_calls_acc;

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
				var n = $12;

				var pct = function(v) { return total > 0 ? (v / total * 100).toFixed(1) : '?'; };
				var avg = function(v) { return n > 0 ? (v / n).toFixed(1) : '?'; };
				console.log('');
				console.log('=== PROFILING REPORT (mainloop #' + $10 + ', ' + n + ' frames) ===');
				console.log('Dispatch: call_indirect (C dispatch loop, no JS)');
				console.log('');
				console.log('--- Time (total / per-frame avg) ---');
				console.log('Wall time:    ' + total.toFixed(0) + ' ms  (avg ' + avg(total) + ' ms/frame)');
				console.log('Emulation:    ' + emu.toFixed(0) + ' ms  (avg ' + avg(emu) + ' ms/frame)  ' + pct(emu) + '%');
				console.log('System:       ' + sys.toFixed(0) + ' ms  (avg ' + avg(sys) + ' ms/frame)  ' + pct(sys) + '%');
				console.log('Compilation:  ' + compile.toFixed(1) + ' ms  (avg ' + avg(compile) + ' ms/frame)  ' + pct(compile) + '%');
				console.log('Other:        ' + other.toFixed(0) + ' ms  (avg ' + avg(other) + ' ms/frame)  ' + pct(other) + '%');
				console.log('');
				console.log('--- Block Stats (total / per-frame avg) ---');
				console.log('Blocks:       ' + blks.toLocaleString() + '  (avg ' + (blks / n).toFixed(0) + '/frame)');
				console.log('Timeslices:   ' + ts.toLocaleString() + '  (avg ' + (ts / n).toFixed(0) + '/frame  = ' + (ts * 448 / n / 1e6).toFixed(2) + 'M cyc)');
				console.log('Blks/ts:      ' + (blks / ts).toFixed(1));
				console.log('Idle loops:   ' + $11);
				console.log('');
				console.log('--- Op Coverage (compile-time) ---');
				console.log('Native WASM:  ' + nativeOps.toLocaleString());
				console.log('Fallback C++: ' + fbOps.toLocaleString());
				console.log('Native ratio: ' + (nativeOps / (nativeOps + fbOps) * 100).toFixed(1) + '%');
				console.log('');
				console.log('--- Runtime Fallback (total / per-frame avg) ---');
				console.log('FB calls:     ' + fbCalls.toLocaleString() + '  (avg ' + (fbCalls / n).toFixed(0) + '/frame)');
				console.log('FB/block:     ' + (fbCalls / blks).toFixed(2));
				console.log('=== END PROFILING ===');
				console.log('');
			},
				total,               // $0
				emu,                 // $1
				sys,                 // $2
				compile,             // $3
				other,               // $4
				blks,                // $5
				ts,                  // $6
				fbCalls,             // $7
				prof_native_ops_compiled,   // $8
				prof_fallback_ops_compiled, // $9
				mainloop_count,      // $10
				prof_idle_loops_detected,   // $11
				n                    // $12
			);

			EM_ASM({
				console.log('--- Dispatch Details (total over ' + $8 + ' frames) ---');
				console.log('Interp instr: ' + $0.toLocaleString());
				console.log('Exit: ts_complete=' + $1 + ' miss=' + $2 + ' interrupt=' + $3);
				console.log('Miss had block: ' + $4 + '  dispatch_zero: ' + $5);
				console.log('Multi-blocks: ' + $6 + ' modules (' + $7 + ' blocks)');
			}, (int)prof_interp_acc, exit_ts_total, exit_miss_total, exit_int_total,
				miss_had_block, dispatch_zero_blocks,
				prof_multiblock_modules, prof_multiblock_total_blocks,
				n);

			struct FbEntry { int op; u32 count; };
			FbEntry top[10] = {};
			for (int i = 0; i < 128; i++) {
				if (prof_fb_by_op[i] > 0) {
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
				EM_ASM({ console.log('  shop_' + $0 + ': ' + $1.toLocaleString() + ' calls  (avg ' + ($1 / $2).toFixed(0) + '/frame)'); },
					top[j].op, top[j].count, n);
			}

			// Structured perf marker for automated collection
			EM_ASM({
				console.log('[PERF] ' +
					'{"type":"report"' +
					',"mainloop":' + $0 +
					',"frames":' + $1 +
					',"wall_ms":' + (+$2.toFixed(1)) +
					',"avg_frame_ms":' + (+($2 / $1).toFixed(2)) +
					',"emu_ms":' + (+$3.toFixed(1)) +
					',"emu_pct":' + (+($3 / $2 * 100).toFixed(1)) +
					',"sys_ms":' + (+$4.toFixed(1)) +
					',"sys_pct":' + (+($4 / $2 * 100).toFixed(1)) +
					',"compile_ms":' + (+$5.toFixed(1)) +
					',"compile_pct":' + (+($5 / $2 * 100).toFixed(1)) +
					',"other_ms":' + (+$6.toFixed(1)) +
					',"other_pct":' + (+($6 / $2 * 100).toFixed(1)) +
					',"blocks_per_frame":' + (+($7 / $1).toFixed(0)) +
					',"timeslices_per_frame":' + (+($8 / $1).toFixed(0)) +
					',"mcycles_per_frame":' + (+($8 * 448 / $1 / 1e6).toFixed(2)) +
					',"fallback_calls_per_frame":' + (+($9 / $1).toFixed(0)) +
					',"native_ops":' + $10 +
					',"fallback_ops":' + $11 +
					',"native_ratio_pct":' + (+($10 / ($10 + $11) * 100).toFixed(1)) +
					',"cache_size":' + $12 +
					',"exit_ts":' + $13 +
					',"exit_miss":' + $14 +
					',"exit_int":' + $15 +
					'}');
			},
				mainloop_count,              // $0
				n,                           // $1
				total,                       // $2
				emu,                         // $3
				sys,                         // $4
				compile,                     // $5
				other,                       // $6
				blks,                        // $7
				ts,                          // $8
				fbCalls,                     // $9
				prof_native_ops_compiled,    // $10
				prof_fallback_ops_compiled,  // $11
				wasm_cache_size(),           // $12
				exit_ts_total,               // $13
				exit_miss_total,             // $14
				exit_int_total               // $15
			);
#ifndef JIT_PROD_BUILD
		{
			static u32 mr_last = 0, mrr_last = 0, mw_last = 0, mwr_last = 0;
			static u32 mr8_last = 0, mw8_last = 0, mwsq_last = 0;
			u32 dr = g_fly_mr - mr_last, drr = g_fly_mr_ram - mrr_last;
			u32 dw = g_fly_mw - mw_last, dwr = g_fly_mw_ram - mwr_last;
			u32 dr8 = g_fly_mr8 - mr8_last, dw8 = g_fly_mw8 - mw8_last;
			u32 dwsq = g_fly_mw_sq - mwsq_last;
			mr_last = g_fly_mr; mrr_last = g_fly_mr_ram;
			mw_last = g_fly_mw; mwr_last = g_fly_mw_ram;
			mr8_last = g_fly_mr8; mw8_last = g_fly_mw8;
			mwsq_last = g_fly_mw_sq;
			{
				static u32 hw_last = 0, chw_last = 0;
				u32 dhw = g_fly_hash_words - hw_last;
				hw_last = g_fly_hash_words;
				extern u32 g_fly_chain_hash_words;
				u32 dchw = g_fly_chain_hash_words - chw_last;
				chw_last = g_fly_chain_hash_words;
				EM_ASM({ console.log('[SMC-HASH] words_pf=' + ($0 / 120 | 0) +
					' chain_words_pf=' + ($1 / 120 | 0)); }, dhw, dchw);
			}
			{
				// Chain absorption + churn per report window. singles = call_indirects
				// that were NOT chain entries; absorption = links / (links + singles).
				static u32 ce_last = 0, cl_last = 0, cb_last = 0, ci_last = 0;
				u32 dce = g_fly_chain_enters - ce_last; ce_last = g_fly_chain_enters;
				u32 dcl = g_fly_chain_links - cl_last;  cl_last = g_fly_chain_links;
				u32 dcb = g_fly_chains_built - cb_last; cb_last = g_fly_chains_built;
				u32 dci = g_chain_invalidated - ci_last; ci_last = g_chain_invalidated;
				u32 singles = (blks > dce) ? (blks - dce) : 0;
				static u32 qp_last = 0, qd_last = 0;
				u32 dqp = g_fly_chq_prom - qp_last; qp_last = g_fly_chq_prom;
				u32 dqd = g_fly_chq_drop - qd_last; qd_last = g_fly_chq_drop;
				{
					// Page-locality report: coverage of top-N pages this window
					static u32 pe_last[4096];
					u32 deltas[4096];
					u64 tot = 0;
					for (int pi = 0; pi < 4096; pi++) {
						deltas[pi] = g_fly_page_execs[pi] - pe_last[pi];
						pe_last[pi] = g_fly_page_execs[pi];
						tot += deltas[pi];
					}
					if (tot > 0) {
						// partial selection: top 64 by repeated max-scan (dev-only cost)
						u64 acc = 0; u32 active = 0;
						double top4 = 0, top16 = 0, top64 = 0;
						for (int pi = 0; pi < 4096; pi++) if (deltas[pi]) active++;
						for (int rank = 0; rank < 64; rank++) {
							int best = -1; u32 bv = 0;
							for (int pi = 0; pi < 4096; pi++)
								if (deltas[pi] > bv) { bv = deltas[pi]; best = pi; }
							if (best < 0) break;
							acc += bv; deltas[best] = 0;
							if (rank == 3)  top4  = 100.0 * acc / tot;
							if (rank == 15) top16 = 100.0 * acc / tot;
							if (rank == 63) top64 = 100.0 * acc / tot;
						}
						if (top64 == 0) top64 = 100.0 * acc / tot;
						EM_ASM({ console.log('[PAGE-LOC] top4=' + (+$0.toFixed(1)) +
							'% top16=' + (+$1.toFixed(1)) +
							'% top64=' + (+$2.toFixed(1)) +
							'% active_pages=' + $3); },
							top4, top16, top64, active);
					}
				}
#if FLY_REGION_SHADOW
				{
					// FARM PUMP: re-arm the differential every report window —
					// clear the once-per-pc set and re-clear live members' slots
					// so each member re-compares against fresh game state.
					// Turns ~1 comparison per member per run into hundreds.
					g_rs_done.clear();
					u32 fly_pump_cleared = 0, fly_pump_total = 0;
					for (auto& fr : g_regions) {
						if (fr.table_idx == 0) continue;
						for (const ChainMemberFp& fm : fr.members) {
							fly_pump_total++;
							u32 fk = (fm.pc >> 1) & JIT_TABLE_MASK;
							if (jit_dispatch_pc[fk] == fm.pc) {
								jit_dispatch_table[fk] = 0; jit_dispatch_pc[fk] = 0;
								jit_dispatch_hash[fk] = 0; jit_dispatch_sz[fk] = 0;
								jit_dispatch_arg[fk] = 0;
								fly_pump_cleared++;
							}
						}
					}
					extern u32 g_rs_hook_hits, g_rs_early_ret;
					EM_ASM({ console.log('[PUMP] cleared=' + $0 + '/' + $1 +
						' hook_hits=' + $2 + ' early_ret=' + $3); },
						fly_pump_cleared, fly_pump_total, g_rs_hook_hits, g_rs_early_ret);
				}
#endif
				// (region pipeline moved to fly_region_pipeline_tick() — prod-active, called from the mainloop)
				{
					static u32 ei_last = 0, ec_last = 0;
					u32 di = g_fly_edge_intra - ei_last; ei_last = g_fly_edge_intra;
					u32 dc = g_fly_edge_cross - ec_last; ec_last = g_fly_edge_cross;
					u32 p1k = 0, p1v = 0, p2k = 0, p2v = 0, p3k = 0, p3v = 0;
					for (auto& [ek, ev] : g_fly_edge_pairs) {
						if (ev > p1v)      { p3k = p2k; p3v = p2v; p2k = p1k; p2v = p1v; p1k = ek; p1v = ev; }
						else if (ev > p2v) { p3k = p2k; p3v = p2v; p2k = ek; p2v = ev; }
						else if (ev > p3v) { p3k = ek; p3v = ev; }
					}
					EM_ASM({ console.log('[PAGE-EDGE] intra_pct=' + (($0 + $1) ? ($0 * 100 / ($0 + $1) | 0) : 0) +
						' cross_pf=' + ($1 / $2 | 0) +
						' top_pairs(cum)=' + ($3 >>> 16).toString(16) + '->' + ($3 & 0xFFFF).toString(16) + ':' + $4 +
						' ' + ($5 >>> 16).toString(16) + '->' + ($5 & 0xFFFF).toString(16) + ':' + $6 +
						' ' + ($7 >>> 16).toString(16) + '->' + ($7 & 0xFFFF).toString(16) + ':' + $8); },
						di, dc, n, p1k, p1v, p2k, p2v, p3k, p3v);
				}
				{
					static u32 cc_last = 0, cr_last = 0;
					u32 dcc = g_fly_code_compiles - cc_last; cc_last = g_fly_code_compiles;
					u32 dcr = g_fly_code_returns - cr_last;  cr_last = g_fly_code_returns;
					EM_ASM({ console.log('[CODE-RETURN] compiles_d=' + $0 +
						' returns_d=' + $1 +
						' ret_pct=' + ($0 ? ($1 * 100 / $0 | 0) : 0) +
						' total=' + $2 + '/' + $3); },
						dcc, dcr, g_fly_code_returns, g_fly_code_compiles);
				}
				EM_ASM({ console.log('[CHAIN-STATS] enters_pf=' + ($0 / $5 | 0) +
					' links_pf=' + ($1 / $5 | 0) +
					' singles_pf=' + ($2 / $5 | 0) +
					' absorb_pct=' + (($1 + $2) ? ($1 * 100 / ($1 + $2) | 0) : 0) +
					' built_d=' + $3 + ' inval_d=' + $4 +
					' q_prom_d=' + $6 + ' q_drop_d=' + $7 + ' pending=' + $8); },
					dce, dcl, singles, dcb, dci, n, dqp, dqd,
					(u32)g_chain_pending.size());
			}
			EM_ASM({ console.log('[MEM-DENSITY] r_pf=' + ($0 / 120 | 0) +
				' r_ram_pct=' + ($1 ? ($2 * 100 / $1 | 0) : 0) +
				' w_pf=' + ($3 / 120 | 0) +
				' w_ram_pct=' + ($4 ? ($5 * 100 / $4 | 0) : 0) +
				' r8_pf=' + ($6 / 120 | 0) +
				' w8_pf=' + ($7 / 120 | 0) +
				' r8_share_pct=' + ($0 ? ($6 * 2 * 100 / $0 | 0) : 0) +
				' wsq_pf=' + ($8 / 120 | 0) +
				' wsq_share_pct=' + ($3 ? ($8 * 100 / $3 | 0) : 0)); },
				dr, dr, drr, dw, dw, dwr, dr8, dw8, dwsq);
		}
#endif

			// Reset all accumulators
			prof_wall_ms = 0;
			prof_emulation_ms = 0;
			prof_system_ms = 0;
			prof_compile_ms_acc = 0;
			prof_block_execs_acc = 0;
			prof_timeslices_acc = 0;
			prof_fb_calls_acc = 0;
			prof_interp_acc = 0;
			prof_report_frames = 0;
			memset(prof_fb_by_op, 0, sizeof(prof_fb_by_op));
		}
#endif
#if FLY_THROTTLE_FRAME_MS > 0
		// ISOLATION: busy-spin to pad this frame to ~FLY_THROTTLE_FRAME_MS wall-clock,
		// keeping emulated work identical, to replicate the slow-motion condition
		// under which the clean dispatch showed no glitches.
		while (emscripten_get_now() - fly_throttle_t0 < (double)FLY_THROTTLE_FRAME_MS) { }
#endif
		// Promote deferred blocks with whatever frame budget remains (always
		// at least one — see drainCompileQueue).
		drainCompileQueue();
#ifndef JIT_PROD_BUILD
		// Spike-composition instrument (2026-07-16): the 850ms+ frames are NOT
		// compile time (they survived full compile deferral). Attribute what a
		// slow frame is actually made of; whatever lands in `other` is
		// unattributed (render lists, GC pauses, ...) and gets split next.
		{
			double fly_total_ms = emscripten_get_now() - g_frame_start_ms;
			// Threshold 100→50ms (2026-07-23 spike hunt): the felt stutter
			// class post-fastmem is 63-330ms single frames; catch all of it.
			if (fly_total_ms > 50.0) {
				static u32 spike_log = 0;
				if (spike_log < 300) {
					spike_log++;
					EM_ASM({ console.log('[SPIKE] total=' + $0.toFixed(1) +
						'ms compile=' + $1.toFixed(1) +
						'ms tex_check=' + $2.toFixed(1) +
						'ms tex_update=' + $3.toFixed(1) + 'ms (' + $4 +
						' textures) shil=' + $6.toFixed(1) + 'ms (' + $7 +
						' execs) other=' + $5.toFixed(1) + 'ms'); },
						fly_total_ms, compileTimeThisFrame,
						g_fly_tex_check_ms, g_fly_tex_update_ms,
						g_fly_tex_update_count,
						fly_total_ms - compileTimeThisFrame - g_fly_tex_check_ms - g_fly_tex_update_ms - g_fly_shil_ms,
						g_fly_shil_ms, g_fly_shil_count);
					// Round-2 attribution (separate EM_ASM — 16-arg cap):
					// guest_ms = guest time advanced this frame (200MHz).
					// ~16.6 = one vblank (per-op slowness); >>16.6 = pacer
					// debt amplification. Bracket deltas name Population A
					// (er = present path; all-zero = GC/unbracketed).
					{
						extern double g_fly_rp[10];
						double guest_ms = (double)(s64)(sh4_sched_now64() - g_fly_guest_snap) / 200000.0;
						EM_ASM({ console.log('[SPIKE2] guest_ms=' + $0.toFixed(1) +
							' ml=' + $1.toFixed(1) +
							' er=' + $2.toFixed(1) +
							' parse=' + $3.toFixed(1) +
							' sort=' + $4.toFixed(1) +
							' draw=' + $5.toFixed(1) +
							' aica=' + $6.toFixed(1)); },
							guest_ms,
							g_fly_rp[8] - g_fly_rp_snap[8],
							g_fly_rp[4] - g_fly_rp_snap[4],
							g_fly_rp[0] - g_fly_rp_snap[0],
							g_fly_rp[1] - g_fly_rp_snap[1],
							g_fly_rp[2] - g_fly_rp_snap[2],
							g_fly_rp[5] - g_fly_rp_snap[5]);
					}
				}
			}
			g_fly_tex_check_ms = 0;
			g_fly_tex_update_ms = 0;
			g_fly_tex_update_count = 0;
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
		static u32 reset_counter = 0;
		reset_counter++;
		u32 blocks_evicted = (u32)blockByVaddr.size();
		// Pass current SH4 PC in `d` slot — identifies the trigger (magic PCs
		// 0x8c0000e0 / 0xac010000 / 0xac008300 are known reset triggers in
		// compilePC, other PCs mean some other path hit reset).
		u32 trigger_pc = sh4ctx ? sh4ctx->pc : 0;
		FLY_EVT(FLY_EVT_CACHE_RESET, reset_counter, blocks_evicted, compiledCount, trigger_pc);
		wasm_clear_cache();
		memset(jit_dispatch_table, 0, sizeof(jit_dispatch_table));
		memset(jit_dispatch_pc, 0, sizeof(jit_dispatch_pc));
		memset(jit_dispatch_hash, 0, sizeof(jit_dispatch_hash));
		memset(jit_dispatch_sz, 0, sizeof(jit_dispatch_sz));
		g_batch_blocks.clear();     // queued ptrs die with the block map
		g_batch_pending.clear();
		blockByVaddr.clear();
		g_block_smc.clear();
#if FLY_BRIDGE_SHADOW
		g_bs_done.clear();
#endif
#if FLY_WRITE_PARITY
		g_wp_done.clear();
#endif
#if FLY_CHAINS_ANY
		for (auto& [chp, cci] : g_chain_by_head)
			free(cci.guard_cells);
		g_chain_by_head.clear();
		g_chain_member_index.clear();
		fly_reap_pending_chains();
		for (auto& rr : g_regions) {
			free(rr.gen_cell);
			free(rr.ic_cells);
			free(rr.ras_cells);
			free(rr.pc_table);
		}
		g_regions.clear();
		g_region_member_of.clear();
		g_region_dyn_miss = 0xFFFFFFFFu;
		fly_region_watch_rebuild();   // registry must not point at freed cells
		memset(jit_dispatch_arg, 0, sizeof(jit_dispatch_arg));
		g_cs_done.clear();
#if FLY_REGION_SHADOW
		g_rs_done.clear();
#endif
		wasm_clear_chains();
#endif
		blockExecCount.clear();
		compiledCount = 0;
		failCount = 0;
#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
		EM_ASM({ console.log('[rec_wasm] reset #' + $0 + ': cleared ' + $1 + ' blocks'); },
			reset_counter, blocks_evicted);
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

// ============================================================
// Layer 1 SHIL op unit test harness
// ============================================================
// Included here (after all statics) so the harness has access to
// buildBlockModule, blockByVaddr, wasm_compile_block, etc.
#if defined(__EMSCRIPTEN__) && !defined(JIT_PROD_BUILD)
#include "wasm_test_shil_ops.h"
#include "wasm_test_dispatch.h"
#include "wasm_test_rte.h"
#include "wasm_test_singlestep.h"

extern "C" int EMSCRIPTEN_KEEPALIVE run_shil_op_tests() {
	return shil_op_test_harness();
}

extern "C" int EMSCRIPTEN_KEEPALIVE run_dispatch_tests() {
	return dispatch_test_harness();
}

extern "C" int EMSCRIPTEN_KEEPALIVE run_rte_tests() {
	return rte_test_harness();
}

extern "C" int EMSCRIPTEN_KEEPALIVE run_singlestep_tests() {
	return run_singlestep_tests_impl();
}
#endif

extern "C" void wasm_dynarec_init()
{
	if (!sh4Dynarec)
		sh4Dynarec = &instance;
}

#endif // FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_GENERIC
