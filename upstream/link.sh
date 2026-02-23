#!/bin/bash
# Flycast WASM Upstream — Link Script
# Links upstream flyinghead/flycast .a with EmulatorJS RetroArch objects
set -e

cd /home/ghost/flycast-wasm
source /home/ghost/.emsdk/emsdk_env.sh 2>/dev/null

UPSTREAM_ARCHIVE="/mnt/c/DEV Projects/flycast-wasm/upstream/source/build-wasm/libflycast_libretro_emscripten_stripped.a"
RESOURCES_ARCHIVE="/mnt/c/DEV Projects/flycast-wasm/upstream/source/build-wasm/libflycast-resources.a"

# Verify archives exist
if [ ! -f "$UPSTREAM_ARCHIVE" ]; then
    echo "ERROR: Upstream archive not found: $UPSTREAM_ARCHIVE"
    exit 1
fi
if [ ! -f "$RESOURCES_ARCHIVE" ]; then
    echo "ERROR: Resources archive not found: $RESOURCES_ARCHIVE"
    exit 1
fi

echo "=== Flycast WASM Upstream Link ==="
echo "Core archive: $(ls -lh "$UPSTREAM_ARCHIVE" | awk '{print $5}')"
echo "Resources archive: $(ls -lh "$RESOURCES_ARCHIVE" | awk '{print $5}')"

# RetroArch objects — exclude conflicts:
# - libchdr_* / LzmaEnc/Dec / chd_stream: provided by Flycast's libchdr-static.a
# - flycast_stubs: our own stubs linked separately
# - glsym_es3: Flycast's version has 716 GL symbols vs RetroArch's 407.
#   Flycast's glsm.c needs all 716, so we use Flycast's glsym_es3.c.o from the archive.
RA_OBJS=$(find EJS-RetroArch/obj-emscripten -name "*.o" -type f | grep -vE "libchdr_chd|libchdr_cdrom|libchdr_lzma|libchdr_bitstream|libchdr_huffman|libchdr_zlib|libchdr_flac|chd_stream|LzmaEnc|LzmaDec|Lzma2Dec|Lzma86Dec|flycast_stubs|glsym_es3" | sort)

echo "RetroArch objects: $(echo "$RA_OBJS" | wc -l)"
echo ""
echo "Linking..."

# Link: stubs FIRST, then RetroArch objects, then Flycast archives
# (link order matters — stubs provide fill_short_pathname_representation)
emcc -O3 -flto \
  -s WASM=1 \
  -s WASM_BIGINT \
  -s MODULARIZE=1 \
  -s EXPORT_NAME=EJS_Runtime \
  -s EXPORTED_FUNCTIONS='["_main","_malloc","_free","_system_restart","_save_state_info","_load_state","_cmd_take_screenshot","_simulate_input","_toggleMainLoop","_get_core_options","_ejs_set_variable","_set_cheat","_reset_cheat","_shader_enable","_get_disk_count","_get_current_disk","_set_current_disk","_save_file_path","_cmd_savefiles","_supports_states","_refresh_save_files","_toggle_fastforward","_set_ff_ratio","_toggle_rewind","_set_rewind_granularity","_toggle_slow_motion","_set_sm_ratio","_get_current_frame_count","_set_vsync","_set_video_rotation","_get_video_dimensions","_ejs_set_keyboard_enabled","_wasm_mem_read8","_wasm_mem_read16","_wasm_mem_read32","_wasm_mem_write8","_wasm_mem_write16","_wasm_mem_write32","_wasm_exec_ifb","_wasm_exec_shil_fb"]' \
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
  --js-library EJS-RetroArch/emscripten/library_platform_emscripten.js \
  --js-library EJS-RetroArch/emscripten/library_rwebaudio.js \
  --js-library EJS-RetroArch/emscripten/library_rwebcam.js \
  flycast_stubs.o \
  $RA_OBJS \
  "$UPSTREAM_ARCHIVE" \
  "$RESOURCES_ARCHIVE" \
  "/mnt/c/DEV Projects/flycast-wasm/upstream/source/build-wasm/core/deps/tinygettext/libtinygettext.a" \
  "/mnt/c/DEV Projects/flycast-wasm/upstream/source/build-wasm/core/deps/nowide/libnowide.a" \
  "/mnt/c/DEV Projects/flycast-wasm/upstream/source/build-wasm/core/deps/libchdr/libchdr-static.a" \
  "/mnt/c/DEV Projects/flycast-wasm/upstream/source/build-wasm/core/deps/libchdr/deps/lzma-24.05/liblzma.a" \
  "/mnt/c/DEV Projects/flycast-wasm/upstream/source/build-wasm/core/deps/libchdr/deps/zstd-1.5.6/build/cmake/lib/libzstd.a" \
  "/mnt/c/DEV Projects/flycast-wasm/upstream/source/build-wasm/core/deps/libchdr/deps/zlib-1.3.1/libz.a" \
  "/mnt/c/DEV Projects/flycast-wasm/upstream/source/build-wasm/core/deps/xxHash/cmake_unofficial/libxxhash.a" \
  -o flycast_libretro_upstream.js \
  --js-library /home/ghost/flycast-wasm/gl_override.js \
  --pre-js EJS-RetroArch/emscripten/pre.js

echo ""
echo "=== Link Complete ==="
ls -la flycast_libretro_upstream.js flycast_libretro_upstream.wasm
