# Flycast WASM

**Sega Dreamcast emulation in the browser via WebAssembly.**

The first known public build of [Flycast](https://github.com/flyinghead/flycast) compiled to WASM, running as a libretro core inside [EmulatorJS](https://emulatorjs.org). February 2026.

[![Flycast WASM Demo — Dreamcast in the Browser](https://img.youtube.com/vi/VAGoy-kjqYA/maxresdefault.jpg)](https://www.youtube.com/watch?v=VAGoy-kjqYA)

## Status: Working

Games boot with real BIOS, render via WebGL2, and play with full audio.

| Game | Performance | Notes |
|------|-------------|-------|
| 18 Wheeler | Near-perfect | Arcade-style, GPU-heavy |
| Jet Set Radio | Playable | Lower FPS, perfect audio |
| Dave Mirra Freestyle BMX | Playable | Fixed via texParameter patch |
| FMV-heavy intros | Slow | Software MPEG decode bottleneck |

Performance is limited by the SH4 interpreter (no dynarec in WASM). GPU-heavy games run well; CPU-heavy tasks (FMV, complex game logic) are the bottleneck. See [PERFORMANCE.md](PERFORMANCE.md) for the optimization roadmap.

## Why This Exists

Flycast has never officially supported WebAssembly. The upstream maintainer [explicitly declined](https://github.com/flyinghead/flycast/issues/1883) WASM support. EmulatorJS does not list Dreamcast as a supported system. The libretro buildbot does not produce Flycast WASM cores. No prior public build exists.

The deprecated `libretro/flycast` fork has a broken but structurally present Emscripten target. I fixed it. Over 30 bugs were identified and resolved across the Makefile, C/C++ source, Emscripten linker, JavaScript runtime, and EmulatorJS integration.

**Read the full technical writeup: [TECHNICAL_WRITEUP.md](TECHNICAL_WRITEUP.md)**

## Quick Start

### Use Pre-Built Core

Download `flycast-wasm.data` from [Releases](../../releases) and place it in EmulatorJS's `data/cores/` directory. You'll also need:

- Dreamcast BIOS files (`dc_boot.bin` + `dc_flash.bin`)
- The [runtime WebGL2 patches](patches/webgl2-compat.js) injected into your emulator page
- Flycast added to EmulatorJS's `requiresWebGL2` array

See [Phase 10-11 of the technical writeup](TECHNICAL_WRITEUP.md#phase-10-runtime-webgl2-patches) for integration details.

### Build From Source

Requires WSL2/Linux and Emscripten SDK 3.1.74. Full instructions in the [technical writeup](TECHNICAL_WRITEUP.md#phase-1-clone-repositories).

```bash
# Clone
git clone https://github.com/libretro/flycast.git ~/flycast-wasm/flycast
cd ~/flycast-wasm/flycast

# Patch (6 source patches across 5 files)
git apply ../patches/flycast-all-changes.patch

# Build (~2 min)
emmake make -f Makefile platform=emscripten -j$(nproc)

# Archive, link with EmulatorJS RetroArch, package as 7z
# See TECHNICAL_WRITEUP.md Phases 3-9
```

## What I Had to Fix

**6 source patches:**
1. Makefile — Emscripten platform block (zlib, exceptions, OpenMP, NO_REC, HAVE_GENERIC_JIT)
2. Makefile — HOST_CPU override (GNU Make `$(filter)` bug with empty variables)
3. `sh4_core_regs.cpp` — CPU_GENERIC floating-point rounding
4. `gles.cpp` — Force GLES3 detection (runtime `glGetString` returns garbage in WASM)
5. `libretro.cpp` — Emscripten-safe `os_DebugBreak` with stack traces
6. `nullDC.cpp` — Init sequence tracing

**3 runtime JavaScript patches** (critical for gameplay):
1. `getParameter` override — correct GL_VERSION for WebGL2
2. `getError` suppression — prevents RetroArch from aborting video init on GL_INVALID_ENUM
3. `texParameteri/f` guard — prevents console spam that causes massive lag

**Plus:** stub functions for WASM signature mismatches, libchdr FLAC collision workaround, EmulatorJS metadata schema fixes, BIOS path handling, CHD filename fixes, and more.

The [complete bug reference](TECHNICAL_WRITEUP.md#complete-bug-reference) documents all 32 issues encountered.

## Repository Structure

```
flycast-wasm/
├── README.md                           # This file
├── TECHNICAL_WRITEUP.md                # Full build guide + all 32 bugs documented
├── PERFORMANCE.md                      # Optimization roadmap
├── LICENSE                             # GPLv2
├── patches/
│   ├── flycast-all-changes.patch       # Combined source patch (git apply)
│   ├── gles-force-gles3.patch          # Individual: gles.cpp GLES3 force
│   ├── gl_override.js                  # Emscripten JS library override
│   └── webgl2-compat.js               # Runtime WebGL2 compatibility patches
├── stubs/
│   ├── flycast_stubs.c                 # WASM signature mismatch stubs (C)
│   └── flycast_stubs_cpp.cpp           # Dynarec no-op stub (C++)
├── build/
│   └── link.sh                         # Emscripten link script
└── config/
    ├── core.json                       # EmulatorJS core metadata
    ├── build.json                      # EmulatorJS build metadata
    └── dreamcast-core-options.json     # Tuned core options for WASM
```

Pre-built binaries (`flycast_libretro.js`, `flycast_libretro.wasm`, `flycast-wasm.data`) are available in [Releases](../../releases).

## License

Flycast is licensed under [GPLv2](LICENSE). This project contains patches and build tooling for the [libretro/flycast](https://github.com/libretro/flycast) fork.
