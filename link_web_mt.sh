#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
exec ./build_web_mt.sh
