#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
project_dir=$(cd "$script_dir/.." && pwd)
test_dir="$project_dir/tests/voicelife"
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/voicelife-host.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT

cjson_dir="$project_dir/managed_components/espressif__cjson/cJSON"
common_flags=(
  -std=c++17 -Wall -Wextra -Werror -Wno-unused-private-field
  -I"$test_dir/host_stubs"
  -I"$test_dir"
  -I"$project_dir/main/voicelife"
  -I"$cjson_dir"
)

cc -std=c11 -Wall -Wextra -Werror -I"$cjson_dir" -c "$cjson_dir/cJSON.c" -o "$build_dir/cJSON.o"
c++ "${common_flags[@]}" \
  "$test_dir/voicelife_service_host_test.cc" \
  "$project_dir/main/voicelife/voicelife_domain.cc" \
  "$project_dir/main/voicelife/voicelife_service.cc" \
  "$build_dir/cJSON.o" -o "$build_dir/voicelife_service_host_test"

"$build_dir/voicelife_service_host_test"

c++ "${common_flags[@]}" \
  "$test_dir/voicelife_turn_policy_host_test.cc" \
  "$project_dir/main/voicelife/voicelife_turn_policy.cc" \
  -o "$build_dir/voicelife_turn_policy_host_test"

"$build_dir/voicelife_turn_policy_host_test"

c++ "${common_flags[@]}" \
  "$test_dir/voicelife_im_sync_host_test.cc" \
  "$project_dir/main/voicelife/voicelife_domain.cc" \
  "$project_dir/main/voicelife/voicelife_service.cc" \
  "$project_dir/main/voicelife/voicelife_im_sync.cc" \
  "$build_dir/cJSON.o" -o "$build_dir/voicelife_im_sync_host_test"

"$build_dir/voicelife_im_sync_host_test"
