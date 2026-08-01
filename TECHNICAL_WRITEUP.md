# Flycast WASM: A Custom SH4-to-WebAssembly JIT

## How the First Browser Dreamcast Emulator Reached Native Speed

**Part 2 of the technical write-up. February to August 2026.**

---

## 1. What this is

This is a Sega Dreamcast emulator running in a browser tab, the first one. Inside
it is the first SH4-to-WebAssembly dynamic recompiler ever built. Heavy 3D titles
that ran at roughly 2 FPS on the initial interpreter port now run at a locked
60 FPS at 640x480 with clean audio.

The community consensus was that this couldn't be done. The upstream Flycast
maintainer explicitly declined WebAssembly support in 2025. EmulatorJS doesn't
list Dreamcast. The libretro buildbot produces around 97 Emscripten cores, and
Flycast isn't one of them. The standard argument went like this: the Dreamcast's
SH4 runs at 200 MHz with a demanding superscalar pipeline, an interpreter can't
keep up, and WebAssembly can't self-modify. A WASM module cannot generate and
execute new code in its own address space, so the conventional dynarec design is
simply unavailable.

The first half of that argument is true. The second half has a loophole, and
this document is about driving a full production JIT through it.

**Part 1** (the February write-up, `TECHNICAL_WRITEUP.md`) covered getting
upstream Flycast compiled, linked, and booting in a browser at all: some 30
distinct bugs across the Makefile, Emscripten linker flags, WebGL2 compatibility
shims, and EmulatorJS integration. The result was a correct but slow emulator.
It ran the pure SH4 interpreter, instruction by instruction, at a couple of
frames per second in 3D games. Everything in this document happened after that.
The JIT is the project's real substance.

The short version of the loophole: a WASM module can't modify itself, but the
*embedder* can compile new WASM modules at runtime (`WebAssembly.compile`), and
those modules can share the emulator's linear memory and function table. So the
JIT emits raw WASM bytecode from C++ and hands the bytes to the browser's
compiler, which produces genuinely native machine code (V8's TurboFan or
Firefox's Ion). The resulting function gets registered in the shared indirect
function table, and a C dispatch loop invokes it with `call_indirect`. After
warm-up, the hot path never touches JavaScript.

Getting from "this executes" to "this is fast" took six months of hitting very
specific walls. Each of them is documented below with its mechanism and its
measured result. The other half of the story is the validation methodology that
made it possible to keep a JIT correct while rebuilding its memory system
underneath running games (§5). That half may be the more transferable one.

---

## 2. Architecture

```
SH4 machine code
      │  (Flycast's decoder, unmodified upstream)
      ▼
SHIL IR  (Flycast's portable intermediate representation)
      │  (wasm_emit.h: per-op emitters + register cache)
      ▼
WASM bytecode module  (wasm_module_builder.h: raw binary format, no deps)
      │  WebAssembly.compile (async, off the frame path)
      ▼
Entry in Emscripten's __indirect_function_table
      │
      ▼
c_dispatch_loop ──call_indirect──▶ compiled block   (no JS in the hot path)
```

The recompiler slots into Flycast's existing dynarec framework as a new backend
(`CPU_GENERIC` plus `DYNAREC_JIT` under Emscripten), so block discovery,
decoding, and the SHIL IR all come from unmodified upstream code. Three new
files hold the backend:

- **`rec_wasm.cpp`**: the dynarec driver. C dispatch loop, the compile bridge
  to JavaScript, chaining, SMC detection, and all the differential harnesses.
- **`wasm_module_builder.h`**: a dependency-free WASM binary-format writer
  (i32/i64/f32/f64 ops, locals, blocks/loops/branches, custom sections).
- **`wasm_emit.h`**: SHIL-op to WASM emitters, plus the register cache.

### 2.1 Block compilation

Each SH4 basic block becomes one exported WASM function. The function takes two
parameters, a pointer to `Sh4Context` and the linear-memory offset of Dreamcast
main RAM, and operates directly on the emulator's state in shared linear
memory. Memory I/O that can't be inlined goes through imported C functions.
System operations the emitter doesn't handle natively fall back to Flycast's
canonical SHIL interpreter via an import.

Native coverage is 62 of 70 SHIL ops, which in practice is a **100% native
runtime ratio**. The residual fallback calls (about 140 per frame) are
legitimate system ops like `sync_sr` and `ifb` that *should* run in C. The
import ABI is deliberately thin: 13 imports across 5 type signatures. A few ops
get dedicated thin imports (`div32u` and `div32s` return quotient and remainder
packed in an i64; `sq_pref` flushes a store queue). Others that look like
import candidates must be inline for correctness. Any op touching `sr.status`
(the divider's Q/M bits) has to be emitted against the same cached-or-context
status register its neighboring ops use, because a C import reads stale state
and gets its writes clobbered. That's why `div1` is fully inline, and why
`fsca` reads the sine table with direct f32 loads instead of calling out.

### 2.2 Register caching in WASM locals

Hot SH4 integer registers live in WASM locals for the duration of a block
(`RegCache`). A pre-scan walks the oplist and allocates a local per referenced
register, and ops read and write locals instead of doing `i32.load` and
`i32.store` against `Sh4Context`. Browser engines map WASM locals directly onto
CPU registers, so this is the difference between register-register arithmetic
and a memory round-trip per operand. The load/store path measured 3-5x slower.
Dirty locals are flushed back to the context at block exits and before any
fallback call. Float registers are not yet cached; FPU ops still do context
loads and stores each time. This is a known remaining headroom item.

### 2.3 Dispatch

The dispatch table is keyed by `(pc >> 1) & MASK`, and SH4's address mirrors
(the 0x0C/0x8C/0xAC regions) share entries. Compiled blocks are registered in
the Emscripten indirect function table (built with `-sALLOW_TABLE_GROWTH`), and
the C dispatch loop invokes them with `call_indirect`. A table hit costs a hash
probe and an indirect call, entirely inside WASM. A miss exits to the mainloop,
which queues the block for compilation and executes it through the SHIL
interpreter bridge in the meantime.

Two details that matter more than they look:

- **Idle-loop soft fast-forward.** When a block is a detected idle loop, its
  cycle counter is capped at 32 rather than zeroed. This fast-forwards the idle
  spin without distorting scheduler timing.
- **Async compilation.** Block compilation is asynchronous:
  `WebAssembly.compile` returns a promise, and a JS-side pump promotes finished
  modules into the table. No synchronous compile ever runs on the frame path in
  the steady state. Hot misses that *can't* wait are batched. Multiple pending
  blocks compile as one multi-function module per frame, which amortizes
  per-module overhead from about 1 ms per block to about 20 µs per block. What
  happens when even that isn't enough is §3.5 (storm mode).

---

## 3. The walls, in order

Each subsection names a wall the project hit, the mechanism that removed it, and
the measured result. The ordering is chronological, which is also roughly the
order of descending cost. Each fix exposed the next wall.

### 3.1 Self-modifying code: page generations

Dreamcast games write code into RAM constantly: loaders, overlays, unpacked
routines. A JIT must detect when the bytes under a compiled block have changed,
and the classical solutions (hardware page protection, write barriers via
`mprotect`) don't exist in WASM. The naive fallback is to hash the source bytes
of a block before executing it. That is correct and catastrophically slow. At
its peak the dispatcher was hashing **1.97 million halfwords per frame** in a
heavy 3D fighting game.

The fix is a **page-generation scheme**: one u32 generation counter per 4 KB
page of main RAM (`g_fly_page_gen[]`). Every path that can write RAM bumps the
counter for the touched pages:

- the JIT's store imports (C-side),
- the C block-write paths (`WriteMemBlock_nommu_ptr/_dma/_sq`), covering all
  DMA and store-queue block transfers,
- and the *emitted* inline stores (see below). This one took the longest to
  make exact.

Each compiled block records the generation sum of the pages it covers. The
dispatch hit path re-hashes a block **only when that sum changed**, plus a
1-in-64 forced-hash insurance tick. The miss path always full-hashes, since a
miss means the block is being looked at fresh anyway. Result: **98.3% of
dispatch hashing eliminated**, from 1.97 M down to 33 K hashed halfwords per
frame.

Chain-*interior* guards (§3.2) got the same treatment: per-member
baseline-versus-current generation cells with a staggered 1/64 insurance hash.
That collapsed chain-side hashing by about 97%, from 500 K to roughly 10 K
halfwords per frame. It was also the remaining performance killer in Jet Grind
Radio.

**The emitted gen bump, and how it was certified.** The emitter's inline RAM
fast paths (§3.3) store directly into linear memory, bypassing the C write
path. That means they'd bypass the generation bump too, leaving ordinary CPU
stores invisible to SMC detection. Only the insurance tick would catch them,
which allows up to 64 stale executions. The fix is to emit the generation bump
*inline* next to each fast-path store. The contract is exact rather than
approximate because of an SH4 architectural fact: 1/2/4-byte stores are
alignment-constrained and can never straddle a 4 KB page, so a single-cell bump
per store is precisely equivalent to what the C path does. (The C path's
second-page handling exists only for block and DMA ranges.) Measured cost is
about 0.2 ms/frame gross, repaid by retiring the insurance tick for CPU stores.

Certifying this required a purpose-built differential, because every existing
validation mode forced writes through imports and therefore never exercised the
production write path. That gap had existed since February. The **write-parity
harness** inverts the usual order. The reference interpreter runs first with
its writes logged and undone. Then the compiled WASM runs with writes inline,
exactly as production ships them, and stays live. The comparison covers three
things: the full register context; the reference's write set replayed as a
byte-map against final memory, which catches missing or wrong-valued inline
stores; and the certification target, that every written RAM page's generation
cell must have changed across the JIT run. Final result: **20,480 blocks
compared, 0 diffs, 0 generation misses**. The emitted bump is exact, and the
insurance tick is now formally redundant for CPU stores.

### 3.2 Multi-block chaining

Single-block dispatch pays a table probe and an indirect call per basic block.
That is tolerable, but hot loops of 3-6 small blocks spend a large fraction of
their time in dispatch overhead, and the register cache dies at every block
boundary.

**Chains** compile statically-connected runs of blocks into a single WASM
module. Block A's static branch to block B becomes a WASM branch, and the
chain's blocks execute back-to-back without returning to the dispatcher. Chain
heads are primed into the dispatch table, so entering a chain costs the same as
entering a block.

The machinery around chains is where most of the correctness lessons live:

- **Staleness invalidation.** A chain bakes in references to its member blocks'
  oplists at compile time. Flycast replaces blocks routinely, through
  hash-collision recompiles and FPSCR-mode-differing redecodes. A chain holding
  a replaced member executes against a dead oplist: wrong fallback indices,
  wrong exits, wrong cycle charges. The fix is member fingerprints (pc, block
  pointer, op count, guest cycles, block type) plus `fly_chain_invalidate()`
  called at **every** block replacement and eviction site. Before this existed,
  the differential farm measured roughly 1,200 would-have-corrupted chains per
  5-minute run. That was the historical "chain freeze" mechanism, finally
  named.
- **Decode-freshness guard.** A member's stored SMC hash must match live RAM at
  the moment the chain is installed. The background sweep must never bless a
  stale decode.
- **Exclusions.** Idle loops stay out of chains, because the soft fast-forward
  cap can't apply inside a chain. So do store-queue and prefetch blocks: their
  side effects make them unvalidatable by differential re-execution, and what
  can't be validated doesn't ship.
- **Async pipeline with adaptive burst.** Chain discovery and compilation run
  off the hot path, and compiled chains are promoted only after fingerprint and
  decode-freshness re-validation *at promotion time*. Sweep budgets adapt:
  burst mode (512 scans, 64 builds, 2 ms per frame) while the working set is
  un-chained, then a trickle once caught up. This fixed rebuild starvation at
  scene transitions. A fight-scene switch invalidates about 7,000 chains at
  once, and rebuilding them took 10 reporting windows before bursting, 1
  after.

### 3.3 FASTMEM: the memory-import wall

With hashing, dispatch, and coverage all fixed, heavy scenes were still over
frame budget. A careful measurement chain (real content, production builds,
bracketed timers) eliminated every suspect except one: the mainloop window
itself. Dormant memory-density counters then split it open. Fight scenes in the
heaviest title were driving **roughly 450-500 K WASM-to-C import crossings per
frame** (206-248 K reads plus 210-248 K writes). At the measured 30-40 ns per
crossing, that is **13-18 ms per frame of pure boundary overhead**. The entire
performance wall was call glue.

An instructive dead-end resurrection: import-crossing density had been
investigated and *falsified* as the bottleneck back in February. Correctly so,
at the time, because SMC hashing dominated everything. Once hashing was fixed,
the falsified suspect quietly became the dominant cost. Dead-ends have a shelf
life. Re-measure when the landscape changes.

The fix is classic fastmem, adapted to WASM's constraint set. There are no page
faults to trap on, so every fast path needs an explicit guard:

- **Inline RAM fast paths for loads.** The emitted shape: physical-address
  computation, then a range check that the address lands in main RAM (area 3,
  which for the 8-byte case is `((phys | (phys+4)) >> 26) == 3`, exact at the
  64 MB boundary), then a direct linear-memory load. Anything failing the check
  falls back to the import (MMIO, VRAM, BIOS), with the unmasked address
  preserved for the fallback's benefit.
- **The size-8 discovery.** The 1/2/4-byte fast paths had existed since
  February. The crossings were almost entirely `shop_readm` size==8: FPSCR.SZ=1
  float-*pair* loads, each previously costing two 32-bit import calls. An
  emitted counter measured that 99% of fight-content read crossings were
  size-8. Inlining them took import reads from 200-278 K down to **68-95 per
  frame**.
- **Store-queue writes inlined.** Write-side measurement showed 90-95% of write
  crossings were SH4 store-queue stores (addresses 0xE0000000-0xE3FFFFFF). The
  C handler for those is a plain masked store onto a 64-byte context buffer
  with zero side effects, so the emitter inlines it for all sizes:
  `(addr >> 26) == 0x38` routes to a store at `ctx.sq_buffer + (addr & 0x3F)`,
  with per-word masking so a straddle at 0x3C wraps to sq[0] exactly as the C
  semantics do. Ordinary RAM data writes larger than 4 bytes stay imports for
  now, since TA FIFO traffic wants C-side handling anyway.

Combined result: **from roughly 450-500 K import crossings per frame to about
9 K**. Certification came in two parts. The size-8 read path went through the
bridge-shadow differential at record volume (20,480 blocks, 0 diffs). The
inline SQ stores can't be seen by shadow builds, which force imports, so eight
new SHIL op-level test cases became the gate: banks, mirrors, sizes, wrap,
decode boundary (151/151 passing), plus an 18,432-comparison farm regression.

This was the change that produced the **first-ever locked-60 fight scenes in
the heaviest 3D title in the library**, a game that native SH4 emulators
historically struggled with.

### 3.4 Frame pacing: guest-time debt

Browser emulators inherit a pacing problem. `requestAnimationFrame` gives you
display ticks, but the guest has its own notion of time. Two mechanisms,
learned the hard way:

- **Repay guest time, not render counts.** The pacer tracks a wall-clock debt
  and repays it in *guest cycles consumed* (the SH4 scheduler's cycle counter
  divided by 200 MHz), not in frames rendered. The distinction matters because
  many Dreamcast games are 30fps-native. A render-count pacer runs them at 2x
  guest speed on a 60 Hz display. Cycle-denominated debt makes pacing
  independent of the guest's render cadence and the display's refresh rate.
- **Cap the catch-up gulp.** Upstream Flycast's render timeout lets the
  emulator run up to 50 ms of guest time waiting for the guest to present a
  frame. That is exactly 3 vblanks, and it was exactly the signature of every
  scene-transition freeze: transitions legitimately don't render for a few
  vblanks, and the emulator would grind the whole gap in one host tick. The
  timeout is lowered to about 1.5 vblanks under Emscripten, and the pacer adds
  a per-host-tick guest budget so the catch-up iteration cannot double-gulp.
  Debt that can't be repaid this tick carries forward and drains as a **brief
  slow-motion** instead of a multi-vblank freeze. Measured across matched
  6-minute A/B runs: spikes of 50 ms or more fell from 130 to 17-34, the worst
  spike from 219 to 87 ms, and total HUD-visible stall from 2816 to 233 ms.

### 3.5 Storm mode: compile-budget inversion

The remaining freeze class was cold-code storms. A scene transition invalidates
the working set, and the next frame executes hundreds of thousands of
block-runs through the interpreter bridge while the async pipeline catches up.
This measured at up to 2.35 *million* bridged executions in a single frame, a
2-second freeze.

Storm mode inverts the compile budget when it detects this (more than 2,000
bridged executions in a frame). The over-budget miss gate lifts, pending blocks
are flushed mid-frame in batches of 64 (0.2-0.4 ms per multi-function module),
and the synchronous compile budget rises from 2 ms to 50 ms for the storm
frames. The counterintuitive part is that the *high* budget is correct, and was
proven to be a floor rather than a cap. An attempt to retune it to 15 ms
measured catastrophically worse, because a starved budget leaves the actual hot
load-loop blocks bridging at 138-307 K executions per frame. Spending about
20 ms compiling in one gulp strictly beats losing 150-200 ms to the bridge.
Worst transition freeze after the full pacing and storm work: 1573 ms, then
219 ms, then 87 ms.

### 3.6 Audio: a worklet ring and honest rate control

Audio runs through an **AudioWorklet ring buffer** (jitter priming at 2048
samples, queue setpoint 4096) with a ±0.5% **tempo-based dynamic rate
control**. When the queue drifts from setpoint, the *game speed* is steered by
up to half a percent. That approach was chosen because ±0.5% is inaudible by
design, unlike resampling artifacts. Volume and mute route through a GainNode.
If worklet init fails, the sink falls back to the legacy audio callback path.

One subtlety is worth recording because it silently disabled the entire
mechanism for a week. After the pacer moved to guest-time debt, the rate trim
became **self-cancelling**: debt accrual and debt repayment were both
denominated in the *trimmed* rate, so the trim algebraically cancelled out of
the steady state, and the queue could never refill after a hitch. The fix is to
repay debt at the **untrimmed native rate**, with a saturating controller (full
±0.5% when the queue is more than 512 below setpoint, proportional inside the
band). The refill/drain asymmetry that falls out is about 30x. Max refill is
around 220 samples/s, while a throughput deficit drains at (deficit × 44,100)
per second. That means a below-setpoint queue during varied play is the
*designed* shape, not a fault. The underrun counter, not queue depth, is the
audible-failure signal.

---

## 4. Validation: farming differentials to zero

This may be the most transferable part of the project. The standing law, in
force since April:

> **Validate subsystems against the reference interpreter. Do not chase game
> bugs.**

The source of truth is upstream Flycast's SH4 interpreter, unmodified, running
in the same process. Game symptoms are manifestations, never evidence. Staring
at a glitched screen and guessing is not debugging. Every subsystem the JIT
replaced was brought up behind a **differential harness** that executes both
implementations from identical state and binary-compares the results. Nothing
ships until its harness has been farmed to zero diffs on real game content.

### The harness inventory

| Harness | What it compares | Real bugs it caught |
|---|---|---|
| SHIL op unit tests (151 cases) | per-op JIT vs canonical semantics | `cvt_f2i_t` NaN handling; certified the inline SQ-store path (8 dedicated cases) |
| **Bridge-shadow** | each compiled block vs the SHIL interpreter, same snapshot, writes logged and undone | `readF32` dropping float immediates (fallbacks computed with 0.0); `fmac` double-rounding divergence (both paths unified on f64). Fastmem size-8 certified at n=20,480 / 0 diffs |
| **Chain-shadow** | chain module vs sequential single blocks | COND both-target routing (an unverified else-arm); idle-loop links missing soft fast-forward; the single-block-only contract of inline `pref`; and stale member references, ~1,200 would-have-corrupted chains per 5-minute run |
| **Region-shadow** | multi-block region module vs sequential singles | routing and guard bugs in an experimental whole-region compiler (certified to 17,504 / 0 on live content) |
| **Write-parity** | reference-first write-set replay vs production inline stores, plus a page-gen postcondition | certified the emitted gen bump exact: 20,480 / 0 diffs / 0 gen misses |
| Mode-7 lockstep | timeslice-level production dispatch vs a clean reference loop; registers plus full write streams (RAM/VRAM/MMIO, order included) | 180 K blocks, zero divergences. Retired the dispatcher as a suspect permanently |
| Dispatch / interrupt / exception suites | table semantics, RTE/SR, jdyn and exception delivery | two SMC hazards in dispatch; 10/10, 20/20, 24/24, 5/5 |
| SingleStepTests | per-instruction architectural tests | regression parity baseline |

Three hard-won contract rules for differential harnesses:

1. **Reference-first ordering.** The experimental path's results must never
   leak into live state. Run the reference, log and undo, then run the
   candidate. (Or the reverse; the point is to pick the direction where the
   *trusted* result stays live.)
2. **Never re-run what isn't re-runnable.** MMIO reads consume state, and
   store-queue/TA activity has side effects. Blocks touching them are skipped
   and *counted*. A skip taxonomy is part of the harness output, so "clean"
   means "clean over a known population," not "clean over whatever survived."
3. **Make sure the harness exercises the production shape.** The
   force-writes-through-imports farm ran clean for months while the actual
   production inline-store path went uncertified. The write-parity mode exists
   because a differential that quietly diverges from the shipping
   configuration certifies nothing.

### Process laws

The methodology has process teeth, all of them paid for:

- **Variants are build-baked and committed.** One variable per build, no
  runtime toggles for verdicts. A runtime toggle means you can never be sure
  what you measured.
- **Human eyes are the final gate.** Headless runs check crashes, counters,
  and composition sanity only. Wall-clock behavior and audio are never tuned on
  headless timing, because a headless browser's scheduling is not your user's
  browser.
- **Measure workload, not absence-of-correctness.** Disabling a correctness
  mechanism to measure its cost corrupts the experiment. The code that runs
  without it is not the code that runs with it. (Learned by disabling SMC
  checks "just to see.")
- **Measurement content must match the felt complaint.** Menu screens idle;
  attract modes fight. An A/B measured on menu content once produced a −38.5%
  improvement that partially evaporated on real content. Real-content capture
  is the standard, and menu numbers are sanity-only.
- **A verdict on an unverified build is void by construction.** A dark feature
  once spent two evaluation rounds "showing no improvement" because a
  build-flag interaction meant it had never engaged at all. Eyes-builds for
  dark features must first prove *engagement* via a production-active signal.
- **Dead-ends carry timestamps.** The February import-density falsification
  was correct in February and wrong in July. Re-open retired suspects when the
  cost landscape shifts.

---

## 5. What's not done

- **Windows CE titles** (Sega Rally 2, Tomb Raider IV, Resident Evil 2 and
  3, and others). These enable the SH4 MMU, and the JIT currently has zero MMU
  awareness: wrong exception PCs (no per-op PC materialization), register
  cache lost on mid-block TLB-miss throws, fastmem bypassing translation. The
  full campaign plan is documented in
  [`docs/wince-mmu-campaign.md`](docs/wince-mmu-campaign.md): the root-cause
  chain, a go/no-go test for C++ exception unwinding through JIT WASM frames,
  a correct-but-slow phase, and an inline FAST_MMU LUT phase modeled on
  Flycast's x64 backend. Anyone extending this work should start there. It is
  written as a roadmap for a successor.
- **Small parked cosmetics.** A class of titles renders blank (the PVR render
  start is never issued, plausibly SMC-related, under investigation). One
  title has a menu gradient glitch, another shows translucency-sorting
  residue, and one title hard-crashes at boot in the full library sweep (22 of
  29 titles rendering at last baseline).
- **Remaining headroom items**, in researched priority order: whole-region
  compilation (built and farm-certified, currently dark pending live
  validation), an IndexedDB per-title compiled-module byte cache so second
  sessions boot pre-warmed, and float-register caching.

---

## 6. Credits & license

- **Flycast** by [flyinghead](https://github.com/flyinghead/flycast) is the
  upstream emulator this project builds on. GPLv2. All of the SH4 decoder,
  SHIL IR, PowerVR2 renderer, AICA, and GD-ROM emulation is upstream's work.
  This project adds a WebAssembly backend and browser integration to it.
- **WebAssembly port and SH4-to-WASM JIT** by Nick Somers.
- **[EmulatorJS](https://emulatorjs.org/)** provides the browser frontend
  (loader, input, save states).
- Prior art studied, none of it Dreamcast: CheerpX (x86-to-WASM, commercial),
  v86, and a Rust nullDC WASM experiment.

This project is licensed under **GPLv2**, matching upstream.

---

*Part 1, the original February port (build system, linking, WebGL2
compatibility, EmulatorJS integration, all 30+ bugs), is preserved in
[`TECHNICAL_WRITEUP.md`](TECHNICAL_WRITEUP.md).*
