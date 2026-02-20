# Flycast WebAssembly: Technical Writeup

## Building the First Dreamcast Emulator for the Browser

**Date:** February 2026
**Status:** Working — games boot, render, and play with audio. Performance limited by SH4 interpreter overhead.
**Emscripten:** 3.1.74
**Source:** libretro/flycast (deprecated fork — the only one with Emscripten scaffolding)

---

## Background

Flycast is the leading Sega Dreamcast emulator. It has never officially supported WebAssembly:

- **Upstream** (`flyinghead/flycast`) uses CMake exclusively. Zero references to Emscripten, WASM, or WebAssembly. The maintainer [explicitly declined](https://github.com/flyinghead/flycast/issues/1883) WASM support (April 2025).
- **EmulatorJS** does not list Dreamcast as a supported system. Flycast is not in `cores.json`, not on the CDN.
- **Libretro buildbot** does not produce Flycast among its ~97 emscripten core builds.
- **No prior art.** No GitHub issues, forum posts, Reddit threads, or blog posts document a working Flycast WASM build.

The **deprecated** `libretro/flycast` fork has an emscripten platform target in its Makefile — broken, years stale, but structurally present. This is our starting point.

Over 30 distinct bugs were identified and fixed across the Makefile, C/C++ source, Emscripten linker configuration, JavaScript runtime environment, and EmulatorJS integration layer.

### What Works

- Real Dreamcast BIOS boot sequence (orange swirl animation)
- CHD, CDI, and GDI disc image formats
- 3D game rendering via WebGL2 (GLES3 shader path)
- Audio via Web Audio API (OpenAL)
- Keyboard and gamepad input via EmulatorJS
- CRT/post-processing effects

### What Doesn't (Yet)

- **Full-speed 3D:** SH4 interpreter only (no dynarec in WASM). Heavier 3D games run below full speed.
- **FMV playback:** Software MPEG/Sofdec decoding is CPU-intensive through the interpreter. Games with long FMV intros will be slow until gameplay starts.
- **Threaded rendering:** Currently single-threaded. Enabling pthreads + WebWorker rendering is the biggest optimization opportunity.
- **WASM SIMD:** Not yet enabled (`-msimd128`).

### Tested Games

| Game | Status | Notes |
|------|--------|-------|
| 18 Wheeler | Near-perfect | Arcade-style, GPU-heavy — ideal for interpreter builds |
| Jet Set Radio | Playable | Low FPS but functional menus and gameplay, perfect audio |
| Dave Mirra Freestyle BMX | Playable | Initially unusable due to console spam; fixed with texParameter patch |
| Games with FMV intros | Slow intros, playable gameplay | Software MPEG decode is the bottleneck, not the game engine |

---

## Architecture

```
Browser / Electron
┌──────────────────────────────────────────────────────┐
│  EmulatorJS (RetroArch WASM frontend)                │
│  ├─ RetroArch core loader + save state management    │
│  ├─ Input handling (keyboard/gamepad → libretro API) │
│  ├─ Audio output (OpenAL → Web Audio API)            │
│  └─ IDBFS persistent save storage                    │
│       │                                              │
│       ▼                                              │
│  flycast_libretro.wasm (this project)                │
│  ├─ SH4 CPU interpreter (C++, no JIT/dynarec)       │
│  ├─ PowerVR2 GPU → GLES3 → WebGL2                   │
│  ├─ AICA sound processor                             │
│  └─ GD-ROM / CHD / CDI disc image loading            │
│       │                                              │
│       ▼                                              │
│  WebGL2 + Web Audio API + WASM Memory (up to 2GB)    │
└──────────────────────────────────────────────────────┘
```

**Key constraint:** WASM cannot generate and execute code at runtime. Flycast's SH4 dynamic recompiler is disabled (`TARGET_NO_REC`). All CPU emulation runs through the C++ interpreter — instruction by instruction. GPU rendering is offloaded to WebGL2 via the GLES3 shader path, which is why 3D graphics still look correct. But CPU-bound tasks (FMV decoding, complex game logic, audio mixing) are the throughput bottleneck.

---

## Prerequisites

- WSL2 (Debian or Ubuntu) or native Linux
- Emscripten SDK 3.1.74 (must match EmulatorJS's expected version)
- `build-essential`, `git`, `python3`, `p7zip-full`

```bash
sudo apt update && sudo apt install -y build-essential git python3 p7zip-full

git clone https://github.com/emscripten-core/emsdk.git ~/.emsdk
cd ~/.emsdk && ./emsdk install 3.1.74 && ./emsdk activate 3.1.74
source ~/.emsdk/emsdk_env.sh
```

Verify: `emcc --version` should show `3.1.74`.

---

## Phase 1: Clone Repositories

```bash
mkdir ~/flycast-wasm && cd ~/flycast-wasm

# Flycast libretro fork (deprecated — only repo with emscripten Makefile)
git clone https://github.com/libretro/flycast.git

# EmulatorJS RetroArch fork (must use 'next' branch)
git clone https://github.com/EmulatorJS/RetroArch.git --depth 1 --branch next EJS-RetroArch
```

---

## Phase 2: Patch Flycast Source

The Flycast Makefile's emscripten platform block (around line 862) is broken in multiple ways. Six source patches are required across 5 files. A combined diff is provided in `patches/flycast-all-changes.patch`.

### Patch 1: Makefile — Fix the Emscripten Platform Block

Replace the existing `else ifeq ($(platform), emscripten)` block with:

```makefile
else ifeq ($(platform), emscripten)
  EXT       ?= bc
  TARGET := $(TARGET_NAME)_libretro_$(platform).$(EXT)
  FORCE_GLES := 1
  WITH_DYNAREC=
  CPUFLAGS += -Dasm=asmerror -D__asm__=asmerror -DNO_ASM -DNOSSE
  INCFLAGS += -I$(CORE_DIR)/core/deps/zlib
  CFLAGS += -DTARGET_NO_EXCEPTIONS -DTARGET_NO_REC -include unistd.h
  CXXFLAGS += -DTARGET_NO_EXCEPTIONS -DTARGET_NO_REC
  SINGLE_THREAD := 1
  HAVE_OPENMP = 0
  PLATCFLAGS += -Drglgen_resolve_symbols_custom=reicast_rglgen_resolve_symbols_custom \
            -Drglgen_resolve_symbols=reicast_rglgen_resolve_symbols
  NO_REC=1
  HAVE_GENERIC_JIT = 0
  PLATFORM_EXT := unix
```

| Change | Why |
|--------|-----|
| `INCFLAGS += -I$(CORE_DIR)/core/deps/zlib` | Fixes `zlib.h` not found — Flycast bundles zlib headers |
| `CFLAGS += -DTARGET_NO_EXCEPTIONS` | Disables POSIX signal handlers (no signals in WASM) |
| `CFLAGS += -DTARGET_NO_REC` | Disables SH4 JIT recompiler (no runtime code gen in WASM) |
| `-include unistd.h` | Provides `getpid()` declaration needed by some translation units |
| `CXXFLAGS += -DTARGET_NO_EXCEPTIONS -DTARGET_NO_REC` | CPUFLAGS don't reach the C++ compiler — these must be in CFLAGS/CXXFLAGS directly |
| `HAVE_OPENMP = 0` | Emscripten doesn't support OpenMP (`omp.h` not found) |
| `NO_REC=1` (was `0`) | Disables recompiler at the Makefile conditional level |
| `HAVE_GENERIC_JIT = 0` | Disables `rec_cpp.cpp` which references unavailable SHIL opcodes |

### Patch 2: Makefile — Force HOST_CPU for Emscripten

Add after the `WITH_DYNAREC` detection chain (around line 946, after all `ifeq ($(WITH_DYNAREC), ...)` blocks):

```makefile
# Fix: emscripten must use CPU_GENERIC (0x20000005)
# Without this, GNU Make's $(filter) evaluates ifeq (,) as TRUE
# when WITH_DYNAREC is empty, causing ARM64 code to compile
ifeq ($(platform), emscripten)
HOST_CPU_FLAGS = -DHOST_CPU=0x20000005
endif
```

**Root cause:** When `WITH_DYNAREC=` (empty), GNU Make's `$(filter $(WITH_DYNAREC), arm64 aarch64)` returns empty. Then `ifeq ($(WITH_DYNAREC), $(filter ...))` evaluates as `ifeq (,)` which is TRUE. The ARM64 block is evaluated last, so it wins, setting `HOST_CPU=CPU_ARM64`. ARM64 inline assembly then crashes the Emscripten compiler.

### Patch 3: `core/hw/sh4/sh4_core_regs.cpp` — CPU_GENERIC Floating-Point

The `setHostRoundingMode()` function has platform-specific implementations for x86, ARM, and ARM64 — but no `CPU_GENERIC` case. The fallback is `#error "Unsupported platform"`.

After the includes, add:
```cpp
#if HOST_CPU == CPU_GENERIC
#include <cfenv>
#endif
```

In `setHostRoundingMode()`, before the final `#endif`, replace the `#error` with:
```cpp
#elif HOST_CPU == CPU_GENERIC
        // Use C99 fenv for generic/WASM platforms
        if (fpscr.RM == 1)
            std::fesetround(FE_TOWARDZERO);
        else
            std::fesetround(FE_TONEAREST);
        // Denormal flush-to-zero not available via standard C — skip for WASM
```

### Patch 4: `core/rend/gles/gles.cpp` — Force GLES3 Detection

**This is the most critical rendering patch.** Flycast's `findGLVersion()` (line ~413) calls `glGetString(GL_VERSION)` to detect the GL profile. In WASM, this returns garbage (`"  #endif"`) because Emscripten's GL context isn't fully initialized when the string is queried through `get_proc_address` function pointers. Flycast sees the garbage, fails the `"OpenGL ES"` prefix check, and selects desktop GL3 shaders (`#version 130`). WebGL2 only accepts GLES shaders (`#version 300 es`), so all shader compilation fails silently and the screen is black.

After `const char *version = (const char *)glGetString(GL_VERSION);` and `NOTICE_LOG(...)`, add:

```cpp
#ifdef __EMSCRIPTEN__
    // Force GLES3 on WebGL2 — runtime glGetString returns garbage in WASM.
    // The GL version string passes through Emscripten's get_proc_address callback
    // which caches results incorrectly. Hardcode the correct values.
    gl.is_gles = true;
    gl.gl_major = 3;
    gl.gl_minor = 0;
    gl.gl_version = "GLES3";
    gl.glsl_version_header = "#version 300 es";
    gl.single_channel_format = GL_ALPHA;
    {
        GLint stencilBits = 0;
        glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
        if (stencilBits == 0)
            gl.stencil_present = false;
    }
#else
```

And add `#endif // __EMSCRIPTEN__` after the existing version detection block's closing brace.

**Why three layers of GL_VERSION patching?** Different code paths read the GL version through different mechanisms:

1. **Emscripten `--js-library` override** (`gl_override.js`) — intercepts `_glGetString` at the Emscripten symbol level, catching RetroArch's GL driver calls
2. **WebGL2 `getParameter` runtime patch** — intercepts direct `ctx.getParameter(GL_VERSION)` calls at the browser API level
3. **Source code patch** (this one) — bypasses both JS layers entirely, because Flycast's renderer gets its GL function pointers through RetroArch's HW render callback (`get_proc_address`), which can resolve to different implementations than the ones we patched

All three are necessary. Removing any one causes black screen for specific code paths.

### Patch 5: `core/libretro/libretro.cpp` — Emscripten-Safe Debug Break

Replace the `os_DebugBreak()` implementation with an Emscripten-safe version:

```cpp
void os_DebugBreak(void)
{
    ERROR_LOG(COMMON, "DEBUGBREAK!");
#ifdef __EMSCRIPTEN__
    emscripten_log(EM_LOG_ERROR | EM_LOG_C_STACK | EM_LOG_DEMANGLE,
        "os_DebugBreak: verify/die triggered");
    abort();
#elif defined(HAVE_LIBNX)
    svcExitProcess();
#else
    __builtin_trap();
#endif
}
```

Requires `#include <emscripten.h>` at the top of the file (inside `#ifdef __EMSCRIPTEN__`).

**Why:** The original `__builtin_trap()` compiles to a WASM `unreachable` instruction — a silent crash with no stack trace, no error message. `emscripten_log` with `EM_LOG_C_STACK | EM_LOG_DEMANGLE` produces a full demangled C++ stack trace in the browser console before aborting.

### Patch 6: `core/nullDC.cpp` — Init Sequence Traces (Optional)

Debug `fprintf(stderr, "[TRACE] ...")` statements tracking `dc_init()` progress: vmem reserve, reios init, settings load, SH4 init, mem init, plugins init, mem map, dc reset.

**Note:** These are development aids. Remove for production builds to eliminate syscall overhead.

---

## Phase 3: Build Flycast Objects

```bash
cd ~/flycast-wasm/flycast
source ~/.emsdk/emsdk_env.sh

emmake make -f Makefile platform=emscripten -j$(nproc) 2>&1 | tee build.log
```

Build time: ~2 minutes on 16 threads.

This produces `.o` object files throughout the source tree. The Makefile's final "link" step produces standalone JavaScript — **ignore it**. We link separately with RetroArch.

**Critical note on `.bc` vs `.o` files:** With emsdk 3.1.74, the Makefile's `EXT ?= bc` is a historical artifact. Modern Emscripten produces WebAssembly object files (`.o` with `\0asm` magic), not LLVM bitcode. The Makefile's final link step uses `em++` which produces a standalone JS file, not an intermediate library. We skip this entirely and archive the `.o` files ourselves.

### Verify JIT Symbols Are Excluded

```bash
emnm flycast_libretro_emscripten.a 2>/dev/null | grep ngen_Compile
```

If `ngen_Compile` appears, `TARGET_NO_REC` didn't take effect. Re-check the Makefile patches.

### Archive Object Files

Create a static archive for linking, **excluding** `libretro-common/file/file_path.o`:

```bash
cd ~/flycast-wasm/flycast
find . -name '*.o' -type f | grep -v 'libretro-common/file/file_path.o' | xargs emar rcs flycast_libretro_emscripten.a
```

**Why exclude `file_path.o`?** Flycast bundles its own `file_path.o` from libretro-common. It contains `fill_pathname` which returns `void`. RetroArch's version returns `size_t`. On native platforms, this mismatch is harmless — the return value is just ignored. **In WASM, calling a function with a mismatched signature is an instant `unreachable` trap** — a silent, unrecoverable crash with no error message. The linker warns (`wasm-ld: warning: function signature mismatch: fill_pathname`) but the warning is easy to miss.

**This step must be repeated after every `emmake make` that rebuilds the archive.**

---

## Phase 4: Build RetroArch Objects

```bash
cd ~/flycast-wasm/EJS-RetroArch
source ~/.emsdk/emsdk_env.sh

emmake make -f Makefile.emulatorjs \
  HAVE_7ZIP=1 HAVE_CHD=1 HAVE_THREADS=0 PTHREAD_POOL_SIZE=0 \
  ASYNC=1 HAVE_OPENGLES3=1 STACK_SIZE=4194304 INITIAL_HEAP=268435456 \
  TARGET=flycast_libretro.js -j$(nproc)
```

This builds RetroArch's own `.o` files in `obj-emscripten/`.

---

## Phase 5: Create Stub Functions

Flycast references functions that either conflict with RetroArch's versions or don't exist in the EJS-RetroArch libretro-common.

Create `~/flycast-wasm/flycast_stubs.c`:

```c
#include <string.h>
#include <stdlib.h>

/* fill_short_pathname_representation
 * Flycast calls this, but it internally calls fill_pathname which has a
 * void vs size_t return signature mismatch between Flycast and RetroArch.
 * In WASM, signature mismatches are instant unreachable traps — not warnings.
 * This stub provides a safe implementation that avoids the problematic call chain.
 */
void fill_short_pathname_representation(char* out_rep, const char *in_path, size_t size) {
    const char *last_slash = in_path;
    const char *p;
    for (p = in_path; *p; p++) {
        if (*p == '/' || *p == '\\')
            last_slash = p + 1;
    }
    strncpy(out_rep, last_slash, size - 1);
    out_rep[size - 1] = '\0';
}

void fill_short_pathname_representation_noext(char* out_rep, const char *in_path, size_t size) {
    char *dot;
    fill_short_pathname_representation(out_rep, in_path, size);
    dot = strrchr(out_rep, '.');
    if (dot)
        *dot = '\0';
}

/* fill_pathname — must match RetroArch's signature (returns size_t, not void) */
size_t fill_pathname(char *out_path, const char *in_path, const char *replace, size_t size) {
    strncpy(out_path, in_path, size - 1);
    out_path[size - 1] = '\0';
    char *dot = strrchr(out_path, '.');
    if (dot)
        *dot = '\0';
    if (replace) {
        size_t len = strlen(out_path);
        strncpy(out_path + len, replace, size - len - 1);
        out_path[size - 1] = '\0';
    }
    return strlen(out_path);
}
```

Create `~/flycast-wasm/flycast_stubs_cpp.cpp`:

```cpp
/* bm_Reset — SH4 dynarec block manager reset, not applicable in interpreter mode */
extern "C" void bm_Reset() { /* no-op */ }
```

Compile:

```bash
cd ~/flycast-wasm
source ~/.emsdk/emsdk_env.sh

emcc -c -O2 flycast_stubs.c -o flycast_stubs.o
em++ -c -O2 flycast_stubs_cpp.cpp -o flycast_stubs_cpp.o
```

---

## Phase 6: Create the GL Override Library

Emscripten's `glGetString` implementation can return garbage through its GL caching layer, especially when accessed via `get_proc_address` function pointers. This JS library overrides `_glGetString` at the Emscripten symbol level.

Create `~/flycast-wasm/gl_override.js`:

```javascript
mergeInto(LibraryManager.library, {
  glGetString__deps: ['malloc'],
  glGetString: function(name) {
    if (typeof GL === 'undefined') GL = {};
    if (typeof GL.stringCache === 'undefined') GL.stringCache = {};
    if (typeof GL.stringCache[name] === 'number') return GL.stringCache[name];
    var str = null;
    if (name === 0x1F02) {        // GL_VERSION
      str = 'OpenGL ES 3.0 WebGL 2.0';
    } else if (name === 0x8B8C) { // GL_SHADING_LANGUAGE_VERSION
      str = 'OpenGL ES GLSL ES 3.00';
    } else {
      var ctx = (typeof GL.currentContext === 'object' && GL.currentContext)
        ? GL.currentContext.GLctx : null;
      str = ctx ? (ctx.getParameter(name) || '') : '';
    }
    var buf = _malloc(str.length + 1);
    stringToUTF8(str, buf, str.length + 1);
    GL.stringCache[name] = buf;
    return buf;
  }
});
```

**Implementation notes:**
- Uses `_malloc` + `stringToUTF8` (not `allocateUTF8` which was deprecated in some Emscripten versions)
- Uses Emscripten's internal `GL.currentContext.GLctx` object to access the real WebGL context for non-version queries
- Caches results in `GL.stringCache` (Emscripten's standard cache location) to avoid repeated allocations
- Declares `['malloc']` as a dependency so the linker includes `_malloc` in exports
- No `!` characters anywhere — Emscripten's preprocessor escapes them

---

## Phase 7: Link the WASM Module

This is the most critical step. Link order and flags must be exact.

Create `~/flycast-wasm/link.sh`:

```bash
#!/bin/bash
set -e
cd ~/flycast-wasm
source ~/.emsdk/emsdk_env.sh

# Collect RetroArch objects, EXCLUDING:
# - libchdr objects (Flycast has its own with FLAC support — RA's lacks it)
# - file_path.o (signature mismatch — see Phase 5)
# - flycast_stubs (we provide our own)
RA_OBJS=$(find EJS-RetroArch/obj-emscripten -name '*.o' -type f \
    | grep -vE 'libchdr_chd|libchdr_cdrom|libchdr_lzma|libchdr_bitstream|libchdr_huffman|libchdr_zlib|libchdr_flac|chd_stream|LzmaEnc|LzmaDec|Lzma2Dec|Lzma86Dec|flycast_stubs' \
    | sort)

RA_COUNT=$(echo "$RA_OBJS" | wc -l)
echo "RetroArch objects: $RA_COUNT (excluding libchdr/file_path/stubs)"

# LINK ORDER MATTERS:
# 1. Stubs first — our fill_short_pathname_representation wins over RA's
# 2. RetroArch objects
# 3. Flycast archive last (fills remaining unresolved symbols)
emcc -O2 \
  -s WASM=1 \
  -s WASM_BIGINT \
  -s MODULARIZE=1 \
  -s EXPORT_NAME=EJS_Runtime \
  -s EXPORTED_FUNCTIONS='["_main","_malloc","_free","_system_restart","_save_state_info","_load_state","_cmd_take_screenshot","_simulate_input","_toggleMainLoop","_get_core_options","_ejs_set_variable","_set_cheat","_reset_cheat","_shader_enable","_get_disk_count","_get_current_disk","_set_current_disk","_save_file_path","_cmd_savefiles","_supports_states","_refresh_save_files","_toggle_fastforward","_set_ff_ratio","_toggle_rewind","_set_rewind_granularity","_toggle_slow_motion","_set_sm_ratio","_get_current_frame_count","_set_vsync","_set_video_rotation","_get_video_dimensions","_ejs_set_keyboard_enabled"]' \
  -s EXPORTED_RUNTIME_METHODS='["callMain","ccall","cwrap","UTF8ToString","stringToUTF8","lengthBytesUTF8","setValue","getValue","writeArrayToMemory","addRunDependency","removeRunDependency","FS","abort","AL"]' \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s INITIAL_MEMORY=67108864 \
  -s MAXIMUM_MEMORY=2147483648 \
  -s STACK_SIZE=1048576 \
  -s ASYNCIFY=1 \
  -s ASYNCIFY_STACK_SIZE=65536 \
  -s EXIT_RUNTIME=0 \
  -s FORCE_FILESYSTEM=1 \
  -s WARN_ON_UNDEFINED_SYMBOLS=0 \
  -s ASSERTIONS=0 \
  -s DISABLE_EXCEPTION_CATCHING=0 \
  -fexceptions \
  -Wl,--wrap=glGetString -Wl,--allow-undefined \
  -s FULL_ES3=1 -s MIN_WEBGL_VERSION=2 -s MAX_WEBGL_VERSION=2 \
  -lopenal -lidbfs.js \
  --js-library EJS-RetroArch/emscripten/library_platform_emscripten.js \
  --js-library EJS-RetroArch/emscripten/library_rwebaudio.js \
  --js-library EJS-RetroArch/emscripten/library_rwebcam.js \
  --js-library gl_override.js \
  --pre-js EJS-RetroArch/emscripten/pre.js \
  flycast_stubs.o flycast_stubs_cpp.o \
  $RA_OBJS \
  flycast/flycast_libretro_emscripten.a \
  -o flycast_libretro.js

echo "Link complete"
ls -la flycast_libretro.js flycast_libretro.wasm
```

### Link Flags Reference

| Flag | Purpose |
|------|---------|
| `MODULARIZE=1` + `EXPORT_NAME=EJS_Runtime` | EmulatorJS expects `var EJS_Runtime = (() => { ... })()` module factory pattern |
| `WASM_BIGINT` | EmulatorJS's platform JS library passes `BigInt()` to WASM i64 functions |
| `EXPORTED_FUNCTIONS` | All C functions that EmulatorJS calls via `cwrap` — includes `ejs_*`, RetroArch core functions, and memory management |
| `EXPORTED_RUNTIME_METHODS` | Must include `callMain`, `FS`, `abort`, `AL` (OpenAL audio) |
| `FULL_ES3=1` | Enable full OpenGL ES 3.0 emulation via WebGL2 |
| `MIN_WEBGL_VERSION=2` + `MAX_WEBGL_VERSION=2` | Force WebGL2 context — no WebGL1 fallback |
| `ASYNCIFY=1` + `ASYNCIFY_STACK_SIZE=65536` | Required for EmulatorJS's main loop management (RetroArch's `retro_run` is called from JS) |
| `DISABLE_EXCEPTION_CATCHING=0` + `-fexceptions` | Flycast's SH4 interpreter uses C++ exceptions for MMU fault handling — without this, `throw` becomes `abort()` |
| `-lopenal` | OpenAL audio library (Web Audio API backend) |
| `-lidbfs.js` | EmulatorJS uses IndexedDB filesystem for persistent save data |
| `--js-library gl_override.js` | glGetString override (see Phase 6) |
| `-Wl,--wrap=glGetString` | Symbol-level wrapping, belt-and-suspenders with the JS library |
| `--pre-js pre.js` | EmulatorJS initialization code injected before module start |
| `WARN_ON_UNDEFINED_SYMBOLS=0` | Some symbols are legitimately unused on WASM (dynarec stubs) |
| Link order: stubs → RA → archive | Stubs must come first to win symbol resolution. Archives are searched for unresolved symbols only — standalone `.o` files always win. |

### Why Exclude RetroArch's libchdr

RetroArch's libchdr build is missing `libchdr_flac.o`. CHD v5 disc images commonly use the `cdfl` (CD-FLAC) codec. Without FLAC support, CHD sector decompression silently returns garbage — no error, no warning, just empty data. Flycast reads empty disc sectors, gets a blank product number, and the game appears to not load.

Flycast's own libchdr (in its `.a` archive) includes full FLAC support. By excluding RetroArch's libchdr objects from the link, the linker resolves CHD symbols from Flycast's archive instead.

---

## Phase 8: Verify the Build

```bash
# Check EJS_Runtime module export
head -3 flycast_libretro.js
# Should show: var EJS_Runtime = (() => {

# Check IDBFS is linked
grep -c "IDBFS" flycast_libretro.js
# Should be >= 1

# Check no JIT symbols leaked in
grep -c "ngen_Compile" flycast_libretro.wasm || echo "Clean (no JIT)"

# Check platform library linked
grep -c "PlatformEmscriptenGetSystemInfo" flycast_libretro.js
# Should be >= 1
```

---

## Phase 9: Package for EmulatorJS

### Create Metadata Files

**`core.json`:**
```json
{
    "name": "flycast",
    "options": {},
    "extensions": ["chd", "cdi", "gdi", "cue", "bin", "zip"]
}
```

**`build.json`:**
```json
{
    "buildDate": "2026-02-20",
    "version": "1.0.0",
    "minimumEJSVersion": "4.0"
}
```

**Both metadata files must match EmulatorJS's expected schema exactly:**
- Missing `"options": {}` in core.json causes `core.options.supportsMouse` → crash on undefined
- Missing `"minimumEJSVersion"` in build.json causes `undefined.endsWith("-beta")` → crash, manifesting as a hang at 99% during core decompression

### Package as 7z

```bash
cd ~/flycast-wasm
mkdir -p pkg && cp flycast_libretro.js flycast_libretro.wasm core.json build.json pkg/
cd pkg && 7z a -mx=5 ../flycast-wasm.data flycast_libretro.js flycast_libretro.wasm core.json build.json
```

Output: `flycast-wasm.data` (~1.4MB). Place in EmulatorJS's `data/cores/` directory.

---

## Phase 10: Runtime WebGL2 Patches

**These are essential.** The WASM binary alone is not enough — three JavaScript runtime patches must be injected into the emulator page HTML **before** EmulatorJS loads. Without these, games either won't boot (black screen) or will be unplayably slow.

The patches intercept `HTMLCanvasElement.prototype.getContext` and wrap the WebGL2 context with compatibility shims.

### Patch A: `getParameter` — GL Version Override

**Problem:** Even with the source patch (Patch 4) and `gl_override.js` (Phase 6), some code paths query the WebGL2 context directly via `ctx.getParameter(GL_VERSION)` rather than through Emscripten's GL emulation layer.

**Fix:** For WebGL2 contexts, wrap `getParameter` to return proper GLES3 version strings:
```javascript
var origGetParam = ctx.getParameter.bind(ctx);
ctx.getParameter = function(pname) {
  if (pname === 0x1F02 || pname === ctx.VERSION)
    return 'OpenGL ES 3.0 WebGL 2.0';
  if (pname === 0x8B8C || pname === ctx.SHADING_LANGUAGE_VERSION)
    return 'OpenGL ES GLSL ES 3.00';
  return origGetParam(pname);
};
```

### Patch B: `getError` — GL_INVALID_ENUM Suppression

**Problem:** WebGL2 generates `GL_INVALID_ENUM` (0x500) for certain FBO operations during RetroArch's video driver initialization. These operations are valid on desktop GL but trigger errors under WebGL2's stricter validation. RetroArch's `gl2_check_error()` calls `getError()` after FBO init and **aborts the entire video driver** if any error is present. The errors are harmless — rendering works fine once initialized.

**Fix:** Wrap `getError()` to consume and discard `GL_INVALID_ENUM`:
```javascript
var origGetError = ctx.getError.bind(ctx);
ctx.getError = function() {
  var err = origGetError();
  while (err === 0x500) { err = origGetError(); }
  return err;
};
```

**Without this patch, no games will boot.** RetroArch prints `[ERROR] [GL] GL: Invalid enum` followed by `Cannot open video driver. Exiting...` and the screen stays black.

**Discovery story:** We initially added a full GL debug interceptor wrapping ~20 GL functions to diagnose shader compilation issues. This inadvertently consumed all GL errors via `getError()` calls in the wrappers, masking the `GL_INVALID_ENUM`. Games worked. When we removed the heavy interceptor for performance, the errors came back and crashed video init. The targeted `getError` wrapper above is the minimal fix — it only suppresses 0x500, passing all other errors through.

### Patch C: `texParameteri/f` — Unbound Texture Guard

**Problem:** Flycast calls `texParameteri`/`texParameterf` before binding a texture to the target. This is valid on desktop OpenGL (the GL spec allows it) but produces `GL_INVALID_OPERATION` on WebGL2. The browser logs a synchronous `console.error()` for each invalid call. During gameplay, texture-heavy games like Dave Mirra BMX produce **hundreds of these errors per frame**. The synchronous console logging — not the GL error itself — causes massive lag.

**Fix:** Check if a texture is bound before forwarding the call:
```javascript
var texBindings = {};
texBindings[ctx.TEXTURE_2D] = ctx.TEXTURE_BINDING_2D;
texBindings[ctx.TEXTURE_CUBE_MAP] = ctx.TEXTURE_BINDING_CUBE_MAP;
texBindings[ctx.TEXTURE_3D] = ctx.TEXTURE_BINDING_3D;
texBindings[ctx.TEXTURE_2D_ARRAY] = ctx.TEXTURE_BINDING_2D_ARRAY;

var origTexParameteri = ctx.texParameteri.bind(ctx);
ctx.texParameteri = function(target, pname, param) {
  var b = texBindings[target];
  if (b && !origGetParam(b)) return; // no texture bound — skip silently
  return origTexParameteri(target, pname, param);
};
// Same for texParameterf
```

**Impact:** Dave Mirra BMX went from completely unusable (slideshow) to playable. The game itself isn't particularly CPU-heavy — it was the console spam causing the lag.

### Console Noise Suppression

Wrap `console.warn` to filter harmless Emscripten noise:
- `__syscall_mprotect` — WASM has no `mprotect`, harmless no-op (~210 warnings per session)
- `"is not a valid value"` — Flycast core option warnings for values it doesn't recognize

The complete runtime patch code is in `patches/webgl2-compat.js`.

---

## Phase 11: EmulatorJS Integration

### Register Flycast as a WebGL2 Core

In EmulatorJS's `emulator.js` (or `emulator.min.js`), find the `requiresWebGL2` array and add `flycast`:

```javascript
// Before:
const requiresWebGL2 = ["ppsspp"];
// After:
const requiresWebGL2 = ["ppsspp", "flycast"];
```

Without this, EmulatorJS requests `flycast-legacy-wasm.data` (WebGL1 variant, which doesn't exist) instead of `flycast-wasm.data`.

### BIOS Requirements

Dreamcast requires two BIOS files:
- `dc_boot.bin` (2MB) — Boot ROM
- `dc_flash.bin` (128KB) — Flash memory

**Multi-BIOS problem:** EmulatorJS's `EJS_biosUrl` only accepts a single URL. For systems requiring multiple BIOS files, the additional files must be delivered separately and written to the WASM filesystem before the core boots.

**BIOS location:** Flycast expects BIOS files in `{system_directory}/dc/`, not in the root. The files must be placed at `/dc/dc_boot.bin` and `/dc/dc_flash.bin` in the WASM filesystem.

### CHD Filename Handling

EmulatorJS's `checkCompression()` function can misidentify CHD files as compressed archives, stripping the `.chd` extension when writing to the WASM filesystem. Flycast then can't find the ROM at the expected path.

Additionally, URL-encoded filenames (e.g., `Dave%20Mirra%20Freestyle%20BMX%20(USA).chd`) should be decoded before Flycast sees them.

### Debug Mode

If you need to monkey-patch EmulatorJS's `startGame` method (for BIOS relocation, filename fixes, core option injection, etc.), set `EJS_DEBUG_XX = true` before loading EmulatorJS. The minified `emulator.min.js` mangles method names, making interception impossible. Debug mode loads unminified source files.

### Core Options

Flycast's core options use the `reicast_` prefix (legacy naming from the Reicast emulator):

```json
{
  "reicast_boot_to_bios": "disabled",
  "reicast_hle_bios": "disabled",
  "reicast_threaded_rendering": "disabled",
  "reicast_synchronous_rendering": "disabled",
  "reicast_internal_resolution": "640x480",
  "reicast_mipmapping": "disabled",
  "reicast_anisotropic_filtering": "1",
  "reicast_texupscale": "disabled",
  "reicast_enable_rttb": "disabled",
  "reicast_enable_purupuru": "disabled",
  "reicast_alpha_sorting": "per-strip (fast, least accurate)",
  "reicast_delay_frame_swapping": "disabled",
  "reicast_frame_skipping": "enabled",
  "reicast_framerate": "normal"
}
```

**Option value gotchas:** Some values that seem logical are invalid:
- `reicast_anisotropic_filtering`: `"off"` is invalid → use `"1"`
- `reicast_texupscale`: `"off"` is invalid → use `"disabled"`

Invalid values are silently ignored with a console warning.

---

## Complete Bug Reference

Every bug discovered during the build, in order of encounter:

### Build-Time Bugs

| # | Bug | Symptom | Root Cause | Fix |
|---|-----|---------|-----------|-----|
| 1 | Emscripten platform block broken | Won't compile — `zlib.h` not found, `omp.h` not found, signal handler errors | Missing include paths, missing feature disables | Rewrite platform block (Patch 1) |
| 2 | ARM64 code selected on Emscripten | Compiler crashes on ARM64 inline assembly | GNU Make `$(filter)` evaluates `ifeq (,)` as TRUE when `WITH_DYNAREC` is empty | Force `HOST_CPU=0x20000005` (Patch 2) |
| 3 | Missing CPU_GENERIC float support | `#error "Unsupported platform"` in `sh4_core_regs.cpp` | No `CPU_GENERIC` case in `setHostRoundingMode()` | Add `cfenv`-based implementation (Patch 3) |
| 4 | SH4 JIT symbols compiled in | `unreachable` trap at runtime, undefined `SH4_TCB` | `TARGET_NO_REC` not in CFLAGS/CXXFLAGS (only in CPUFLAGS, which doesn't reach compiler) | Add `-DTARGET_NO_REC` to both CFLAGS and CXXFLAGS |
| 5 | Duplicate `file_path.o` symbols | `fill_pathname` signature mismatch linker warning → silent runtime crash | Both Flycast and RetroArch ship `file_path.o` with different `fill_pathname` signatures | Remove Flycast's copy with `emar d` |

### Link-Time Bugs

| # | Bug | Symptom | Root Cause | Fix |
|---|-----|---------|-----------|-----|
| 6 | Missing `EJS_Runtime` export | `"EJS_Runtime is not defined"` | Default Emscripten uses `var Module`, EmulatorJS expects `EJS_Runtime` | `-s MODULARIZE=1 -s EXPORT_NAME=EJS_Runtime` |
| 7 | Missing IDBFS | `"Cannot read properties of undefined (reading 'mount')"` | IDBFS not linked | `-lidbfs.js` |
| 8 | Missing `callMain` | `"callMain is not a function"` | Not in exported runtime methods | Add to `EXPORTED_RUNTIME_METHODS` |
| 9 | BigInt conversion error | `"Cannot convert a BigInt value to a number"` | Platform JS passes `BigInt()`, WASM expects i64 | `-s WASM_BIGINT` |
| 10 | Missing JS libraries | `"PlatformEmscriptenGetSystemInfo"` abort | Platform, audio, webcam JS libraries not linked | `--js-library` for all three `.js` files |
| 11 | Missing `ejs_*` exports | `"Cannot call unknown function ejs_set_variable"` | EmulatorJS C API functions not in EXPORTED_FUNCTIONS | Add all required functions to export list |
| 12 | libchdr FLAC collision | CHD decompression returns garbage (empty product number, game won't load) | RetroArch's libchdr missing `libchdr_flac.o`, shadows Flycast's complete version | Exclude RA's libchdr objects from link |
| 13 | `fill_pathname` signature mismatch | Silent `unreachable` trap during `retro_load_game()` | Flycast returns `void`, RetroArch returns `size_t` — **WASM traps on any signature mismatch** | Provide stubs with correct signature, link before RA objects |
| 14 | Stale stub file path | Fix didn't take effect | Stubs compiled to wrong directory, link script used relative path | Ensure stubs are in link script's working directory |
| 15 | C++ exceptions as `unreachable` | Silent crash during SH4 MMU fault handling | Emscripten defaults to no exception support — `throw` becomes `abort()` | `-s DISABLE_EXCEPTION_CATCHING=0 -fexceptions` |
| 16 | Missing OpenAL export | `"'AL' was not exported"` | `AL` not in EXPORTED_RUNTIME_METHODS | Add `"AL"` to the list |
| 17 | Missing WebGL2 flags | WebGL1 context created, GLES3 features unavailable | No explicit WebGL2 requirement in link flags | `-s FULL_ES3=1 -s MIN_WEBGL_VERSION=2 -s MAX_WEBGL_VERSION=2` |

### Runtime Bugs

| # | Bug | Symptom | Root Cause | Fix |
|---|-----|---------|-----------|-----|
| 18 | GLSL version mismatch | Black screen, shader errors: `"'130' is not supported"` | Flycast generates `#version 130` (desktop GL) instead of `#version 300 es` (GLES3) | Force GLES3 in gles.cpp (Patch 4) |
| 19 | `glGetString` returns garbage | Console shows `OpenGL version:   #endif` | Emscripten GL cache returns stale data through `get_proc_address` function pointers | Three-layer fix: JS library + runtime patch + source patch |
| 20 | GL_INVALID_ENUM aborts video init | Black screen: `"Cannot open video driver. Exiting..."` | WebGL2 FBO operations produce 0x500; RetroArch's `gl2_check_error()` treats it as fatal | Runtime `getError` wrapper suppresses 0x500 (Patch B) |
| 21 | texParameter console spam causes lag | Massive lag on texture-heavy games (Dave Mirra: slideshow) | Flycast calls `texParameteri` without bound texture → hundreds of synchronous `console.error()` per frame | Runtime `texParameteri/f` guard (Patch C) |
| 22 | `__syscall_mprotect` spam | ~210 console warnings per session | WASM has no `mprotect` syscall — Emscripten stubs it with a warning | `console.warn` filter |

### EmulatorJS Integration Bugs

| # | Bug | Symptom | Root Cause | Fix |
|---|-----|---------|-----------|-----|
| 23 | `core.json` schema mismatch | Crash on `core.options.supportsMouse` | Wrong JSON structure — missing `"options": {}` | Match EmulatorJS expected schema |
| 24 | `build.json` missing version field | Hang at 99% during core decompression | `undefined.endsWith("-beta")` — missing `"minimumEJSVersion"` | Add `"minimumEJSVersion": "4.0"` |
| 25 | Minified `startGame` can't be patched | Monkey-patching silently fails | `emulator.min.js` mangles method names | `EJS_DEBUG_XX = true` for unminified source |
| 26 | BIOS location mismatch | `"Unable to find bios in //dc/"` | Flycast expects `/dc/` subdirectory, EmulatorJS writes to root | Relocate BIOS files before core boot |
| 27 | Multi-BIOS delivery | Only one BIOS file loaded | `EJS_biosUrl` is a single string | Custom delivery for `dc_flash.bin` |
| 28 | CHD extension stripping | ROM file not found by core | `checkCompression()` misidentifies CHD as compressed archive | Preserve `.chd` extension in WASM FS |
| 29 | Percent-encoded filenames | Path mismatch in WASM FS | URL encoding (`%20`) not decoded for filesystem paths | Decode before Flycast sees filename |
| 30 | `os_DebugBreak` is silent | Crash with no stack trace, no error message | `__builtin_trap()` compiles to WASM `unreachable` — instant silent death | `emscripten_log()` with C stack trace (Patch 5) |
| 31 | Invalid core option values | Options silently ignored, unexpected behavior | `"off"` is not a valid value for some Flycast options | Use correct values: `"1"`, `"disabled"` |
| 32 | WebGL1 fallback requested | EmulatorJS requests `flycast-legacy-wasm.data` | Flycast not in `requiresWebGL2` array | Add `"flycast"` to the array |

---

## Key Lessons for WASM Core Porting

1. **WASM function signature mismatches are crashes, not warnings.** The linker warns (`wasm-ld: warning: function signature mismatch`), but at runtime it's an instant `unreachable` trap with no error message, no stack trace, no recovery. Treat ALL signature mismatch warnings as fatal errors.

2. **Link order matters more than you think.** Stubs must come before the objects they override. Archives (`.a`) are searched only for unresolved symbols — standalone `.o` files always win. Getting this wrong produces silent runtime crashes, not link errors.

3. **`--allow-undefined` hides bugs.** Unresolved symbols become `unreachable` WASM instructions instead of link errors. You get a clean build and a runtime crash. Consider using `ERROR_ON_UNDEFINED_SYMBOLS` during development.

4. **Emscripten's GL layer has caching issues.** `glGetString` results can be garbage when accessed through `get_proc_address` function pointers. You may need to override at multiple layers (JS library, runtime patch, source code) to catch all code paths.

5. **Synchronous console logging is a performance killer in WASM.** WebGL validation errors logged via `console.error()` are synchronous. Hundreds per frame will destroy performance. The lag looks like a CPU bottleneck but is actually I/O. Suppress or prevent the errors at the source.

6. **EmulatorJS's core metadata schema is undocumented.** Wrong JSON structure causes crashes during core loading with unhelpful error messages. Match the schema of existing working cores exactly.

7. **Rebuild the archive after every recompile.** `emmake make` regenerates the `.a` file with all objects, including ones you previously removed (like `file_path.o`). Script the removal step.

8. **The minifier is your enemy.** If you need to intercept EmulatorJS internals at runtime (startGame, BIOS paths, etc.), use debug mode. The minified build mangles everything.

9. **Desktop GL behaviors you take for granted don't exist in WebGL2.** `texParameteri` without a bound texture, `GL_INVALID_ENUM` during FBO init, `#version 130` shaders — all valid on desktop, all broken on WebGL2. The GL spec is forgiving; WebGL2 is not.

10. **Three distinct crash modes in WASM.** (a) `unreachable` instruction — signature mismatch or undefined symbol. (b) `abort()` — unhandled exception or explicit abort. (c) Silent hang — infinite loop or deadlock. Each requires different debugging strategy. WASM debugging is not like native debugging.

---

## Future Work

- **Performance optimization:** `-O3 -flto -msimd128`, pre-allocated memory, threaded rendering via pthreads + WebWorkers. See PERFORMANCE.md for the full roadmap.
- **Upstream submission:** Submit Flycast WASM core to EmulatorJS project. File bugs for CHD extension stripping, multi-BIOS support, libchdr FLAC gap.
- **Production build:** Remove TRACE statements from libretro.cpp/nullDC.cpp, set `ASSERTIONS=0`, strip debug info.
- **Broader testing:** Game compatibility testing across CHD, CDI, and GDI formats. Build a compatibility database.
- **Saturn:** Similar approach may work for Mednafen Beetle Saturn — another missing EmulatorJS core with a broken emscripten target.
- **Upstream Flycast port:** Port these patches to `flyinghead/flycast` (CMake) for access to years of GPU fixes and game compatibility improvements.

---

## Acknowledgments

Built using [Claude Code](https://claude.ai/claude-code) for iterative compilation troubleshooting, runtime debugging, and WebGL compatibility engineering. All patches and findings are original work produced during this build effort.
