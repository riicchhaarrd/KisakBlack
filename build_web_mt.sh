#!/bin/bash
# build_web_mt.sh — MULTITHREADED (Emscripten pthreads / Web Workers) compile sweep.
#
# Same as build_web.sh but adds -pthread on BOTH compile and link, and writes objects
# to a SEPARATE tree (build_web_mt/obj) so the single-thread/fiber build's objects are
# never mixed with the pthreads ones (-pthread changes TLS/atomics codegen, so the two
# object sets are NOT interchangeable). Emscripten defines __EMSCRIPTEN_PTHREADS__ when
# -pthread is passed; the engine uses that to take the real-thread (RB_RenderThread on
# its own worker) path instead of the cooperative single-OS-thread fiber path.
#
# Usage:
#   ./build_web_mt.sh                # full sweep (all C++ TUs + zlib/jpeg C)
#   ./build_web_mt.sh <substr> ...   # sweep only TUs whose path matches a substring
# Output (all under the gitignored build_web_mt/):
#   build_web_mt/obj/*.o             # objects for TUs that compiled
#   build_web_mt/logs/<tu>.log       # first compiler error for TUs that failed
#   build_web_mt/pass.txt            # one line per TU that compiled
#   build_web_mt/fail.txt            # one line per TU that failed
set -u
cd "$(dirname "$0")"
mkdir -p build_web_mt/obj build_web_mt/logs
: > build_web_mt/pass.txt
: > build_web_mt/fail.txt

# --- toolchain bring-up: writable Emscripten cache + un-frozen config (shared) ---
# Same provisioning as build_web.sh. NOTE: -pthread needs the SDL2 + sysroot ports
# rebuilt WITH -pthread; Emscripten keys those by flags, so the first MT run will
# build a pthread-flavoured libSDL2.a / libc into the same writable cache (idempotent).
SYS_EM_CACHE="$(dirname "$(command -v em++)")/cache"
[ -d "$SYS_EM_CACHE" ] || SYS_EM_CACHE="/usr/share/emscripten/cache"
SYS_EM_CONFIG="/usr/share/emscripten/.emscripten"
: "${EM_CACHE_DIR:=$HOME/.emscripten_cache}"
: "${EM_CONFIG:=$HOME/.emscripten_web_config}"
export EM_CONFIG
if [ ! -d "$EM_CACHE_DIR/sysroot" ]; then
  echo "provisioning writable Emscripten cache -> $EM_CACHE_DIR (one-time copy)..."
  cp -a "$SYS_EM_CACHE" "$EM_CACHE_DIR"
fi
if [ ! -f "$EM_CONFIG" ]; then
  echo "writing un-frozen Emscripten config -> $EM_CONFIG"
  sed -e 's/^FROZEN_CACHE = True.*/FROZEN_CACHE = False/' "$SYS_EM_CONFIG" > "$EM_CONFIG"
  grep -q '^CACHE' "$EM_CONFIG" || echo "CACHE = '$EM_CACHE_DIR'" >> "$EM_CONFIG"
fi

# --- include dirs: identical to build_web.sh / build_linux.sh ---
INCS="-I. -Isrc -Isrc/platform/winsdk -Isrc/libs -Isrc/libs/libtomcrypt-1.17/src/headers -Isrc/libs/libtommath-1.0 -Isrc/libs/libvpx-1.5.0/include -IDemonWare -Itl -Isrc/jpeg"

# --- C++ flags --- (build_web.sh CXXFLAGS + -pthread)
# -pthread makes the TU emit thread-local / atomic codegen and defines
# __EMSCRIPTEN_PTHREADS__ so the engine selects the real-thread path.
CXXFLAGS="-std=c++20 -c -w -fpermissive -Wno-narrowing \
  -pthread \
  -msimd128 -msse -msse2 \
  -include src/platform/compat/msvc_compat.h -DKISAK_MP -DKISAK_WEB"

# Emscripten settings that affect *compilation* (SDL2 + GLES3 headers + pthreads).
EMFLAGS="-pthread -sUSE_SDL=2 -sMAX_WEBGL_VERSION=2 -sMIN_WEBGL_VERSION=2 -sFULL_ES3=1 \
  -sALLOW_MEMORY_GROWTH=1 -O2"

# --- C flags (bundled zlib + jpeg), mirroring build_web.sh + -pthread ---
CFLAGS="-std=gnu11 -w -fpermissive -c -pthread -DKISAK_WEB \
  -D__cdecl= -D__stdcall= -D__fastcall= -D__int8=char -D__int16=short -D__int32=int"

# --- engine C++ TU list: identical glob to build_web.sh ---
mapfile -t CPP < <(git ls-files 'src/*.cpp' 'DemonWare/*.cpp' 'tl/*.cpp' 'src/platform/linux/*.cpp' 'src/audio_openal/al_audio.cpp' \
  | grep -vE '^src/(libs|tracy|steam|nvapi|binklib|win32)/' | grep -vE 'gfx_gl/tests/|audio_openal/tests/')

# optional path filters
if [ "$#" -gt 0 ]; then
  FILTERED=()
  for f in "${CPP[@]}"; do for pat in "$@"; do [[ "$f" == *"$pat"* ]] && { FILTERED+=("$f"); break; }; done; done
  CPP=("${FILTERED[@]}")
fi

# Warm up the (pthread-flavoured) SDL2 port + sysroot serially so the parallel sweep
# below doesn't race multiple emcc processes building the same cached lib.
echo "warming Emscripten ports (SDL2, pthread)..."
echo 'int main(){return 0;}' > build_web_mt/.warmup.c
emcc $EMFLAGS build_web_mt/.warmup.c -o build_web_mt/.warmup.o -c >/dev/null 2>&1 || true
rm -f build_web_mt/.warmup.c build_web_mt/.warmup.o

echo "sweeping ${#CPP[@]} C++ TUs with em++ (wasm32, -pthread)..."

compile_one() {
  local tu="$1"; local tag; tag="$(echo "$tu" | tr / _)"
  local obj="build_web_mt/obj/${tag}.o"; local log="build_web_mt/logs/${tag}.log"
  if em++ $CXXFLAGS $EMFLAGS $INCS "$tu" -o "$obj" 2> "$log"; then
    echo "$tu" >> build_web_mt/pass.txt
    printf 'PASS %s\n' "$tu"
  else
    local first; first="$(grep -m1 -E 'error:|fatal error:' "$log" | head -1)"
    printf '%s\t%s\n' "$tu" "${first:-<no error: line — see log>}" >> build_web_mt/fail.txt
    printf 'FAIL %s\n' "$tu"
  fi
}
export -f compile_one
export CXXFLAGS EMFLAGS INCS EM_CONFIG

printf '%s\n' "${CPP[@]}" | xargs -P"$(nproc)" -I{} bash -c 'compile_one "{}"'

# --- bundled zlib + jpeg C (only on a full sweep) ---
if [ "$#" -eq 0 ]; then
  echo "sweeping bundled zlib C..."
  for f in $(git ls-files 'src/zlib/*.c'); do
    tag="$(echo "$f" | tr / _)"
    if emcc $CFLAGS $EMFLAGS -Isrc/zlib "$f" -o "build_web_mt/obj/${tag}.o" 2> "build_web_mt/logs/${tag}.log"; then
      echo "$f" >> build_web_mt/pass.txt; printf 'PASS %s\n' "$f"
    else
      printf '%s\t%s\n' "$f" "$(grep -m1 -E 'error:|fatal error:' build_web_mt/logs/${tag}.log)" >> build_web_mt/fail.txt
      printf 'FAIL %s\n' "$f"
    fi
  done

  echo "sweeping bundled jpeg C..."
  for f in $(git ls-files 'src/jpeg/*.c' | grep -vE 'jmemansi|jmemname'); do
    tag="$(echo "$f" | tr / _)"
    if emcc $CFLAGS $EMFLAGS -I. -Isrc -Isrc/jpeg -Isrc/platform/winsdk "$f" -o "build_web_mt/obj/${tag}.o" 2> "build_web_mt/logs/${tag}.log"; then
      echo "$f" >> build_web_mt/pass.txt; printf 'PASS %s\n' "$f"
    else
      printf '%s\t%s\n' "$f" "$(grep -m1 -E 'error:|fatal error:' build_web_mt/logs/${tag}.log)" >> build_web_mt/fail.txt
      printf 'FAIL %s\n' "$f"
    fi
  done
fi

# --- summary ---
P=$(wc -l < build_web_mt/pass.txt); F=$(wc -l < build_web_mt/fail.txt)
echo "================================================================"
echo "WASM-MT compile sweep: $P passed, $F failed  (of $((P+F)) TUs)"
echo "  passes: build_web_mt/pass.txt   failures+first-error: build_web_mt/fail.txt   logs: build_web_mt/logs/"
echo "================================================================"
