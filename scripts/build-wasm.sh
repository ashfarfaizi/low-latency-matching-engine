#!/usr/bin/env bash
# Builds the browser demo: compiles include/lob/order_book.hpp (via
# wasm/lob_wasm.cpp) to WebAssembly with Emscripten and copies the static page
# into dist/. Used by Vercel's build step; works locally if em++ is on PATH.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/dist"
EMSDK_DIR="${EMSDK_DIR:-$ROOT/.emsdk}"
EMSDK_VERSION="${EMSDK_VERSION:-4.0.10}"

if ! command -v em++ >/dev/null 2>&1; then
  echo "== installing Emscripten $EMSDK_VERSION"
  [ -d "$EMSDK_DIR" ] || git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
  pushd "$EMSDK_DIR" >/dev/null
  if ! (./emsdk install "$EMSDK_VERSION" && ./emsdk activate "$EMSDK_VERSION"); then
    echo "== $EMSDK_VERSION unavailable, using latest"
    ./emsdk install latest && ./emsdk activate latest
  fi
  popd >/dev/null
  # shellcheck disable=SC1091
  source "$EMSDK_DIR/emsdk_env.sh"
fi
em++ --version | head -1

rm -rf "$OUT"
mkdir -p "$OUT"
cp -r "$ROOT/web/." "$OUT/"
em++ -std=c++20 -O3 -I"$ROOT/include" "$ROOT/wasm/lob_wasm.cpp" -o "$OUT/lob.js" \
  -sMODULARIZE=1 -sEXPORT_NAME=createLob -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864 -sFILESYSTEM=0 \
  -sEXPORTED_RUNTIME_METHODS=cwrap,getValue
ls -la "$OUT"
echo "== built dist/ (lob.wasm $(wc -c < "$OUT/lob.wasm") bytes)"
