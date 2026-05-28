#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="${1:-$HOME/zhangzhisen/RNIC}"
UCX_ROOT="${UCX_ROOT:-/usr/local/ucx}"

cd "$ROOT_DIR"
cmake -S . -B build -DUCX_ROOT="$UCX_ROOT" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
