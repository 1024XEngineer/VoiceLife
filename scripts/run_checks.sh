#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)

python3 "$root_dir/scripts/check_public_api_docs.py"
"$root_dir/scripts/run_host_tests.sh"
"$root_dir/scripts/check_architecture.sh"
python3 "$root_dir/scripts/firmware.py" validate
python3 -m unittest discover -s "$root_dir/tests/python" -p "test_*.py"
