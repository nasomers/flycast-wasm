# Flycast WASM — v1 Reference (Deprecated Fork)

> Historical reference for the v1 build from deprecated `libretro/flycast` fork.
> Active work is on the upstream port (flyinghead/flycast) — see main CLAUDE.md.

## v1 Build Environment
- **Toolchain:** Emscripten SDK 3.1.74 on WSL Debian (`/home/ghost/flycast-wasm/`)
- **Source:** `libretro/flycast` (deprecated fork, NOT `flyinghead/flycast`)
- **Mode:** `TARGET_NO_REC=1` (pure SH4 interpreter, no JIT)
- **Flags:** `-O2`, `ASSERTIONS=2`, `INITIAL_MEMORY=64MB`, single-threaded, no SIMD

## Repository Structure
```
flycast-wasm/
├── README.md, TECHNICAL_WRITEUP.md, PERFORMANCE.md, LICENSE
├── patches/          # v1 source patches + JS overrides
├── stubs/            # WASM signature mismatch stubs
├── build/link.sh     # v1 link script
├── config/           # EmulatorJS core/build metadata
├── demo/server.js    # Standalone demo server
└── screenshots/
```

## 6 Source Patches (v1)
1. **Makefile** — Emscripten platform block (zlib, exceptions, OpenMP, NO_REC, HAVE_GENERIC_JIT)
2. **Makefile** — HOST_CPU override (GNU Make `$(filter)` bug with empty variables)
3. **sh4_core_regs.cpp** — CPU_GENERIC floating-point rounding
4. **gles.cpp** — Force GLES3 detection (`glGetString(GL_VERSION)` returns garbage in WASM)
5. **libretro.cpp** — Emscripten-safe `os_DebugBreak` with stack traces
6. **nullDC.cpp** — Init sequence tracing

## 3 Runtime JavaScript Patches (webgl2-compat.js)
1. **getParameter override** — correct GL_VERSION string for WebGL2
2. **getError suppression** — suppress GL_INVALID_ENUM that aborts RetroArch video init
3. **texParameteri/f guard** — prevent console spam from unbound texture calls

## Critical Build Knowledge (v1)
- Link order: stubs BEFORE RetroArch objects
- file_path.o stripped from Flycast archive (duplicate symbol)
- emsdk 3.1.74 produces .o files, NOT .bc bitcode — use `emar rcs`
- gl_override.js is Emscripten `--js-library` override
- BIOS path: system_directory = `/`, Flycast appends `/dc/`

## Performance Optimization Roadmap (v1)
See PERFORMANCE.md. Tiers 1-5: -O3, -flto, ASSERTIONS=0, SIMD, pthreads, core options, moonshots.

## Community
- **GitHub:** https://github.com/nasomers/flycast-wasm
- **YouTube:** https://www.youtube.com/watch?v=VAGoy-kjqYA
- **EmulatorJS issue:** #670, **libretro/flycast issue:** #1210
- **Reddit:** r/emulation post (Feb 2026)

## WSL Build Path (v1)
All v1 artifacts in WSL at `/home/ghost/flycast-wasm/`:
- `flycast/` — patched source, `EJS-RetroArch/` — RetroArch fork
- `link.sh`, `test/`, built core: `flycast_libretro.js` + `.wasm`

## Demo Server
- Node.js HTTP server with COEP/COOP headers for SharedArrayBuffer
- Token-based file serving, Range request support
- Unified startGame monkey-patch: BIOS injection + core options + system_directory
- Usage: `node demo/server.js [port] [roms-dir]`

## Competitor
**nullDC WASM** (emudev-org/nullDC) — Rust rewrite at nulldc.emudev.org. Sept 2025, 39 stars. Different codebase.
