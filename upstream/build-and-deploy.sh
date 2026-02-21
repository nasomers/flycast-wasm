#!/bin/bash
# Flycast WASM Upstream — Full Build, Link, Package, Deploy
# Run from WSL: bash "/mnt/c/DEV Projects/flycast-wasm/upstream/build-and-deploy.sh"
#
# Steps:
#   1. Incremental CMake build (only recompiles changed files)
#   2. Copy archive + strip conflicting objects
#   3. Link with EmulatorJS RetroArch
#   4. Package as EmulatorJS .data (7z)
#   5. Deploy to demo/data/cores/
#
# Exit codes: 0 = success, 1 = build failed, 2 = link failed, 3 = package failed

set -e

PROJECT_DIR="/mnt/c/DEV Projects/flycast-wasm"
BUILD_DIR="$PROJECT_DIR/upstream/source/build-wasm"
WSL_DIR="/home/ghost/flycast-wasm"
DEMO_CORES="$PROJECT_DIR/demo/data/cores"

# Init emsdk
source /home/ghost/.emsdk/emsdk_env.sh 2>/dev/null

echo "=== STEP 1: Incremental CMake Build ==="
cd "$BUILD_DIR"
emmake make -j$(nproc) 2>&1 | tail -5
if [ ${PIPESTATUS[0]} -ne 0 ]; then
    echo "FAILED: CMake build"
    exit 1
fi

echo ""
echo "=== STEP 2: Strip Conflicting Objects ==="
cp libflycast_libretro_emscripten.a libflycast_libretro_emscripten_stripped.a
# Strip file_path.c.o (conflicts with RetroArch's copy)
emar d libflycast_libretro_emscripten_stripped.a \
    CMakeFiles/flycast_libretro.dir/core/deps/libretro-common/file/file_path.c.o 2>/dev/null || true
# NOTE: glsym_es3.c.o is NOT stripped — Flycast's version has 716 GL symbols,
# RetroArch's old version only has 407. Flycast's glsm.c needs all 716.
# Instead, RetroArch's glsym_es3.o is excluded from RA_OBJS in link.sh.
echo "Archive objects: $(emar t libflycast_libretro_emscripten_stripped.a | wc -l)"

echo ""
echo "=== STEP 3: Link ==="
cd "$WSL_DIR"
bash "$PROJECT_DIR/upstream/link.sh" 2>&1
if [ $? -ne 0 ]; then
    echo "FAILED: Link"
    exit 2
fi

echo ""
echo "=== STEP 4: Package ==="
mkdir -p pkg_upstream
cp flycast_libretro_upstream.js pkg_upstream/flycast_libretro.js
cp flycast_libretro_upstream.wasm pkg_upstream/flycast_libretro.wasm
echo '{"name":"flycast","extensions":["cdi","gdi","chd","cue","iso","elf","bin","lst","zip","7z","dat"],"options":{}}' > pkg_upstream/core.json
echo '{"version":"1.0.0","minimumEJSVersion":"4.0"}' > pkg_upstream/build.json
cd pkg_upstream
7z a -t7z -mx=9 flycast-wasm.data flycast_libretro.js flycast_libretro.wasm core.json build.json 2>&1 | tail -3
if [ $? -ne 0 ]; then
    echo "FAILED: Package"
    exit 3
fi

echo ""
echo "=== STEP 5: Deploy ==="
cp flycast-wasm.data "$DEMO_CORES/flycast-wasm.data"
echo "Deployed: $(ls -lh "$DEMO_CORES/flycast-wasm.data" | awk '{print $5}')"

echo ""
echo "=== BUILD + DEPLOY COMPLETE ==="
echo "Run test: node upstream/flycast-wasm-test.js"
