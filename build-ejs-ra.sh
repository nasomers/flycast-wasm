#!/bin/bash
# Flycast WASM: build the EmulatorJS RetroArch tree (EJS_RA)
#
# The flycast core links against RetroArch's emscripten objects and JS
# libraries from the EmulatorJS build system. build-prod.sh consumes:
#   $EJS_RA/obj-emscripten/*.o            (RetroArch objects)
#   $EJS_RA/emscripten/library_platform_emscripten.js
#   $EJS_RA/emscripten/library_rwebaudio.js
#   $EJS_RA/emscripten/library_rwebcam.js
#   $EJS_RA/emscripten/pre.js
#
# This script produces exactly that tree with the flags that matter:
#
#   - Makefile.emulatorjs  (NOT Makefile.emscripten): defines EMULATORJS=1,
#     which compiles emulatorjs_input.o (ejs_set_keyboard_enabled) and the
#     EmulatorJS frontend objects the core needs.
#   - HAVE_OPENGLES3=1     : generates glsym_es3.o, which build-prod.sh
#     excludes from the link (flycast provides its own 716-symbol glsym_es3).
#     Without it you get glsym_es2.o whose rglgen_symbol_map collides with
#     flycast's.
#   - HAVE_STATIC_DUMMY not set (default 0): WITHOUT the static dummy core.
#     With HAVE_STATIC_DUMMY=1, cores/dynamic_dummy.c emits retro_* wrappers
#     that collide with flycast's shell/libretro/libretro.cpp, and excluding
#     dynamic_dummy.o leaves abort("missing function: libretro_dummy_*")
#     stubs (game dies with libretro_dummy_retro_get_system_info abort).
#
# LTO (optional, to match the upstream release exactly):
#   The upstream release was built with Link-Time Optimization: its objects
#   are LLVM bitcode (the repo's build/flycast_stubs.o is bitcode, magic BC),
#   and build-prod.sh links with -flto. To reproduce that, export
#   EMCC_CFLAGS="-flto" BEFORE running this script and build-prod.sh. This
#   makes every .o an LLVM bitcode module and lets the final -flto link do
#   whole-program optimization. Without it, the .o are final wasm objects and
#   the -flto link fails with "undefined exported symbol: _cmd_take_screenshot"
#   (see docs/BUILDING.md).
#
# The final LD step fails with "libretro_emscripten.a: No such file" - that is
# EXPECTED and harmless: the .o files are what build-prod.sh needs, not the
# retroarch.js binary.

set -e

EJS_RA="${EJS_RA:-$(pwd)/EJS-RetroArch}"
EJS_RA_URL="${EJS_RA_URL:-https://github.com/EmulatorJS/RetroArch.git}"
EJS_RA_TAG="${EJS_RA_TAG:-v1.22.2}"
# nproc does not exist in Git Bash (Windows); JOBS is overridable.
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

if [ ! -d "$EJS_RA" ]; then
    echo "=== Cloning EmulatorJS/RetroArch @ $EJS_RA_TAG ==="
    git clone --depth 1 --branch "$EJS_RA_TAG" "$EJS_RA_URL" "$EJS_RA"
fi

echo "=== Building EJS_RA objects (LD failure at the end is expected) ==="
cd "$EJS_RA"
emmake make -f Makefile.emulatorjs clean
rm -rf obj-emscripten
# The LD error at the end is expected; we only need obj-emscripten/*.o.
emmake make -f Makefile.emulatorjs -j"$JOBS" HAVE_OPENGLES3=1 \
    || echo "(LD step failed as expected - objects are ready)"

echo "=== Verifying key objects ==="
for o in obj-emscripten/retroarch.o \
         obj-emscripten/runloop.o \
         obj-emscripten/input/drivers/emulatorjs_input.o \
         obj-emscripten/libretro-common/glsym/glsym_es3.o; do
    if [ -f "$o" ]; then
        echo "  OK  $o"
    else
        echo "  MISSING  $o"
    fi
done
echo "EJS_RA ready at: $EJS_RA"
