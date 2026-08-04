#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
clang_format=${CLANG_FORMAT:-clang-format}
ruff=${RUFF:-ruff}

if ! command -v "$clang_format" >/dev/null 2>&1; then
    echo "未找到 ${clang_format}；请安装 clang-format，或通过 CLANG_FORMAT 指定其路径。" >&2
    exit 1
fi

if ! command -v "$ruff" >/dev/null 2>&1; then
    echo "未找到 ${ruff}；请安装 Ruff，或通过 RUFF 指定其路径。" >&2
    exit 1
fi

cpp_files=()
while IFS= read -r -d '' file; do
    cpp_files+=("$file")
done < <(find "$root_dir/components" "$root_dir/main" "$root_dir/tests" -type f \( -name '*.cc' -o -name '*.h' \) -print0)

if ((${#cpp_files[@]})); then
    "$clang_format" --dry-run --Werror "${cpp_files[@]}"
fi

"$ruff" format --check "$root_dir"
"$ruff" check "$root_dir"
