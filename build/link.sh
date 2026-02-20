#!/bin/bash
set -e
cd /home/ghost/flycast-wasm
source /home/ghost/.emsdk/emsdk_env.sh 2>/dev/null

RA_OBJS=$(find EJS-RetroArch/obj-emscripten -name "*.o" -type f | grep -vE "libchdr_chd|libchdr_cdrom|libchdr_lzma|libchdr_bitstream|libchdr_huffman|libchdr_zlib|libchdr_flac|chd_stream|LzmaEnc|LzmaDec|Lzma2Dec|Lzma86Dec|flycast_stubs" | sort)

emcc -O2 -g2 \
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
  -s ASSERTIONS=2 \
  -s DISABLE_EXCEPTION_CATCHING=0 \
  -fexceptions \
  -Wl,--wrap=glGetString -Wl,--allow-undefined \
  -s FULL_ES3=1   -s MIN_WEBGL_VERSION=2   -s MAX_WEBGL_VERSION=2   -lopenal   -lidbfs.js \
  --js-library EJS-RetroArch/emscripten/library_platform_emscripten.js \
  --js-library EJS-RetroArch/emscripten/library_rwebaudio.js \
  --js-library EJS-RetroArch/emscripten/library_rwebcam.js \
  flycast_stubs.o flycast_stubs_cpp.o \
  $RA_OBJS \
  flycast/flycast_libretro_emscripten.a \
  -o flycast_libretro.js \
  --js-library /home/ghost/flycast-wasm/gl_override.js  \
  --pre-js EJS-RetroArch/emscripten/pre.js

echo "Link complete"
ls -la flycast_libretro.js flycast_libretro.wasm
