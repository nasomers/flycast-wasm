#!/bin/bash
# Flycast WASM: build the core AND produce the end-user release zip.
#
# Full pipeline:
#   1. build-prod.sh                 -> $OUT_DIR/flycast_libretro.{js,wasm}
#   2. pack 7z                       -> flycast-wasm.data
#   3. refresh release/frontend/data/cores/  (both data names, see below)
#   4. zip release/                  -> flycast-wasm-release.zip (end users)
#
# Portable bash (Linux and WSL verified). The 7z binary is located
# automatically (7z/7za in PATH, or 7-Zip's default install dir); md5
# checksums fall back to python3 or certutil where md5sum is missing.
#
# Prerequisites:
#   - Emscripten SDK sourced (emsdk_env.sh)
#   - EJS_RA built (build-ejs-ra.sh) and SOURCE_DIR patched (step 1 of
#     docs/BUILDING.md)
#   - For the LTO recipe (recommended, matches the upstream release):
#         export EMCC_CFLAGS="-flto"    before running this script
#   - 7z (p7zip on Linux/WSL; 7-Zip on Windows) or SEVENZIP=/path/to/7z
#
# Environment overrides (same as build-prod.sh):
#   SOURCE_DIR, BUILD_DIR, EJS_RA, STUBS, GL_OVERRIDE, OUT_DIR, JOBS
#   RELEASE_DIR (default: <repo>/release)
#   ZIP_OUT    (default: <repo>/flycast-wasm-release.zip)
#   SEVENZIP   (default: auto-detected)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RELEASE_DIR="${RELEASE_DIR:-$REPO_ROOT/release}"
ZIP_OUT="${ZIP_OUT:-$REPO_ROOT/flycast-wasm-release.zip}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/out}"

# --- Locate a 7z binary (Linux / WSL / Windows Git Bash) ---
SEVENZIP="${SEVENZIP:-}"
if [ -z "$SEVENZIP" ]; then
    for c in 7z 7za; do
        if command -v "$c" >/dev/null 2>&1; then SEVENZIP="$c"; break; fi
    done
fi
if [ -z "$SEVENZIP" ]; then
    for p in "/c/Program Files/7-Zip/7z.exe" \
             "/mnt/c/Program Files/7-Zip/7z.exe"; do
        if [ -x "$p" ]; then SEVENZIP="$p"; break; fi
    done
fi
if [ -z "$SEVENZIP" ]; then
    echo "ERROR: no 7z binary found." >&2
    echo "  Linux/WSL: sudo apt install p7zip-full" >&2
    echo "  Windows:   install 7-Zip (https://www.7-zip.org) or set SEVENZIP=/path/to/7z" >&2
    exit 1
fi
echo "7z : $SEVENZIP"

# --- md5sum with cross-platform fallbacks ---
md5_of() { # file...  ->  hash  file
    if command -v md5sum >/dev/null 2>&1; then
        md5sum "$@"
    elif command -v python3 >/dev/null 2>&1; then
        python3 - "$@" <<'PY'
import hashlib, sys
for f in sys.argv[1:]:
    with open(f, 'rb') as fh:
        print(f"{hashlib.md5(fh.read()).hexdigest()}  {f}")
PY
    elif command -v certutil >/dev/null 2>&1; then
        # Windows certutil: "MD5 hash of file:" + hex (format differs)
        for f in "$@"; do
            h=$(certutil -hashfile "$f" MD5 2>/dev/null | grep -iE '^[0-9a-f]{32}' | head -1 | tr -d ' ')
            echo "$h  $f"
        done
    else
        echo "WARNING: md5sum not available; skipping checksums" >&2
        return 0
    fi
}

echo "=== STEP 1: Build the core (build-prod.sh) ==="
"$REPO_ROOT/build-prod.sh"

echo "=== STEP 2: Pack 7z data archive ==="
PKG_DIR="$(mktemp -d)"
trap 'rm -rf "$PKG_DIR"' EXIT
cp "$OUT_DIR/flycast_libretro.js" "$PKG_DIR/core.js"
cp "$OUT_DIR/flycast_libretro.wasm" "$PKG_DIR/core.wasm"
cat > "$PKG_DIR/core.json" <<'EOF'
{"name":"flycast","extensions":["cdi","gdi","chd","cue","iso","elf","bin","lst","zip","7z","dat"],"options":{},"save":"state","license":"GPLv2","repo":"https://github.com/flyinghead/flycast"}
EOF
cat > "$PKG_DIR/build.json" <<'EOF'
{"minimumEJSVersion":"4.2.3","version":"1.0.0"}
EOF
echo "GPLv2" > "$PKG_DIR/license.txt"
"$SEVENZIP" a -t7z -mx9 "$RELEASE_DIR/frontend/data/cores/flycast-wasm.data" \
    "$PKG_DIR/core.js" "$PKG_DIR/core.wasm" "$PKG_DIR/core.json" \
    "$PKG_DIR/build.json" "$PKG_DIR/license.txt" >/dev/null

echo "=== STEP 3: Refresh release tree ==="
mkdir -p "$RELEASE_DIR/frontend/data/cores" \
         "$RELEASE_DIR/frontend/bios" "$RELEASE_DIR/frontend/roms"
# Both names: the launcher serves "flycast-wasm.data" and some EmulatorJS
# versions request "flycast-legacy-wasm.data" - same core either way.
cp "$RELEASE_DIR/frontend/data/cores/flycast-wasm.data" \
   "$RELEASE_DIR/frontend/data/cores/flycast-legacy-wasm.data"

echo "=== STEP 4: Zip the release (end-user package) ==="
rm -f "$ZIP_OUT"
(cd "$RELEASE_DIR" && "$SEVENZIP" a -tzip -mx9 "$ZIP_OUT" . -xr!__MACOSX -xr!DS_Store >/dev/null)

echo "=== Done ==="
echo "Core : $OUT_DIR/flycast_libretro.js + .wasm"
echo "Data : $RELEASE_DIR/frontend/data/cores/flycast-wasm.data"
md5_of "$RELEASE_DIR/frontend/data/cores/flycast-wasm.data" "$ZIP_OUT"
echo "Zip  : $ZIP_OUT (unzip it anywhere, run 'npm start' or 'node server.js')"
