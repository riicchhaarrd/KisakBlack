#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

export KISAK_BUILD_DIR=build_web_mt
export KISAK_WEB_THREADS=ON
export KISAK_WEB_DEPROXY="${KB_DEPROXY:-OFF}"
export KISAK_WEB_SAFE_HEAP="${KB_SAFEHEAP:-OFF}"
export KISAK_WEB_GL_DEBUG="${KB_GLDEBUG:-OFF}"
if [ "${KB_PERF:-1}" = "0" ]; then
  export KISAK_WEB_ASSERTIONS=ON
else
  export KISAK_WEB_ASSERTIONS=OFF
fi
exec ./build_web.sh
