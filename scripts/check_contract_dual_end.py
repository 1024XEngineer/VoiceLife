#!/usr/bin/env python3
"""Check C++ and TypeScript device contract versions stay aligned.

The shared fixtures and their manifest are the single source of truth for the
wire contract.  This check fails the build if:

- the C++ and TypeScript contract version constants drift apart;
- any valid fixture does not carry the current schemaVersion;
- any fixture on disk is missing from the manifest, or any manifest entry
  refers to a missing file (every fixture must be declared exactly once);
- any declared fixture is not referenced by the C++ host tests (and by the
  TypeScript tests for inbound contracts) — a static reference check, not an
  execution-coverage proof, so a fixture change always breaks its consumers.

The manifest (``contracts/im-gateway/v1/fixtures/manifest.json``) annotates
each fixture's contract and expected outcome, instead of relying on the
``*-invalid-*`` filename convention.  Two optional top-level keys refine the
checks: ``versionless`` lists contracts whose fixtures intentionally carry no
``schemaVersion`` (e.g. the gateway→device ``notification-submission``
response), and ``outbound`` lists gateway→device contracts consumed only by
the C++ host tests (the TypeScript gateway generates them and is covered by
runtime tests, not fixture parsers).
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


def manifest_fixture_names(manifest: dict) -> tuple[dict[str, str], list[tuple[str, str, str]]]:
    """Flatten the manifest into ``{fixture_name: expected_outcome}``.

    Expected outcomes are ``valid`` or ``invalid``.  A fixture declared more
    than once (across contracts or across outcomes) is reported with its two
    declaring ``contract/outcome`` locations, because the manifest must pin
    each fixture to exactly one contract and outcome.
    """
    by_name: dict[str, str] = {}
    first_seen: dict[str, tuple[str, str]] = {}
    duplicates: list[tuple[str, str, str]] = []
    for contract, groups in manifest.get("contracts", {}).items():
        for outcome in ("valid", "invalid"):
            for name in groups.get(outcome, []):
                if name in by_name:
                    first = f"{first_seen[name][0]}/{first_seen[name][1]}"
                    second = f"{contract}/{outcome}"
                    duplicates.append((name, first, second))
                else:
                    by_name[name] = outcome
                    first_seen[name] = (contract, outcome)
    return by_name, duplicates


def strip_comments(text: str) -> str:
    """Remove C/JS-style comments from test source.

    Coverage is judged by quoted fixture names in code; a mention inside a
    comment (quoted or not) must never count as a real reference.
    """
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def is_referenced(text: str, fixture_name: str) -> bool:
    """Whether a comment-stripped test source references the fixture.

    Matches only the name as a quoted string literal (``"name"`` or
    ``'name'``).  The caller strips comments first, so a mention inside a
    comment is never mistaken for coverage.
    """
    return f'"{fixture_name}"' in text or f"'{fixture_name}'" in text


def load_manifest(path: Path) -> tuple[dict | None, list[str]]:
    """Load and structurally validate the manifest.

    Returns ``(manifest, errors)``; ``manifest`` is ``None`` when the file is
    unreadable or fatally malformed, so ``main`` can fail with a controlled
    message instead of a Python traceback.
    """
    errors: list[str] = []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as exc:
        return None, [f"FAIL manifest 无法解析: {exc}"]
    if not isinstance(data, dict):
        return None, ["FAIL manifest 顶层必须是 JSON 对象"]
    if not isinstance(data.get("contracts"), dict):
        return None, ["FAIL manifest 缺少 contracts 对象"]
    for key in ("versionless", "outbound"):
        entries = data.get(key, [])
        if not isinstance(entries, list) or not all(isinstance(entry, str) for entry in entries):
            errors.append(f"FAIL manifest 的 {key} 必须是合同名列表")
    for contract, groups in data["contracts"].items():
        if not isinstance(groups, dict):
            errors.append(f"FAIL manifest 合同 {contract} 的条目必须是对象")
            continue
        for outcome in ("valid", "invalid"):
            names = groups.get(outcome)
            if names is None:
                errors.append(f"FAIL manifest 合同 {contract} 缺少 {outcome} 列表")
            elif not isinstance(names, list) or not all(isinstance(name, str) for name in names):
                errors.append(f"FAIL manifest 合同 {contract} 的 {outcome} 必须是字符串列表")
    return data, errors


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
    versionless = set(manifest.get("versionless", []))
    outbound = set(manifest.get("outbound", []))

    for name, first, second in sorted(duplicates):
        errors.append(f"FAIL fixture {name} 在 manifest 中重复声明（首次 {first}，再次 {second}）")
    for name in sorted(on_disk - declared):
        errors.append(f"FAIL fixture {name} 未在 manifest 中声明")
    for name in sorted(declared - on_disk):
        errors.append(f"FAIL manifest 声明的 fixture 缺失: {name}")

    cpp_texts = [strip_comments(text) for text in cpp_test_sources]
    ts_text = strip_comments(ts_test_source)
    for contract, groups in manifest.get("contracts", {}).items():
        for outcome in ("valid", "invalid"):
            for name in groups.get(outcome, []):
                path = fixtures_dir / name
                if outcome == "valid" and contract not in versionless and path.is_file():
                    data = json.loads(path.read_text(encoding="utf-8"))
                    if data.get("schemaVersion") != expected_version:
                        errors.append(f"FAIL 有效 fixture {name} 的 schemaVersion 与双端常量不一致")
                if not any(is_referenced(text, name) for text in cpp_texts):
                    errors.append(f"FAIL fixture {name} 未接入 C++ 主机测试")
                if contract not in outbound and not is_referenced(ts_text, name):
                    errors.append(f"FAIL fixture {name} 未接入 TypeScript 测试")

    return errors


def main() -> int:
    cpp_version = extract_version(CPP_VERSION_HEADER.read_text(encoding="utf-8"), "kDeviceContractVersion")
    ts_version = extract_version(TS_VERSION_SOURCE.read_text(encoding="utf-8"), "DEVICE_CONTRACT_VERSION")
    if cpp_version != ts_version:
        sys.exit(f"FAIL 双端契约版本不一致: C++={cpp_version}, TypeScript={ts_version}")

    manifest, manifest_errors = load_manifest(MANIFEST_FILE)
    if manifest_errors:
        sys.exit("\n".join(manifest_errors))
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
