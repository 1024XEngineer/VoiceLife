#!/usr/bin/env python3
"""Check C++ and TypeScript device contract versions stay aligned.

The shared fixtures are the single source of truth for the wire contract.
A change to the contract version must land on both ends at once; this check
fails the build if either end drifts, or if any valid fixture carries a
different schemaVersion.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CPP_VERSION_HEADER = ROOT / "components/voicelife_contracts/include/voicelife/contracts/im/im_contracts.h"
TS_VERSION_SOURCE = ROOT / "services/im-gateway/src/contracts/device-gateway.ts"
FIXTURES_DIR = ROOT / "contracts/im-gateway/v1/fixtures"

# 网关下发响应中唯一不带 schemaVersion 的契约：NotificationSubmission 的 TS
# 接口与运行时响应均无版本字段（见 device-gateway.ts 与 services.ts 的
# submission 对象），因此其全部 fixture 不参与版本一致性检查。
VERSIONLESS_FIXTURE_PREFIX = "notification-submission"


def extract_version(text: str, marker: str) -> str:
    pattern = re.escape(marker) + r'\s*=\s*["\']([^"\']+)["\']'
    match = re.search(pattern, text)
    if match is None:
        sys.exit(f"FAIL 无法从版本常量声明中提取版本: {marker}")
    return match.group(1)


def main() -> int:
    cpp_version = extract_version(CPP_VERSION_HEADER.read_text(encoding="utf-8"), "kDeviceContractVersion")
    ts_version = extract_version(TS_VERSION_SOURCE.read_text(encoding="utf-8"), "DEVICE_CONTRACT_VERSION")
    if cpp_version != ts_version:
        sys.exit(f"FAIL 双端契约版本不一致: C++={cpp_version}, TypeScript={ts_version}")

    for fixture in sorted(FIXTURES_DIR.glob("*.json")):
        if "-invalid-" in fixture.name or fixture.name.startswith(VERSIONLESS_FIXTURE_PREFIX):
            continue
        data = json.loads(fixture.read_text(encoding="utf-8"))
        if data.get("schemaVersion") != ts_version:
            sys.exit(f"FAIL 有效 fixture {fixture.name} 的 schemaVersion 与双端常量不一致")

    print(f"PASS 双端契约版本一致 ({ts_version})，全部有效 fixture 携带该版本")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
