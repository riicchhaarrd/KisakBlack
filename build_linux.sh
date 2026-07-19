#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

: "${CMAKE_BUILD_TYPE:=RelWithDebInfo}"
cmake -S . -B build_linux -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE"
cmake --build build_linux --parallel
