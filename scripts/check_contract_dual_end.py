#!/usr/bin/env python3
"""Check C++ and TypeScript device contract versions stay aligned.

The shared fixtures and their manifest are the single source of truth for the
wire contract.  This check fails the build if:

- the C++ and TypeScript contract version constants drift apart;
- any valid fixture does not carry the current schemaVersion;
- any fixture on disk is missing from the manifest, or any manifest entry
  refers to a missing file (every fixture must be declared exactly once);
- any declared fixture is not exercised by BOTH the C++ host tests and the
  TypeScript tests, so a fixture change always breaks both ends together.

The manifest (``contracts/im-gateway/v1/fixtures/manifest.json``) annotates
each fixture's contract and expected outcome, instead of relying on the
``*-invalid-*`` filename convention.
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
MANIFEST_FILE = FIXTURES_DIR / "manifest.json"
# 双端消费共享 fixture 的测试入口；任一 fixture 必须被两处同时引用。
CPP_TEST_FILES = [
    ROOT / "tests/host/im_contract_parser_test.cc",
    ROOT / "tests/host/im_gateway_contract_test.cc",
]
TS_TEST_FILE = ROOT / "services/im-gateway/test/run-tests.mjs"


def extract_version(text: str, marker: str) -> str:
    pattern = re.escape(marker) + r'\s*=\s*["\']([^"\']+)["\']'
    match = re.search(pattern, text)
    if match is None:
        sys.exit(f"FAIL 无法从版本常量声明中提取版本: {marker}")
    return match.group(1)


def manifest_fixture_names(manifest: dict) -> tuple[dict[str, str], list[str]]:
    """Flatten the manifest into ``{fixture_name: expected_outcome}``.

    Expected outcomes are ``valid`` or ``invalid``.  A fixture declared more
    than once (across contracts or across outcomes) is reported as a
    duplicate, because the manifest must pin each fixture to exactly one
    contract and outcome.
    """
    by_name: dict[str, str] = {}
    duplicates: list[str] = []
    for _contract, groups in manifest.get("contracts", {}).items():
        for outcome in ("valid", "invalid"):
            for name in groups.get(outcome, []):
                if name in by_name:
                    duplicates.append(name)
                else:
                    by_name[name] = outcome
    return by_name, duplicates


def is_referenced(text: str, fixture_name: str) -> bool:
    """Whether a test source actually references the fixture.

    Matches only the name as a quoted string literal (``"name"`` or
    ``'name'``), so a bare mention in a comment or a longer identifier is
    not mistaken for coverage.
    """
    return f'"{fixture_name}"' in text or f"'{fixture_name}'" in text


def check_fixtures(
    fixtures_dir: Path,
    manifest: dict,
    cpp_test_sources: list[str],
    ts_test_source: str,
    expected_version: str,
) -> list[str]:
    """Return error messages; an empty list means the gate passes."""
    errors: list[str] = []

    on_disk = {path.name for path in fixtures_dir.glob("*.json") if path.name != "manifest.json"}
    by_name, duplicates = manifest_fixture_names(manifest)
    declared = set(by_name)

    for name in sorted(duplicates):
        errors.append(f"FAIL fixture {name} 在 manifest 中重复声明")
    for name in sorted(on_disk - declared):
        errors.append(f"FAIL fixture {name} 未在 manifest 中声明")
    for name in sorted(declared - on_disk):
        errors.append(f"FAIL manifest 声明的 fixture 缺失: {name}")

    for name, outcome in by_name.items():
        path = fixtures_dir / name
        if outcome == "valid" and path.is_file():
            data = json.loads(path.read_text(encoding="utf-8"))
            if data.get("schemaVersion") != expected_version:
                errors.append(f"FAIL 有效 fixture {name} 的 schemaVersion 与双端常量不一致")

    for name in sorted(declared):
        if not any(is_referenced(text, name) for text in cpp_test_sources):
            errors.append(f"FAIL fixture {name} 未接入 C++ 主机测试")
        if not is_referenced(ts_test_source, name):
            errors.append(f"FAIL fixture {name} 未接入 TypeScript 测试")

    return errors


def main() -> int:
    cpp_version = extract_version(CPP_VERSION_HEADER.read_text(encoding="utf-8"), "kDeviceContractVersion")
    ts_version = extract_version(TS_VERSION_SOURCE.read_text(encoding="utf-8"), "DEVICE_CONTRACT_VERSION")
    if cpp_version != ts_version:
        sys.exit(f"FAIL 双端契约版本不一致: C++={cpp_version}, TypeScript={ts_version}")

    manifest = json.loads(MANIFEST_FILE.read_text(encoding="utf-8"))
    if manifest.get("schemaVersion") != ts_version:
        sys.exit("FAIL manifest 声明的 schemaVersion 与双端常量不一致")

    errors = check_fixtures(
        FIXTURES_DIR,
        manifest,
        [path.read_text(encoding="utf-8") for path in CPP_TEST_FILES],
        TS_TEST_FILE.read_text(encoding="utf-8"),
        ts_version,
    )
    if errors:
        sys.exit("\n".join(errors))

    print(f"PASS 双端契约版本一致 ({ts_version})，全部 fixture 已声明并被双端测试覆盖")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
