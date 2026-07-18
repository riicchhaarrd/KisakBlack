#!/bin/bash
# build_web.sh — Milestone-1 WASM/WebGL2 compile sweep for the KisakBlack engine.
#
# This is NOT a full link/run build. Its job is to compile every engine TU to a
# WASM object with em++ (Emscripten 3.1.69), keep going past failures, and log a
# per-file pass/fail so we can triage what does and does not port to the web.
#
# Mirrors build_linux.sh's source globs / include dirs. Differences vs Linux:
#   * em++ instead of g++; target is wasm32 (not -m32 x86).
#   * x86-isms dropped: NO -malign-double, NO -mmmx. SSE intrinsics are emulated on
#     WASM SIMD via -msimd128 -msse -msse2 (Emscripten ships shim xmmintrin.h etc.).
#   * SDL2 + WebGL2/GLES3 come from Emscripten ports (-sUSE_SDL=2, FULL_ES3), so the
#     desktop <GL/glew.h> path will surface in the sweep as a real finding.
#   * -DKISAK_WEB so source can grow #ifdef __EMSCRIPTEN__ / KISAK_WEB branches.
#
# Usage:
#   ./build_web.sh                # full sweep (all 686 C++ TUs + zlib/jpeg C)
#   ./build_web.sh <substr> ...   # sweep only TUs whose path matches a substring
# Output:
#   build_web/obj/*.o             # objects for TUs that compiled
#   build_web/logs/<tu>.log       # first compiler error for TUs that failed
#   build_web/pass.txt            # one line per TU that compiled
#   build_web/fail.txt            # one line per TU that failed
set -u
cd "$(dirname "$0")"
mkdir -p build_web/obj build_web/logs
: > build_web/pass.txt
: > build_web/fail.txt

# --- toolchain bring-up: writable Emscripten cache + un-frozen config ----------
# The apt Emscripten ships a root-owned cache and a config with FROZEN_CACHE=True,
# so emcc cannot build/fetch ports (SDL2) or the sysroot stamp. We provision a
# writable copy of the cache once and point a private EM_CONFIG at it with
# FROZEN_CACHE=False. (Override either via EM_CACHE_DIR / EM_CONFIG in the env.)
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

# --- include dirs: identical to build_linux.sh ---
INCS="-I. -Isrc -Isrc/platform/winsdk -Isrc/libs -Isrc/libs/libtomcrypt-1.17/src/headers -Isrc/libs/libtommath-1.0 -Isrc/libs/libvpx-1.5.0/include -IDemonWare -Itl -Isrc/jpeg"

# --- C++ flags ---
# Same compat surface as Linux (-std=c++20, the forced msvc_compat.h include,
# -DKISAK_MP) MINUS the x86-only knobs, PLUS WASM SIMD/SSE.
# -msimd128 -msse -msse2 lets Emscripten's <*mmintrin.h> shims lower _mm_* to WASM SIMD.
#
# NOTE: we deliberately DROP -fms-extensions on the web build. common.h includes
# <xmmintrin.h> (for its _mm_cvtss_si32 fast-round path), and Emscripten's SSE
# compat headers define _mm_storeu_ps/_pd via a block-scoped `struct __unaligned`
# that -fms-extensions misparses ("declaration of anonymous struct must be a
# definition"). Because common.h is included almost everywhere, -fms-extensions
# poisons ~535/686 TUs. Dropping it flips the C++ sweep from ~161 to ~500 passing.
# A handful of TUs that genuinely need MS anonymous-struct semantics (e.g.
# snd_dsp.cpp) regress and are tracked in the triage as the "ms-extensions" bucket.
CXXFLAGS="-std=c++20 -c -g -w -fpermissive -Wno-narrowing \
  -msimd128 -msse -msse2 \
  -include src/platform/compat/msvc_compat.h -DKISAK_MP -DKISAK_WEB"

# Emscripten settings that affect *compilation* (SDL2 + GLES3 headers). The -s link
# settings (MAX_WEBGL_VERSION etc.) only matter at link time but are harmless here
# and document intent for milestone 2.
EMFLAGS="-sUSE_SDL=2 -sMAX_WEBGL_VERSION=2 -sMIN_WEBGL_VERSION=2 -sFULL_ES3=1 \
  -sALLOW_MEMORY_GROWTH=1 -O0"

# --- C flags (bundled zlib + jpeg), mirroring build_linux minus -m32 ---
CFLAGS="-std=gnu11 -w -fpermissive -c -DKISAK_WEB \
  -D__cdecl= -D__stdcall= -D__fastcall= -D__int8=char -D__int16=short -D__int32=int"

# --- engine C++ TU list: identical glob to build_linux.sh ---
mapfile -t CPP < <(git ls-files 'src/*.cpp' 'DemonWare/*.cpp' 'tl/*.cpp' 'src/platform/linux/*.cpp' 'src/audio_openal/al_audio.cpp' \
  | grep -vE '^src/(libs|tracy|steam|nvapi|binklib|win32)/' | grep -vE 'gfx_gl/tests/|audio_openal/tests/')

# optional path filters
if [ "$#" -gt 0 ]; then
  FILTERED=()
  for f in "${CPP[@]}"; do for pat in "$@"; do [[ "$f" == *"$pat"* ]] && { FILTERED+=("$f"); break; }; done; done
  CPP=("${FILTERED[@]}")
fi

# Warm up the SDL2 port + sysroot serially so the parallel sweep below doesn't race
# multiple emcc processes building the same cached lib. (No-op once cached.)
echo "warming Emscripten ports (SDL2)..."
echo 'int main(){return 0;}' > build_web/.warmup.c
emcc $EMFLAGS build_web/.warmup.c -o build_web/.warmup.o -c >/dev/null 2>&1 || true
rm -f build_web/.warmup.c build_web/.warmup.o

echo "sweeping ${#CPP[@]} C++ TUs with em++ (wasm32)..."

# Per-TU compile: never abort the sweep. Capture stderr to a log; on failure keep
# just the first error line in fail.txt so the triage stays readable.
compile_one() {
  local tu="$1"; local tag; tag="$(echo "$tu" | tr / _)"
  local obj="build_web/obj/${tag}.o"; local log="build_web/logs/${tag}.log"
  if em++ $CXXFLAGS $EMFLAGS $INCS "$tu" -o "$obj" 2> "$log"; then
    echo "$tu" >> build_web/pass.txt
    printf 'PASS %s\n' "$tu"
  else
    local first; first="$(grep -m1 -E 'error:|fatal error:' "$log" | head -1)"
    printf '%s\t%s\n' "$tu" "${first:-<no error: line — see log>}" >> build_web/fail.txt
    printf 'FAIL %s\n' "$tu"
  fi
}
export -f compile_one
export CXXFLAGS EMFLAGS INCS EM_CONFIG

printf '%s\n' "${CPP[@]}" | xargs -P"$(nproc)" -I{} bash -c 'compile_one "{}"'

# --- bundled zlib C (only on a full sweep) ---
if [ "$#" -eq 0 ]; then
  echo "sweeping bundled zlib C..."
  for f in $(git ls-files 'src/zlib/*.c'); do
    tag="$(echo "$f" | tr / _)"
    if emcc $CFLAGS $EMFLAGS -Isrc/zlib "$f" -o "build_web/obj/${tag}.o" 2> "build_web/logs/${tag}.log"; then
      echo "$f" >> build_web/pass.txt; printf 'PASS %s\n' "$f"
    else
      printf '%s\t%s\n' "$f" "$(grep -m1 -E 'error:|fatal error:' build_web/logs/${tag}.log)" >> build_web/fail.txt
      printf 'FAIL %s\n' "$f"
    fi
  done

  echo "sweeping bundled jpeg C..."
  for f in $(git ls-files 'src/jpeg/*.c' | grep -vE 'jmemansi|jmemname'); do
    tag="$(echo "$f" | tr / _)"
    if emcc $CFLAGS $EMFLAGS -I. -Isrc -Isrc/jpeg -Isrc/platform/winsdk "$f" -o "build_web/obj/${tag}.o" 2> "build_web/logs/${tag}.log"; then
      echo "$f" >> build_web/pass.txt; printf 'PASS %s\n' "$f"
    else
      printf '%s\t%s\n' "$f" "$(grep -m1 -E 'error:|fatal error:' build_web/logs/${tag}.log)" >> build_web/fail.txt
      printf 'FAIL %s\n' "$f"
    fi
  done
fi

# Standalone WMAv2 decoder used by the OpenAL xWMA path.
echo "sweeping libwma (WMAv2 decoder) C..."
for f in wmadeci wmafixed mdct mdct_lookup fft-ffmpeg ffmpeg_bitstream wma_decode; do
  src="src/audio_openal/libwma/${f}.c"; tag="$(echo "$src" | tr / _)"
  if emcc $CFLAGS $EMFLAGS -Isrc/audio_openal/libwma "$src" -o "build_web/obj/${tag}.o" 2> "build_web/logs/${tag}.log"; then
    echo "$src" >> build_web/pass.txt; printf 'PASS %s\n' "$src"
  else
    printf '%s\t%s\n' "$src" "$(grep -m1 -E 'error:|fatal error:' build_web/logs/${tag}.log)" >> build_web/fail.txt
    printf 'FAIL %s\n' "$src"
  fi
done

# --- summary ---
P=$(wc -l < build_web/pass.txt); F=$(wc -l < build_web/fail.txt)
echo "================================================================"
echo "WASM compile sweep: $P passed, $F failed  (of $((P+F)) TUs)"
echo "  passes: build_web/pass.txt   failures+first-error: build_web/fail.txt   logs: build_web/logs/"
echo "================================================================"
