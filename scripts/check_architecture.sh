#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)

assert_requires() {
    local component=$1
    shift
    local cmake_file="$root_dir/components/$component/CMakeLists.txt"
    local contents
    contents=$(tr '\n' ' ' < "$cmake_file")
    for required in "$@"; do
        if [[ "$contents" != *"$required"* ]]; then
            echo "FAIL $component 缺少声明依赖 $required" >&2
            exit 1
        fi
    done
}

assert_not_requires() {
    local component=$1
    shift
    local cmake_file="$root_dir/components/$component/CMakeLists.txt"
    local contents
    contents=$(tr '\n' ' ' < "$cmake_file")
    for forbidden in "$@"; do
        if [[ "$contents" == *"$forbidden"* ]]; then
            echo "FAIL $component 不得向外依赖 $forbidden" >&2
            exit 1
        fi
    done
}

assert_requires voicelife_schedule voicelife_contracts
assert_requires voicelife_timing voicelife_contracts
assert_requires voicelife_application voicelife_schedule voicelife_timing
assert_requires voicelife_mcp voicelife_application
assert_requires voicelife_im voicelife_application

assert_not_requires voicelife_contracts voicelife_schedule voicelife_timing voicelife_application voicelife_mcp voicelife_voice voicelife_im voicelife_platform voicelife_runtime
assert_not_requires voicelife_schedule voicelife_timing voicelife_application voicelife_mcp voicelife_voice voicelife_im voicelife_platform voicelife_runtime
assert_not_requires voicelife_timing voicelife_schedule voicelife_application voicelife_mcp voicelife_voice voicelife_im voicelife_platform voicelife_runtime
assert_not_requires voicelife_voice voicelife_schedule voicelife_timing voicelife_application voicelife_mcp voicelife_im voicelife_platform voicelife_runtime

echo "PASS component dependency rules"
