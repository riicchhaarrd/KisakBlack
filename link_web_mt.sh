#!/bin/bash
# link_web_mt.sh — LINK the MULTITHREADED (Emscripten pthreads / Web Workers) build.
#
# Prereq: run ./build_web_mt.sh first so build_web_mt/obj/*.o exists.
# This links the pthread-flavoured objects into build_web_mt/web/blackops.{js,wasm}
# and stages the harness alongside.
#
# KEY DIFFERENCES vs link_web.sh (single-thread/fiber):
#   * -pthread                          : enable Web Workers + SharedArrayBuffer.
#   * -sPROXY_TO_PTHREAD                : run main() on a dedicated worker (so it can
#                                         BLOCK on condvars), leaving the browser DOM
#                                         thread free for canvas/events.
#   * -sPTHREAD_POOL_SIZE=24            : pre-spawn workers (BACKEND/SERVER/STREAM/
#                                         DATABASE/OCCLUSION + jobqueue + the proxied
#                                         main = well over a dozen live threads).
#   * -sALLOW_BLOCKING_ON_MAIN_THREAD   : the proxied "main" worker may block (it is
#                                         not the DOM thread, so this is safe).
#   * -sOFFSCREENCANVAS_SUPPORT=1       : let the render worker own/transfer the canvas
#                                         where the browser supports it (PROXY_FALLBACK
#                                         otherwise — see glcontext_sdl.cpp).
#   * NO -sASYNCIFY                     : engine threads block on real pthread condvars;
#                                         FS reads proxy to main via an atomic doorbell
#                                         (web_fs.cpp __EMSCRIPTEN_PTHREADS__ path), so
#                                         the per-read Asyncify cost is gone.
#   * EXPORTED_RUNTIME_METHODS adds HEAP32/HEAPF64/stringToNewUTF8 + _malloc keep-alive
#                                         for the MAIN_THREAD_ASYNC_EM_ASM FS proxy.
#
# Run with ERR_UNDEF=0 to allow JS-resolved imports on the first triage pass.
set -u
cd "$(dirname "$0")"

# --- same toolchain bring-up as build_web_mt.sh (writable cache, un-frozen config) -
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

OBJDIR="build_web_mt/obj"
OUTDIR="${KB_OUTDIR:-build_web_mt/web}"
mkdir -p "$OUTDIR"
if ! ls "$OBJDIR"/*.o >/dev/null 2>&1; then
  echo "no objects in $OBJDIR — run ./build_web_mt.sh first" >&2; exit 1
fi
NOBJ=$(ls "$OBJDIR"/*.o | wc -l)
echo "linking $NOBJ pthread objects -> $OUTDIR/blackops.{js,wasm} ..."

LINKFLAGS="\
  -pthread \
  -sPROXY_TO_PTHREAD \
  -sPTHREAD_POOL_SIZE=24 \
  -sALLOW_BLOCKING_ON_MAIN_THREAD \
  -sOFFSCREEN_FRAMEBUFFER=1 \
  -sUSE_SDL=2 \
  -sMAX_WEBGL_VERSION=2 -sMIN_WEBGL_VERSION=2 -sFULL_ES3=1 \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=512MB -sMAXIMUM_MEMORY=2GB \
  -sSTACK_SIZE=8MB \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,callMain,FS,stringToUTF8,stringToNewUTF8,UTF8ToString,lengthBytesUTF8,HEAPU8,HEAP32,HEAPF64 \
  -sEXPORTED_FUNCTIONS=_main,_malloc,_free \
  -lopenal \
  -sALLOW_TABLE_GROWTH=1 \
  -sASSERTIONS=1 \
  -sEMULATE_FUNCTION_POINTER_CASTS=1 \
  -sINVOKE_RUN=0 \
  -O2 --profiling-funcs"
# NOTE: ASYNCIFY (the de-proxy render-thread event-loop yield, KB_RenderThreadYield ->
# emscripten_sleep) lives on the `deproxy` branch. The proxied build (this branch's focus)
# doesn't need it (the DOM thread has an event loop), and it added ~6MB + instrumented the
# render loop, so it's omitted here. KB_RenderThreadYield is a no-op stub on this branch.
# WHY these:
#   PROXY_TO_PTHREAD: main() (the game loop) runs on a worker so it can block on the
#     DX-device-ownership lock / Win32 events that RB_RenderThread (another worker)
#     signals — the exact trade the cooperative single-thread build deadlocked on.
#   OFFSCREENCANVAS_SUPPORT: the render worker creates its WebGL2 context directly via
#     emscripten_webgl_create_context (glcontext_sdl.cpp, __EMSCRIPTEN_PTHREADS__),
#     proxying to the DOM thread when the canvas isn't transferred (PROXY_FALLBACK).
#   _malloc/_free + stringToNewUTF8: the main-thread FS proxy allocates the directory
#     listing buffer in the shared heap (web_fs.cpp kbweb_listdir).
#   INVOKE_RUN=0: the harness calls main() only after a data folder is granted.

: "${ERR_UNDEF:=1}"
if [ "${ERR_UNDEF}" = "1" ]; then
  LINKFLAGS="$LINKFLAGS -sERROR_ON_UNDEFINED_SYMBOLS=1"
else
  LINKFLAGS="$LINKFLAGS -sERROR_ON_UNDEFINED_SYMBOLS=0"
fi

# DE-PROXY the render thread (KB_DEPROXY=1): transfer the page <canvas> to the BACKEND
# worker so its WebGL2 context renders LOCALLY instead of marshaling every GL call to the
# DOM thread (the ~8000-proxied-draws/frame bottleneck -> single-digit fps).
#
# Deliberately NO -sOFFSCREENCANVASES_TO_PTHREAD: transferring DOM->main-worker at startup
# means the later canvas-carrying backend create can't be proxied (the canvas object lives
# on the blocked main worker), so Emscripten spawns a NESTED worker whose startup handshake
# queues on the main worker's never-pumped event loop -> boot hang at com=0. Without the
# startup transfer, the backend pthread_create is proxied to the DOM thread, which still
# owns the canvas and does transferControlToOffscreen() itself onto a pool worker.
# The C side passes the raw id "canvas" (win_kernel.cpp), and SwapBuffers manually presents
# via transferToImageBitmap (glcontext_sdl.cpp) because implicit OffscreenCanvas commit
# requires an event-loop yield the render thread never makes.
: "${KB_DEPROXY:=0}"
if [ "${KB_DEPROXY}" = "1" ]; then
  LINKFLAGS="$LINKFLAGS -sOFFSCREENCANVAS_SUPPORT=1"
  echo "  [KB_DEPROXY] OffscreenCanvas de-proxy ENABLED (DOM-thread transfer -> backend worker)"
fi
# KB_SAFEHEAP=1: instrument heap accesses (-sSAFE_HEAP=2: OOB only, alignment checks OFF -- the decompiled packed structs trip =1 instantly on a benign dvar access) to catch the wild write
# that nulls the runtime's main-thread global (the in-game normalize_thread abort) AT
# THE WRITE SITE with a stack. 2-3x slower — diagnostic runs only.
: "${KB_SAFEHEAP:=0}"
if [ "${KB_SAFEHEAP}" = "1" ]; then
  LINKFLAGS="$LINKFLAGS -sSAFE_HEAP=2"
  echo "  [KB_SAFEHEAP] SAFE_HEAP instrumentation ENABLED (slow, diagnostic)"
fi

# KB_GLDEBUG=1: enable Emscripten's dbg() tracing in the GL + canvas-transfer paths
# (library_html5_webgl / library_pthread). Diagnostic builds only — very chatty.
: "${KB_GLDEBUG:=0}"
if [ "${KB_GLDEBUG}" = "1" ]; then
  LINKFLAGS="$LINKFLAGS -sGL_DEBUG=1"
  echo "  [KB_GLDEBUG] GL/canvas-transfer dbg tracing ENABLED"
fi

set -o pipefail
em++ $LINKFLAGS "$OBJDIR"/*.o -o "$OUTDIR/blackops.js" 2> "$OUTDIR/link.log"
rc=$?
if [ $rc -ne 0 ]; then
  echo "LINK FAILED (rc=$rc). Undefined symbols (unique):"
  grep -oE "undefined symbol: [^ ]+" "$OUTDIR/link.log" | sort -u | sed 's/^/  /'
  echo "full log: $OUTDIR/link.log"
  exit $rc
fi

# Stage the harness + the COOP/COEP serve script next to the module.
cp -f src/platform/web/index.html "$OUTDIR/index.html"
cp -f src/platform/web/web_fs.js  "$OUTDIR/web_fs.js"
cp -f serve_web_mt.py             "$OUTDIR/serve_web_mt.py" 2>/dev/null || true

echo "LINK OK -> $OUTDIR/blackops.wasm ($(du -h "$OUTDIR/blackops.wasm" | cut -f1))"
echo "  harness: $OUTDIR/index.html (+ web_fs.js)."
echo "  serve cross-origin-isolated (SharedArrayBuffer): python3 serve_web_mt.py  (from repo root or $OUTDIR)"
