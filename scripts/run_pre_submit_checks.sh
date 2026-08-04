#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
base_ref=${VOICELIFE_BASE_SHA:-origin/main}

echo "==> C/C++、Python 格式与静态规则"
"$root_dir/scripts/check_format.sh"

echo "==> 变更源码规模"
python3 "$root_dir/scripts/check_code_size.py" --base "$base_ref"

echo "==> 公共 API、主机测试、架构与固件配置"
"$root_dir/scripts/run_checks.sh"

gateway_dir="$root_dir/services/im-gateway"
if [[ -f "$gateway_dir/package.json" ]]; then
    if ! command -v node >/dev/null 2>&1 || [[ "$(node --version)" != v24.* ]]; then
        echo "需要 Node.js 24 才能运行 IM Gateway 门禁。" >&2
        exit 1
    fi
    pnpm_version=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["packageManager"].split("@", 1)[1])' "$gateway_dir/package.json")
    echo "==> IM Gateway（pnpm ${pnpm_version}）"
    corepack "pnpm@${pnpm_version}" --dir "$gateway_dir" install --frozen-lockfile
    corepack "pnpm@${pnpm_version}" --dir "$gateway_dir" run ci
fi

echo "PASS 提交前完整门禁通过"
