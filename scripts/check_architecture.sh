#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)

cmake -DVOICELIFE_ROOT="$root_dir" -P "$root_dir/scripts/check_architecture.cmake"
