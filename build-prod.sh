#!/bin/bash
# Flycast WASM: production core build
#
# Verified against a clean checkout on: 2026-08-01 (verified)
#
# Prerequisites:
#   - Linux or WSL2 with Emscripten SDK 3.1.74+ (emsdk activated)
#   - Upstream flycast cloned at the pinned commit with patches applied:
#       git clone https://github.com/flyinghead/flycast.git source
#       cd source && git checkout 2c48c01
#       git submodule update --init --recursive
#       git apply ../patches/wasm-jit-phase1-modified.patch
#       cp ../patches/rec_wasm.cpp ../patches/wasm_emit.h \
#          ../patches/wasm_module_builder.h ../patches/fly_instrument.h \
#          core/rec-wasm/
#   - An EmulatorJS RetroArch tree built for emscripten (EJS_RA below).
#     The core links against RetroArch's emscripten objects and JS
#     libraries from the EmulatorJS build system.
#   - flycast_stubs.o and gl_override.js from this repository's build/
#     directory.
#
# All paths are overridable via environment variables.

set -e

SOURCE_DIR="${SOURCE_DIR:-$(pwd)/source}"
BUILD_DIR="${BUILD_DIR:-$SOURCE_DIR/build-wasm-prod}"
EJS_RA="${EJS_RA:-$(pwd)/EJS-RetroArch}"
STUBS="${STUBS:-$(pwd)/build/flycast_stubs.o}"
GL_OVERRIDE="${GL_OVERRIDE:-$(pwd)/build/gl_override.js}"
OUT_DIR="${OUT_DIR:-$(pwd)/out}"

echo "=== STEP 0: Ensure git submodules ==="
# The upstream clone must have its submodules (core/deps/*) checked out;
# a plain clone + checkout does not do this automatically.
git -C "$SOURCE_DIR" submodule update --init --recursive

echo "=== STEP 1: Configure + build (CMake) ==="
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
emcmake cmake "$SOURCE_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLIBRETRO=ON \
  -DUSE_GLES=ON \
  -DCMAKE_C_FLAGS="-DJIT_PROD_BUILD=1 -DFLY_RELEASE_BUILD=1" \
  -DCMAKE_CXX_FLAGS="-DJIT_PROD_BUILD=1 -DFLY_RELEASE_BUILD=1"
emmake make -j"$(nproc)"

echo "=== STEP 2: Strip conflicting objects ==="
cp libflycast_libretro_emscripten.a libflycast_libretro_emscripten_stripped.a
# file_path.c.o conflicts with RetroArch's copy.
emar d libflycast_libretro_emscripten_stripped.a \
    CMakeFiles/flycast_libretro.dir/core/deps/libretro-common/file/file_path.c.o \
    2>/dev/null || true
# glsym_es3.c.o is NOT stripped: Flycast's version has 716 GL symbols,
# RetroArch's has 407, and Flycast's glsm.c needs all 716. RetroArch's
# copy is excluded on the link side instead.

echo "=== STEP 3: Link against EmulatorJS RetroArch objects ==="
# Exclusions: libchdr/Lzma provided by Flycast's libchdr; glsym_es3 (above);
# flycast_stubs linked separately.
RA_OBJS=$(find "$EJS_RA/obj-emscripten" -name "*.o" -type f | grep -vE \
  "libchdr_chd|libchdr_cdrom|libchdr_lzma|libchdr_bitstream|libchdr_huffman|libchdr_zlib|libchdr_flac|chd_stream|LzmaEnc|LzmaDec|Lzma2Dec|Lzma86Dec|flycast_stubs|glsym_es3" \
  | sort)

emcc -O3 -flto \
  -s WASM=1 \
  -s WASM_BIGINT \
  -s MODULARIZE=1 \
  -s EXPORT_NAME=EJS_Runtime \
  -s EXPORTED_FUNCTIONS='["_main","_malloc","_free","_system_restart","_save_state_info","_load_state","_cmd_take_screenshot","_simulate_input","_toggleMainLoop","_get_core_options","_ejs_set_variable","_set_cheat","_reset_cheat","_shader_enable","_get_disk_count","_get_current_disk","_set_current_disk","_save_file_path","_cmd_savefiles","_supports_states","_refresh_save_files","_toggle_fastforward","_set_ff_ratio","_toggle_rewind","_set_rewind_granularity","_toggle_slow_motion","_set_sm_ratio","_get_current_frame_count","_set_vsync","_set_video_rotation","_get_video_dimensions","_ejs_set_keyboard_enabled","_wasm_mem_read8","_wasm_mem_read16","_wasm_mem_read32","_wasm_mem_write8","_wasm_mem_write16","_wasm_mem_write32","_wasm_exec_ifb","_wasm_exec_shil_fb","_wasm_sq_pref","_wasm_div32u","_wasm_div32s","_wasm_div1"]' \
  -s EXPORTED_RUNTIME_METHODS='["callMain","ccall","cwrap","UTF8ToString","stringToUTF8","lengthBytesUTF8","setValue","getValue","writeArrayToMemory","addRunDependency","removeRunDependency","FS","abort","AL","wasmExports"]' \
  -s INITIAL_MEMORY=268435456 \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s ALLOW_TABLE_GROWTH \
  -s ASYNCIFY=1 \
  -s ASYNCIFY_STACK_SIZE=65536 \
  -s 'ASYNCIFY_REMOVE=["Sh4Interpreter::*","i0*","i1*","addrspace::*","mmu_*","aica::*","Pvr*","pvr*","*ReadMem*","*WriteMem*","sh4_sched_tick*","*TA_*Param*"]' \
  -s EXIT_RUNTIME=0 \
  -s FORCE_FILESYSTEM=1 \
  -s WARN_ON_UNDEFINED_SYMBOLS=0 \
  -s ASSERTIONS=0 \
  -s DISABLE_EXCEPTION_CATCHING=0 \
  -fexceptions \
  -Wl,--wrap=glGetString -Wl,--allow-undefined \
  -s FULL_ES3=1 \
  -s MIN_WEBGL_VERSION=2 \
  -s MAX_WEBGL_VERSION=2 \
  -lopenal \
  -lidbfs.js \
  --js-library "$EJS_RA/emscripten/library_platform_emscripten.js" \
  --js-library "$EJS_RA/emscripten/library_rwebaudio.js" \
  --js-library "$EJS_RA/emscripten/library_rwebcam.js" \
  "$STUBS" \
  $RA_OBJS \
  "$BUILD_DIR/libflycast_libretro_emscripten_stripped.a" \
  "$BUILD_DIR/libflycast-resources.a" \
  "$BUILD_DIR/core/deps/tinygettext/libtinygettext.a" \
  "$BUILD_DIR/core/deps/nowide/libnowide.a" \
  "$BUILD_DIR/core/deps/libchdr/libchdr-static.a" \
  "$BUILD_DIR/core/deps/libchdr/deps/lzma-24.05/liblzma.a" \
  "$BUILD_DIR/core/deps/libchdr/deps/zstd-1.5.6/build/cmake/lib/libzstd.a" \
  "$BUILD_DIR/core/deps/libchdr/deps/zlib-1.3.1/libz.a" \
  "$BUILD_DIR/core/deps/xxHash/cmake_unofficial/libxxhash.a" \
  -o flycast_libretro.js \
  --js-library "$GL_OVERRIDE" \
  --pre-js "$EJS_RA/emscripten/pre.js"

echo "=== STEP 4: Package ==="
mkdir -p "$OUT_DIR"
cp flycast_libretro.js flycast_libretro.wasm "$OUT_DIR/"
echo "Core built: $OUT_DIR/flycast_libretro.js + .wasm"
echo "Load these in an EmulatorJS-style frontend. See README: Using the core."
