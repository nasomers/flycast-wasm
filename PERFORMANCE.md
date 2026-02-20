# Performance Optimization Roadmap

## Current State

Flycast WASM runs as a **pure SH4 interpreter** (`TARGET_NO_REC`). No JIT, no dynarec. Every SH4 instruction is decoded and executed one at a time in C, compiled to WASM. GPU rendering is offloaded to WebGL2 — the 3D pipeline works well. The bottleneck is **CPU emulation throughput**.

**Current build flags:**
- Compiler: `-O2` (moderate optimization)
- Threading: `NO_THREADS`, `SINGLE_THREAD := 1`
- SIMD: not enabled
- Memory: `INITIAL_MEMORY=67108864` (64MB), growth enabled, max 2GB
- Assertions: `-s ASSERTIONS=2` (debug level)

**Observed performance (AMD 9800X3D @ 5GHz):**
| Category | Example | Performance |
|----------|---------|-------------|
| Arcade/racing (GPU-heavy) | 18 Wheeler | Near-perfect |
| Action/platformer (balanced) | Jet Set Radio | Low FPS, playable |
| Sports (CPU-heavy) | Dave Mirra BMX | Playable after spam fix |
| FMV intros | Various | Very slow |

---

## Tier 1: Low-Hanging Fruit (Single Rebuild)

These can all be applied in one rebuild cycle. Expected combined improvement: **15-30%**.

### 1a. Compiler Optimization: `-O3`

**Current:** `-O2` — moderate inlining, basic loop optimization.
**Target:** `-O3` — aggressive inlining, loop vectorization, branch prediction hints.

The SH4 interpreter dispatch loop is the hottest code path. More aggressive inlining of instruction handlers into the dispatch loop could eliminate function call overhead on every emulated instruction.

**Risk:** Slightly larger WASM binary, rare miscompilation edge cases.
**Effort:** Change one flag in link.sh + recompile all .o files.

### 1b. Link-Time Optimization: `-flto`

**Current:** Each .cpp file compiled independently, linked at WASM level.
**Target:** `-flto` on both compile and link — compiler sees all code as one unit.

LTO allows cross-module inlining. The interpreter loop in one file can inline instruction handlers from another. This is where the biggest single-flag gain comes from.

**Risk:** Slower build times, larger intermediate files.
**Effort:** Add `-flto` to both CFLAGS/CXXFLAGS and LDFLAGS. Full rebuild required.

### 1c. Disable Debug Assertions

**Current:** `-s ASSERTIONS=2` (full debug assertions).
**Target:** `-s ASSERTIONS=0` (production).

Debug assertions add runtime checks on every memory access and function call. Removing them eliminates per-operation overhead.

**Effort:** Change one flag.

### 1d. Pre-Allocated Memory: 256MB Initial

**Current:** `INITIAL_MEMORY=67108864` (64MB), grows on demand.
**Target:** `INITIAL_MEMORY=268435456` (256MB).

WASM memory growth is expensive — each `memory.grow` operation can cause a full memory copy. Pre-allocating 256MB eliminates growth stalls during the critical first seconds of emulation (BIOS init, game loading).

Dreamcast has 16MB main + 8MB VRAM + 2MB sound = 26MB hardware RAM. Flycast's total footprint with buffers and caches is much larger.

**Effort:** Change one flag.

### 1e. Remove Debug Trace Statements

The patched `libretro.cpp` and `nullDC.cpp` contain `[TRACE]` `fprintf`/`log_cb` calls added during debugging. These should be removed for production builds — each is a synchronous syscall in WASM.

**Effort:** Strip TRACE lines from patch, recompile 2 files.

---

## Tier 2: WASM SIMD (Full Recompile)

### 2a. Enable WASM SIMD: `-msimd128`

**Current:** Scalar operations only.
**Target:** 128-bit SIMD instructions (supported in all modern browsers since 2021).

Flycast's texture processing, audio mixing, vertex transformation, and some SH4 floating-point operations can benefit from SIMD. Emscripten's auto-vectorizer will pick up suitable loops.

**Expected improvement:** 10-20% on specific hot paths (texture uploads, audio, FPU-heavy games).

**Risk:** Requires ALL .o files recompiled with `-msimd128`. The `.a` archive must be rebuilt from scratch — can't just relink.

**Effort:** Add `-msimd128` to all compile commands. Full rebuild of Flycast + relink.

**Browser support:** Chrome 91+, Firefox 89+, Edge 91+, Safari 16.4+. Electron 20+.

---

## Tier 3: Threaded Rendering (Biggest Potential Gain)

### 3a. Enable pthreads + Threaded Rendering

**Current:** Single-threaded. CPU emulation and GL rendering run sequentially on the main thread.
**Target:** Separate CPU and GPU threads via Web Workers + SharedArrayBuffer.

This is the **single biggest potential improvement**. Flycast's architecture supports threaded rendering — the SH4 CPU produces render commands that the PowerVR2 GPU consumes asynchronously. On native builds, this is the default mode and it roughly doubles throughput.

**Changes required:**
```makefile
# Makefile
SINGLE_THREAD := 0
HAVE_THREADS = 1
CFLAGS += -pthread
CXXFLAGS += -pthread
LDFLAGS += -pthread -sPROXY_TO_PTHREAD
```

```bash
# Link flags
-s USE_PTHREADS=1
-s PTHREAD_POOL_SIZE=2
-s PROXY_TO_PTHREAD=1
```

**Core option:** `reicast_threaded_rendering: 'enabled'`

**Prerequisites:**
- SharedArrayBuffer support (requires COEP/COOP headers — already present in the demo server)
- Electron or a secure context with cross-origin isolation
- EmulatorJS must not break when the WASM module spawns Web Workers

**Risks:**
- Threading bugs in Flycast's WASM build (race conditions, deadlocks)
- EmulatorJS may not expect a multi-threaded core (input/audio handling)
- Debugging threaded WASM is significantly harder
- Larger WASM binary (pthread support adds ~50KB)

**Effort:** Substantial. Requires full Flycast rebuild with pthread flags, RetroArch relink with thread support, and testing for stability.

---

## Tier 4: Core Options Tuning (No Rebuild)

These are pure configuration changes — no recompilation needed.

### 4a. Lower Internal Resolution

**Current:** `reicast_internal_resolution: '640x480'` (2x native).
**Target:** `'320x240'` (native Dreamcast resolution).

Halves the GPU workload (quarter the pixels). Visually blurrier but significantly faster for GPU-bound scenes.

### 4b. Frame Skipping Tuning

**Current:** `reicast_frame_skipping: 'enabled'`.
Flycast's frame skip drops rendering on some frames, reducing GPU load. Already enabled.

Could experiment with `reicast_framerate: 'fullspeed'` for uncapped mode if audio sync allows.

### 4c. Alpha Sorting

**Current:** `'per-strip (fast, least accurate)'` — already the fastest option.

Per-pixel alpha sorting is much more GPU-intensive. Already on the fastest setting.

---

## Tier 5: Moonshots (Research-Level)

### 5a. Ahead-of-Time SH4 → WASM Translation

Instead of interpreting SH4 at runtime, pre-translate game code to WASM modules. This is essentially writing a static recompiler that targets WASM bytecode instead of x86/ARM machine code.

**Impact:** 10-50x for CPU-bound code. Would make the interpreter bottleneck disappear entirely.
**Effort:** Weeks to months. Research-level project. No known prior work.

### 5b. Upstream Flycast Port

The upstream `flyinghead/flycast` repo is actively maintained with better GLES3 support, performance improvements, and bug fixes. But it uses CMake exclusively and has zero Emscripten support.

Porting the Emscripten changes to upstream would give access to years of improvements. But it's a different codebase with different architecture.

**Effort:** Days to weeks. Requires familiarity with both codebases.

### 5c. Interpreter Optimization

The SH4 interpreter dispatch loop compiles to a WASM `br_table` (computed branch). The efficiency of this varies by browser engine. Potential optimizations:

- Emscripten's `-s BINARYEN_EXTRA_PASSES` for WASM-level optimization
- Manual interpreter loop restructuring (switch → function table)
- Profile-guided optimization (PGO) if Emscripten supports it

### 5d. WebGPU Renderer

WebGPU is the successor to WebGL, offering lower overhead and compute shaders. If Flycast's Vulkan renderer were adapted to WebGPU, it could be significantly faster than the GLES→WebGL2 path.

**Timeline:** WebGPU is available in Chrome 113+, but Flycast would need a new renderer backend.

---

## Priority Order

For maximum impact with minimum effort:

1. **Tier 1 (all at once)** — Single rebuild, 15-30% improvement
2. **Tier 4a** — Zero effort, trade visual quality for speed
3. **Tier 2** — Full rebuild, 10-20% on specific paths
4. **Tier 3** — Most complex, potentially largest gain
5. **Tier 5** — Future research directions

## Measurement

Before optimizing, establish baselines:

- **FPS counter:** EmulatorJS has a built-in FPS overlay
- **Frame timing:** `performance.now()` around `requestAnimationFrame` callbacks
- **CPU profiling:** Chrome DevTools Performance tab (WASM is profiler-friendly)
- **Specific test cases:** Use the same game, same scene for A/B comparisons
  - 18 Wheeler: race start (GPU-bound baseline)
  - Jet Set Radio: title screen demo (balanced)
  - Game with FMV intro (CPU-bound stress test)
