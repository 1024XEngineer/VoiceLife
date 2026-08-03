#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
build_dir=${VOICELIFE_HOST_BUILD_DIR:-"$root_dir/build-host"}

cmake_args=(-S "$root_dir/tests/host" -B "$build_dir")
if [[ ! -f "$build_dir/CMakeCache.txt" ]] && command -v ninja >/dev/null 2>&1; then
    cmake_args+=(-G Ninja)
fi

cmake "${cmake_args[@]}"
cmake --build "$build_dir" --parallel
ctest --test-dir "$build_dir" --output-on-failure "$@"
