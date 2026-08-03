#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
build_dir=${VOICELIFE_HOST_BUILD_DIR:-"$root_dir/build-host"}

cmake -S "$root_dir/tests/host" -B "$build_dir" -G Ninja
cmake --build "$build_dir"
ctest --test-dir "$build_dir" --output-on-failure
"$root_dir/scripts/check_architecture.sh"
python3 "$root_dir/scripts/firmware.py" validate
python3 -m unittest discover -s "$root_dir/tests/python" -p "test_*.py"
