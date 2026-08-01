# WinCE / MMU roadmap: full SH4 MMU support in the WASM JIT

**Status:** researched and planned, not implemented. This is the one big
missing feature, and this document is a complete implementation plan for
whoever picks it up.
**Goal:** Windows CE titles (Sega Rally 2, Tomb Raider IV, Resident Evil 2
and 3, Hidden & Dangerous, and friends) boot and run at native performance
under the WASM JIT.
**Estimate:** three phases plus a front-loaded go/no-go experiment. Roughly
4 to 8 weeks of focused work.

---

## 1. Symptom and crash chain (verified, with file and line references)

WinCE titles crash back to the frontend with an SH4 fatal exception. The
chain:

1. The WinCE kernel enables address translation (MMUCR.AT=1).
2. `mmu_set_state()` (`core/hw/sh4/modules/mmu.cpp:440-460`) finds the
   literal `"SH-4 Kernel"` magic in RAM, sets `mmuOn = true`, and enables
   full MMU emulation. This detection fires only for WinCE titles, which is
   why nothing else is affected.
3. `SetMemoryHandlers()` (`core/hw/sh4/sh4_mem.cpp:346-358`) swaps
   `ReadMem*/WriteMem*` to `mmu_ReadMem/mmu_WriteMem`, which throw
   `SH4ThrownException` on TLB miss (`mmu_raise_exception`,
   `mmu.cpp:154-159`). TLB misses are routine control flow under WinCE,
   thousands per second.
4. The WASM JIT keeps running (`FEAT_SHREC = DYNAREC_JIT` on Emscripten,
   `build.h:28-33`; nothing forces the interpreter) and mishandles these
   exceptions (see section 2). State corrupts. Eventually `Do_Exception`
   fires while `SR.BL=1`, producing
   `FlycastException("Fatal: SH4 exception when blocked")`
   (`sh4_interrupts.cpp:233`).
5. `retro_run` catches it and calls `RETRO_ENVIRONMENT_SHUTDOWN`, which the
   frontend surfaces as a crash screen.

## 2. Root causes in the WASM backend (rec-wasm has zero mmu references)

Three independent, fatal defects, ranked:

1. **Wrong exception PC.** `mmu_raise_exception` throws with
   `epc = ctx.pc - 2`, which requires ctx.pc to be current at every
   instruction. Generated blocks never store ctx.pc mid-block; only
   `shop_ifb` materializes PC (`wasm_emit.h`). Every TLB miss therefore
   delivers a stale epc, and the WinCE refill handler RTEs to the wrong
   address.
2. **Register cache lost on mid-block throw.** SH4 registers live in wasm
   locals and are written back only at block exits; only `shop_ifb` flushes
   before calling out. A throw from a memory import abandons the frame with
   all dirty cached registers still in locals, so the exception handler runs
   on stale r0 through r15, sr, and pr. Unrecoverable by itself.
3. **Fastmem bypasses translation.** The inline memory fast path fires when
   `(addr & 0x1FFFFFFF) >> 26 == 3`. Under MMU those are virtual addresses.
   Any vaddr that masks into area 3 reads or writes physical RAM
   untranslated: silent wrong data, no fault.

Secondary defects (these surface after the three above are fixed):

- All dispatch caches are keyed by raw vaddr with no ASID or translation
  awareness (`blockByVaddr`, the dispatch table arrays, the JS-side module
  cache, chains). WinCE remaps vaddrs per process, so a context switch can
  leave stale code mapped.
- SMC checks are skipped for exactly the blocks WinCE cares about:
  `primeDispatchEntry` stores sz=0 (meaning "skip the check") for non-area-3
  vaddrs, and `hashRamBlock` hashes `vaddr & RAM_MASK`, which is the wrong
  bytes for translated pages.
- The dev-build `wasm_execute_block` JS wrapper catches everything as a
  generic trap and would misclassify SH4 exceptions on that path.

## 3. Reference design (proven, in-tree)

rec-x64 under MMU is the template (`core/rec-x64/rec_x64.cpp`):

- Materializes `block->vaddr + op.guest_offs` before every memory op and SQ
  write.
- `genMmuLookup`: an inline FAST_MMU LUT probe before every access.
- `shop_ifb` goes to `interpreter_fallback` with the real PC.
- A mandatory `CheckBlock` on every block entry when the MMU is enabled;
  blockcheck failure discards the block and escalates through the
  SMC-hotspot path (`driver.cpp:238-263`). This, not TLB-write
  invalidation, is how upstream survives process remaps.
- `FAST_MMU` and `USE_WINCE_HACK` are already compiled into this build
  (`build.h:4-5`). Native flycast runs WinCE titles at full speed on x64
  with this exact scheme, so the performance ceiling is known to be good.

The interpreter maintains per-instruction pc and precise exceptions. It is
the correct MMU reference for all differential validation.

## 4. Phase 0: go/no-go (about a day)

**Prove C++ exception unwinding through JIT-generated wasm frames.** The
whole MMU design is throw-based. The mainloop catch must receive
`SH4ThrownException` after it unwinds through a generated module frame, and
this is currently unproven: no shipped title ever throws from inside a JIT
frame. Build a small unit test in the style of the existing RTE test
harness: a synthetic block whose memory import throws, verifying the
exception lands in the mainloop catch with intact state, in both dev and
production Emscripten exception configurations.

**Fallback if it fails in the production config:** error-code returns from
the memory imports plus early-exit checks in generated code. About one extra
week of emitter work, and no unknowns.

## 5. Phase A: correct but slow. WinCE boots. (1 to 2 weeks)

All emitter-level work; the machinery exists:

- PC materialization plus a full register flush before every `readm`,
  `writem`, and `pref` when the MMU is enabled. This extends the existing
  `shop_ifb` pattern, as a compile-time-per-block flag exactly like rec-x64.
- Gate fastmem and SQ-fastmem off in MMU blocks. This comes free:
  `ccn.cpp` calls `ResetCache()` on the AT toggle, so every block recompiles
  under the new mode. The guest flipping the mode makes the change baked per
  block rather than a runtime toggle.
- `shop_ifb` routes to `interpreter_fallback` with the real PC under MMU.
- Block-entry code verification for translated blocks: hash via the
  translated physical address, and stop skipping SMC for non-area-3 vaddrs.
  Reuse the existing mmu blockcheck, discard, and hotspot path from
  `driver.cpp`.
- Chains and regions disabled for MMU blocks (one guard each).

**Exit criterion:** a WinCE title reaches gameplay under the JIT,
differentially validated against the interpreter with MMU on.

## 6. Phase B: stability. Context switches, SMC, remaps. (about a week)

Mostly inherited: Phase A's translated-hash block check is upstream's remap
defense. Phase B is a soak across WinCE scheduler behavior, fixing whatever
the differential harness surfaces. The existing sweep and lockstep tooling
carries over unchanged.

## 7. Phase C: performance. FAST_MMU inline. (2 to 4 weeks)

- Inline the `mmuAddressLUT` lookup in generated code (a wasm equivalent of
  `genMmuLookup`): load the LUT base, index by VPN, branch on zero. Roughly
  8 to 10 wasm ops on the hit path, no import call, no exception possible.
  Same guarded fast/slow shape the emitter already uses for fastmem.
- Flush the register cache only on the miss branch. The hit path keeps full
  register caching, preserving nearly all current per-block performance.
  Misses are rare after warmup; that is the point of the LUT.
- SQ writes: a `do_sqw_mmu_no_ex` equivalent with the PC passed in,
  mirroring rec-x64.
- Re-enable chains for MMU blocks if profiling justifies it.

The performance ceiling is known-good: native flycast runs these titles at
full speed with the same scheme.

## 8. Stopgap (optional, a few hours)

On `mmuOn` detection, fall back to the interpreter executor and show a user
notice. This turns today's crash into "WinCE runs, slowly" while the real
campaign proceeds, with zero risk to the JIT path.

## 9. Validation and test titles

- All existing harnesses reuse directly: the SHIL op tests, the differential
  farms, and the lockstep tooling, with the interpreter-plus-MMU as the
  reference.
- One new Phase 0 unit test: the throw-through-JIT-frame test, in both
  build configurations.
- Pick two WinCE titles with different stress profiles for final visual
  verification: one kernel/TLB-heavy (Tomb Raider IV is a candidate) and one
  FPU/render-heavy (Sega Rally 2). Human eyes on real gameplay remain the
  final gate.

## 10. Risks, in order

1. Phase 0 exception unwinding fails in the production exception model.
   Mitigation: the fallback design (error-code returns), about one extra
   week, no unknowns.
2. The Phase B soak surfaces WinCE-specific timing or interrupt
   interactions. This is the same class of grind as the transition and
   pacing work already done for retail titles, and the same tooling
   localizes it.
3. The per-op flush cost in Phase A makes WinCE unplayably slow before
   Phase C lands. Acceptable: Phase A is a correctness milestone, not a
   shipping state.
