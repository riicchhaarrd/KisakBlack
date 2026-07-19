#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

EMSCRIPTEN_ROOT="$(dirname "$(readlink -f "$(command -v em++)")")"
SYS_EM_CACHE="$EMSCRIPTEN_ROOT/cache"
if [ -d "$SYS_EM_CACHE" ] && [ ! -w "$SYS_EM_CACHE" ]; then
  : "${EM_CACHE_DIR:=$HOME/.emscripten_cache}"
  : "${EM_CONFIG:=$HOME/.emscripten_web_config}"
  if [ ! -d "$EM_CACHE_DIR/sysroot" ]; then
    cp -a "$SYS_EM_CACHE" "$EM_CACHE_DIR"
  fi
  if [ ! -f "$EM_CONFIG" ]; then
    sed 's/^FROZEN_CACHE = True.*/FROZEN_CACHE = False/' "$EMSCRIPTEN_ROOT/.emscripten" > "$EM_CONFIG"
    grep -q '^CACHE' "$EM_CONFIG" || echo "CACHE = '$EM_CACHE_DIR'" >> "$EM_CONFIG"
  fi
  export EM_CONFIG
fi

: "${KISAK_BUILD_DIR:=build_web}"
: "${KISAK_WEB_THREADS:=OFF}"
: "${KISAK_WEB_ASSERTIONS:=ON}"
: "${KISAK_WEB_DEPROXY:=OFF}"
: "${KISAK_WEB_SAFE_HEAP:=OFF}"
: "${KISAK_WEB_GL_DEBUG:=OFF}"

emcmake cmake -S . -B "$KISAK_BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DKISAK_WEB_THREADS="$KISAK_WEB_THREADS" \
  -DKISAK_WEB_ASSERTIONS="$KISAK_WEB_ASSERTIONS" \
  -DKISAK_WEB_DEPROXY="$KISAK_WEB_DEPROXY" \
  -DKISAK_WEB_SAFE_HEAP="$KISAK_WEB_SAFE_HEAP" \
  -DKISAK_WEB_GL_DEBUG="$KISAK_WEB_GL_DEBUG"
cmake --build "$KISAK_BUILD_DIR" --parallel
