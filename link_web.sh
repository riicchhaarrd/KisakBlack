#!/bin/bash
# link_web.sh — Milestone-2 LINK step: link the WASM objects produced by
# build_web.sh into blackops.wasm / .js / .html.
#
# Prereq: run ./build_web.sh first so build_web/obj/*.o exists (743 objects).
# This script does NOT recompile; it only links. It reuses the same writable
# Emscripten cache + un-frozen config that build_web.sh provisions.
#
# Output (all under the gitignored build_web/):
#   build_web/web/blackops.js     — JS loader/runtime
#   build_web/web/blackops.wasm   — the module
#   build_web/web/blackops.html   — minimal Emscripten shell (replaced by our
#                                   harness index.html for the real run)
#
# Expect undefined-symbol link errors on the first pass (Win32 emulation, audio /
# video / voice backends). Triage: stub or resolve each under __EMSCRIPTEN__. Run
# with VERBOSE=1 to keep going and dump the full undefined list:
#   VERBOSE=1 ./link_web.sh
set -u
cd "$(dirname "$0")"

# --- same toolchain bring-up as build_web.sh (writable cache, un-frozen config) -
SYS_EM_CACHE="$(dirname "$(command -v em++)")/cache"
[ -d "$SYS_EM_CACHE" ] || SYS_EM_CACHE="/usr/share/emscripten/cache"
SYS_EM_CONFIG="/usr/share/emscripten/.emscripten"
: "${EM_CACHE_DIR:=$HOME/.emscripten_cache}"
: "${EM_CONFIG:=$HOME/.emscripten_web_config}"
export EM_CONFIG
if [ ! -d "$EM_CACHE_DIR/sysroot" ]; then
  cp -a "$SYS_EM_CACHE" "$EM_CACHE_DIR"
fi
if [ ! -f "$EM_CONFIG" ]; then
  sed -e 's/^FROZEN_CACHE = True.*/FROZEN_CACHE = False/' "$SYS_EM_CONFIG" > "$EM_CONFIG"
  grep -q '^CACHE' "$EM_CONFIG" || echo "CACHE = '$EM_CACHE_DIR'" >> "$EM_CONFIG"
fi

OBJDIR="build_web/obj"
OUTDIR="build_web/web"
mkdir -p "$OUTDIR"
if ! ls "$OBJDIR"/*.o >/dev/null 2>&1; then
  echo "no objects in $OBJDIR — run ./build_web.sh first" >&2; exit 1
fi
NOBJ=$(ls "$OBJDIR"/*.o | wc -l)
echo "linking $NOBJ objects -> $OUTDIR/blackops.{js,wasm,html} ..."

# Link settings (milestone 2 baseline; see WEB_PORT_TRIAGE.md §6 / the mission):
#   * WebGL2 / GLES3 only, SDL2 + OpenAL from Emscripten ports.
#   * ALLOW_MEMORY_GROWTH + 512 MB initial (wasm32 ~2 GB cap; data streams on
#     demand — see M4). 8 MB stack/main, 8 MB worker default.
#   * EXPORTED_RUNTIME_METHODS for the JS harness (FS bridge in M4).
#   * ASYNCIFY is intentionally OFF here (single-threaded inline seam in M3); the
#     FS-Access sync bridge lands in M4 and may flip it on or move to pthreads.
LINKFLAGS="\
  -sUSE_SDL=2 \
  -sMAX_WEBGL_VERSION=2 -sMIN_WEBGL_VERSION=2 -sFULL_ES3=1 \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=512MB -sMAXIMUM_MEMORY=2GB \
  -sSTACK_SIZE=8MB \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,callMain,FS,stringToUTF8,UTF8ToString,lengthBytesUTF8,HEAPU8 \
  -lopenal \
  -sALLOW_TABLE_GROWTH=1 \
  -sASSERTIONS=1 \
  -sASYNCIFY=1 -sASYNCIFY_STACK_SIZE=262144 \
  -sEMULATE_FUNCTION_POINTER_CASTS=1 \
  -sINVOKE_RUN=0 \
  -O0 -g"
# ASYNCIFY: the File System Access reads are async (web_fs.js); the EM_ASYNC_JS
# bridge in web_fs.cpp suspends/resumes the wasm stack around them so the engine's
# synchronous file I/O blocks. INVOKE_RUN=0 lets the harness call main() only
# after a data folder is granted.
#
# ASYNCIFY + COOPERATIVE FIBERS (web_fibers.cpp): every engine "thread" runs as a
# fiber on one OS thread via emscripten_fiber_swap (itself an Asyncify async op).
# Each fiber carries its OWN 512 KB Asyncify stack (allocated in web_fibers.cpp), so
# an FS read INSIDE a worker fiber unwinds/rewinds against that fiber's stack. This
# global ASYNCIFY_STACK_SIZE governs the ROOT context's asyncify stack and any
# non-fiber sleeps; bumped 64 KB -> 256 KB for headroom on the deeper boot-path call
# trees (DB_TryLoadXFile -> fastfile inflate -> nested FS reads) that unwind here
# before/around fiber bring-up.

# Some Win32/CRT-emulation symbols (e.g. SEH, a few rarely-hit kernel calls) may be
# referenced but never defined for wasm. For the FIRST link pass we want to SEE
# them, not paper over them, so undefined symbols are errors by default. Once the
# real list is triaged into stubs, set ERR_UNDEF=0 to allow JS-resolved imports.
: "${ERR_UNDEF:=1}"
if [ "${ERR_UNDEF}" = "1" ]; then
  LINKFLAGS="$LINKFLAGS -sERROR_ON_UNDEFINED_SYMBOLS=1"
else
  # Don't hard-error; emit JS trampolines that throw if actually called.
  LINKFLAGS="$LINKFLAGS -sERROR_ON_UNDEFINED_SYMBOLS=0"
fi

set -o pipefail
# Emit blackops.js + blackops.wasm (NOT the default .html shell — our harness
# index.html drives the run after a data folder is granted).
em++ $LINKFLAGS "$OBJDIR"/*.o -o "$OUTDIR/blackops.js" 2> "$OUTDIR/link.log"
rc=$?
if [ $rc -ne 0 ]; then
  echo "LINK FAILED (rc=$rc). Undefined symbols (unique):"
  grep -oE "undefined symbol: [^ ]+" "$OUTDIR/link.log" | sort -u | sed 's/^/  /'
  echo "full log: $OUTDIR/link.log"
  exit $rc
fi

# Stage the harness (FS-Access bridge + page) next to the module.
cp -f src/platform/web/index.html "$OUTDIR/index.html"
cp -f src/platform/web/web_fs.js  "$OUTDIR/web_fs.js"

echo "LINK OK -> $OUTDIR/blackops.wasm ($(du -h "$OUTDIR/blackops.wasm" | cut -f1))"
echo "  harness: $OUTDIR/index.html (+ web_fs.js). Serve $OUTDIR and open index.html."
