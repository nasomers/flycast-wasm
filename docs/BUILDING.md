# Building flycast-wasm from source — exact recipe

This document records the exact, verified build recipe for producing a
fully functional core with the WASM JIT enabled, from a fresh clone.
It complements `build-prod.sh` and `build-ejs-ra.sh` by explaining *why*
each flag is required, based on the failures encountered when deviating.

## Overview

```
flycast (pinned 2c48c01 + JIT patches)
        │  CMake -DLIBRETRO=ON -DUSE_GLES=ON -DJIT_PROD_BUILD=1 -DFLY_RELEASE_BUILD=1
        │  (bitcode LLVM: EMCC_CFLAGS="-flto")
        ▼
libflycast_libretro_emscripten.a  ──┐
                                    ├─► emcc link (-flto) ──► flycast_libretro.js/.wasm
EmulatorJS/RetroArch @ v1.22.2      │
  Makefile.emulatorjs               │
  HAVE_OPENGLES3=1                  │
  (no HAVE_STATIC_DUMMY)            │
  EMCC_CFLAGS="-flto"               │
  obj-emscripten/*.o (bitcode) ─────┘
```

## 1. Upstream flycast

Pin to commit `2c48c01` (`v2.6-142-g2c48c0188`), init submodules, then apply
`patches/wasm-jit-phase1-modified.patch` and copy the four JIT sources into
`core/rec-wasm/`.

**CRLF pitfall:** upstream `shell/libretro/audiostream.cpp` is committed with
CRLF line endings, but the patch context is LF. `git apply` fails with
"patch does not apply" until you run:

```bash
sed -i 's/\r$//' shell/libretro/audiostream.cpp
```

This does not change the compiled output (line endings only), but it is
required for the patch to apply.

## 2. EJS_RA (EmulatorJS RetroArch)

`build-prod.sh` links against `$EJS_RA/obj-emscripten/*.o` plus the JS
libraries under `$EJS_RA/emscripten/`. The tree is the EmulatorJS fork of
RetroArch, tag `v1.22.2`.

The three flags that make or break the build:

### 2a. Use `Makefile.emulatorjs`, not `Makefile.emscripten`

`Makefile.emulatorjs` sets `EMULATORJS=1` on its DEFINES line. That compiles:

- `input/drivers/emulatorjs_input.o` — provides `ejs_set_keyboard_enabled`
  (exported by the core, required by the EmulatorJS frontend)
- `frontend/drivers/platform_emulatorjs.o` — EmulatorJS frontend glue
- `audio/drivers/rwebaudio.o` — web audio driver

With `Makefile.emscripten` these objects are absent and the link fails with
`undefined exported symbol: _ejs_set_keyboard_enabled`.

### 2b. `HAVE_OPENGLES3=1`

`Makefile.common` compiles either `glsym/glsym_es3.o` (when
`HAVE_OPENGLES3=1`) or `glsym/glsym_es2.o` (otherwise). `build-prod.sh`
excludes `glsym_es3` from the link because flycast ships its own 716-symbol
`glsym_es3.c.o`. If the EJS_RA is built with `HAVE_OPENGLES3=0`, you get
`glsym_es2.o` whose `rglgen_symbol_map` collides with flycast's `glsym_es3`
→ `duplicate symbol: rglgen_symbol_map`.

### 2c. Do NOT set `HAVE_STATIC_DUMMY` (keep the default 0)

`cores/dynamic_dummy.c` has:

```c
#if defined(HAVE_STATIC_DUMMY)
void retro_get_system_info(...) { libretro_dummy_retro_get_system_info(info); }
void retro_init(void)           { libretro_dummy_retro_init(); }
// ... 16 retro_* wrappers
#endif
void libretro_dummy_retro_get_system_info(...) { ... }
// ... libretro_dummy_retro_* real implementations
```

With `HAVE_STATIC_DUMMY=1`, `dynamic_dummy.o` defines the `retro_*` entry
points, which collide with flycast's `shell/libretro/libretro.cpp`:
`duplicate symbol: retro_load_game_special`, `retro_get_memory_data`, etc.
Removing `dynamic_dummy.o` to fix that leaves `libretro_dummy_*` unresolved,
and the emscripten linker emits `abort("missing function:
libretro_dummy_retro_get_system_info")` stubs — games then die at runtime
with `Aborted(missing function: libretro_dummy_retro_get_system_info)`.

With the default `HAVE_STATIC_DUMMY=0` the `retro_*` wrappers are not
compiled, `dynamic_dummy.o` stays in the link without colliding, and no
stubs are generated.

### 2d. The LD failure is expected

`Makefile.emulatorjs` with `HAVE_STATIC_DUMMY=0` tries to link
`libretro_emscripten.a` (a core archive that only exists when a core is
built inside the tree). The final link fails with:

```
emcc: error: libretro_emscripten.a: No such file or directory
```

This is expected and harmless: `build-ejs-ra.sh` only needs the
`obj-emscripten/*.o` files that were already produced.

## 3. The `-flto` link: the EJS_RA must be LLVM bitcode

`build-prod.sh` links with `-flto`. For that to work, **every input object
must be LLVM bitcode** (magic bytes `BC`), not a final wasm module (`\0asm`).
This is exactly how the upstream release was built: its `build/flycast_stubs.o`
is bitcode, and its core's wasm exports are LTO-renamed (`wasmExports["Oj"]`).

### Why a naive build fails

If the EJS_RA is compiled with plain `-O3`, its `obj-emscripten/*.o` are
**final wasm modules**. The EJS frontend symbols that live inside them
(`retroarch.o`, `runloop.o`: `cmd_take_screenshot`,
`get_current_frame_count`, `toggleMainLoop`, `system_restart`, `load_state`,
`save_state_info`, disk helpers, fast-forward/rewind controls, ...) are not
visible as linkable symbols, and the `-flto` link fails with:

```
emcc: error: undefined exported symbol: "_cmd_take_screenshot" [-Wundefined] [-Werror]
```

### The correct recipe (matches the author's build)

Compile **both** the EJS_RA and flycast with `-flto` (LLVM bitcode objects),
then run `build-prod.sh` as-is:

```bash
export EMCC_CFLAGS="-flto"     # makes every emcc/em++ compile emit bitcode
bash build-ejs-ra.sh           # EJS_RA objects -> bitcode (LD failure expected)
bash build-prod.sh             # flycast + link with -flto, unmodified
unset EMCC_CFLAGS
```

`EMCC_CFLAGS` is honored by emscripten's compiler driver and is injected into
every compile without touching any Makefile. With all objects as bitcode, the
final `-flto` link resolves every EJS symbol and does whole-program
optimization across flycast and RetroArch.

**Verified results** (SDK 3.1.74, Aug 2026):

| Build | wasm size | vs release |
|---|---|---|
| upstream release (Feb 2026) | 10,398,725 B | — |
| no LTO (wasm objects) | 10,430,712 B | +0.31% |
| EJS_RA bitcode only | 10,957,160 B | +5.37% |
| **all bitcode (recommended)** | **10,746,527 B** | **+3.34%** |

The residual size difference comes from toolchain/emsdk versions (the
author's release was built Feb 2026), not from the recipe.

Do not "fix" the naive failure with `-sERROR_ON_UNDEFINED_SYMBOLS=0` or
`-Wl,--unresolved-symbols=ignore-all` — those do not help (the error is in
the EXPORTED_FUNCTIONS validation) and would silently break the frontend
API. The correct fix is bitcode objects, as above.

## 4. Exclusions in the link

`build-prod.sh` excludes from `RA_OBJS`:

- `libchdr*`, `Lzma*`, `chd_stream*` — provided by flycast's own libchdr
- `flycast_stubs` — linked separately
- `glsym_es3` — flycast's 716-symbol GL syms win

No further exclusions are needed. In particular `dynamic_dummy.o` and
`glsym_es2.o` must **not** be excluded; with the flags above they link
cleanly.

## 5. Packaging

`build-prod.sh` writes `out/flycast_libretro.js` + `out/flycast_libretro.wasm`.
To serve it through an EmulatorJS-style frontend, pack both into a 7z
archive named `flycast-wasm.data` containing `core.js`, `core.wasm`,
`core.json`, `build.json` and `license.txt`, and place it in
`data/cores/` (see the `release/` folder in this repository for a complete
serving setup).

## 6. Building the release zip (end-user package)

`build-release.sh` automates the full pipeline: build the core with
`build-prod.sh` (LTO recipe, see §3), pack the 7z data archive, refresh the
`release/` tree (both `flycast-wasm.data` and `flycast-legacy-wasm.data`,
the launcher serves either name for cache-busting compatibility) and
finally produce `flycast-wasm-release.zip`.

```bash
# From the repo root:
export EMCC_CFLAGS="-flto"       # bitcode objects (LTO recipe)
bash build-release.sh            # core + release/ + zip, all steps
unset EMCC_CFLAGS
```

`build-release.sh` accepts the same environment overrides as
`build-prod.sh` (`SOURCE_DIR`, `BUILD_DIR`, `OUT_DIR`, ...). The zip
contains **only what a non-developer needs to run the emulator**:

- `server.js` + `package.json` (run with `npm start` or `node server.js`)
- `frontend/` — EmulatorJS data (loader, emulator.js, cores, compression,
  localization), the `test-flycast.html` launcher, and empty `bios/` /
  `roms/` folders with `LEEME.txt` instructions
- `README.md`, `CREDITS.md`, `LICENSE` (bilingual: English first, Spanish
  at the end)

It deliberately **excludes** everything build-related: `build-prod.sh`,
`build-ejs-ra.sh`, `patches/`, `docs/`, `source/`, `build/` — end users run
the emulator, they do not build it.

## 7. Cross-platform notes (Linux / WSL)

Building is supported on **Linux** and **Windows with WSL** (the same native
Linux toolchain). The scripts are written in portable bash; the `.gitattributes`
file pins `*.sh` and `patches/*` to LF so a Windows clone does not corrupt them
with CRLF (which would otherwise break bash with `bad interpreter` and
`git apply`).

| Tool | Linux | WSL |
|---|---|---|
| emsdk (emcmake/emmake) | ✅ | ✅ |
| git | ✅ | ✅ |
| make | ✅ | ✅ |
| cmake | ✅ | ✅ |
| nproc | ✅ | ✅ |
| 7z | ⚠️ `p7zip-full` | ⚠️ `p7zip-full` |
| md5sum | ✅ | ✅ |

**Verified on both platforms** (Aug 2026): the resulting cores are
functionally identical (0 `libretro_dummy` stubs, EJS exports present, WASM
JIT active). Linux and WSL build the EJS_RA objects and run `build-prod.sh`
as-is.

Notes:

- **WSL** — the most straightforward Windows route: the whole toolchain is
  native Linux; everything works out of the box (verified).
- **Linux** — install `build-essential git p7zip-full`, then the emsdk.
- `JOBS` overrides the parallel make jobs everywhere (default: `nproc`).
- Native Windows (Git Bash) is **not** supported for building: MSYS2's path
  mangling (TEMP/TMP, python3 vs Windows python, 32KB command-line limit on
  the emcc link) breaks the toolchain in ways the scripts do not paper over.
  Use WSL instead.

The exact per-platform tool install commands for each distro (apt/pacman)
are left to the reader; only the availability of the tools above is required
by the scripts.
