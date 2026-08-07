from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import check_contract_dual_end as gate  # noqa: E402

VERSION = "1"

# C++ 主机测试源码：两个文件都以文本形式参与引用检查。
CPP_TEST_SOURCES = [
    """
    const std::string strong = ReadFixture("notification-strong.json");
    const std::string weak = ReadFixture("notification-weak.json");
    RequireRejected("notification-invalid-enum.json", "非法枚举必须被 C++ 拒绝");
    RequireRejected("notification-invalid-time.json", "非法时间必须被 C++ 拒绝");
    """,
    """
    const std::string conflict = ReadFixture("notification-conflict.json");
    """,
]

TS_TEST_SOURCE = """
    const strong = await readFixture("notification-strong.json");
    const weak = await readFixture("notification-weak.json");
    const conflict = await readFixture("notification-conflict.json");
    for (const name of [
      "notification-invalid-enum.json",
      "notification-invalid-time.json",
    ]) {
      await expectGatewayError(...);
    }
"""


def make_manifest() -> dict:
    return {
        "schemaVersion": VERSION,
        "contracts": {
            "notification": {
                "valid": ["notification-strong.json", "notification-weak.json"],
                "invalid": ["notification-invalid-enum.json", "notification-invalid-time.json"],
            },
            "conflict-scenario": {
                "valid": ["notification-conflict.json"],
                "invalid": [],
            },
        },
    }


def make_fixture_tree() -> tuple[str, dict]:
    """Build a temp tree with a fixtures dir matching make_manifest()."""
    directory = tempfile.mkdtemp(prefix="dual-end-")
    fixtures_dir = Path(directory) / "fixtures"
    fixtures_dir.mkdir()
    for _contract, groups in make_manifest()["contracts"].items():
        for outcome in ("valid", "invalid"):
            for name in groups[outcome]:
                body = {"schemaVersion": VERSION if outcome == "valid" else "999"}
                (fixtures_dir / name).write_text(json.dumps(body), encoding="utf-8")
    return directory, make_manifest()


class ContractDualEndCheckTest(unittest.TestCase):
    def check(
        self,
        fixtures_dir: Path,
        manifest: dict,
        cpp_sources: list[str] | None = None,
        ts_source: str | None = None,
        expected_version: str = VERSION,
    ) -> list[str]:
        return gate.check_fixtures(
            fixtures_dir,
            manifest,
            CPP_TEST_SOURCES if cpp_sources is None else cpp_sources,
            TS_TEST_SOURCE if ts_source is None else ts_source,
            expected_version,
        )

    def test_passes_when_manifest_complete_and_covered(self) -> None:
        directory, manifest = make_fixture_tree()
        try:
            self.assertEqual(self.check(Path(directory) / "fixtures", manifest), [])
        finally:
            import shutil

            shutil.rmtree(directory)

    def test_detects_fixture_not_declared_in_manifest(self) -> None:
        directory, manifest = make_fixture_tree()
        try:
            orphan = Path(directory) / "fixtures" / "notification-strong-extra.json"
            orphan.write_text(json.dumps({"schemaVersion": VERSION}), encoding="utf-8")
            errors = self.check(Path(directory) / "fixtures", manifest)
            self.assertTrue(any("notification-strong-extra.json 未在 manifest 中声明" in error for error in errors))
        finally:
            import shutil

            shutil.rmtree(directory)

    def test_detects_manifest_referencing_missing_fixture(self) -> None:
        directory, manifest = make_fixture_tree()
        try:
            manifest["contracts"]["notification"]["invalid"].append("notification-invalid-missing.json")
            errors = self.check(Path(directory) / "fixtures", manifest)
            self.assertTrue(
                any("manifest 声明的 fixture 缺失: notification-invalid-missing.json" in error for error in errors)
            )
        finally:
            import shutil

            shutil.rmtree(directory)

    def test_detects_duplicate_manifest_entries(self) -> None:
        directory, manifest = make_fixture_tree()
        try:
            manifest["contracts"]["duplicate-scenario"] = {
                "valid": ["notification-strong.json"],
                "invalid": [],
            }
            errors = self.check(Path(directory) / "fixtures", manifest)
            self.assertTrue(any("notification-strong.json 在 manifest 中重复声明" in error for error in errors))
            self.assertTrue(
                any(
                    "notification-strong.json 在 manifest 中重复声明" in error
                    and "notification/valid" in error
                    and "duplicate-scenario/valid" in error
                    for error in errors
                )
            )
        finally:
            import shutil

            shutil.rmtree(directory)

    def test_detects_duplicate_within_same_contract(self) -> None:
        directory, manifest = make_fixture_tree()
        try:
            manifest["contracts"]["notification"]["invalid"].append("notification-strong.json")
            errors = self.check(Path(directory) / "fixtures", manifest)
            self.assertTrue(
                any(
                    "notification-strong.json 在 manifest 中重复声明" in error
                    and "notification/valid" in error
                    and "notification/invalid" in error
                    for error in errors
                )
            )
        finally:
            import shutil

            shutil.rmtree(directory)

    def test_detects_valid_fixture_with_wrong_version(self) -> None:
        directory, manifest = make_fixture_tree()
        try:
            target = Path(directory) / "fixtures" / "notification-strong.json"
            target.write_text(json.dumps({"schemaVersion": "2"}), encoding="utf-8")
            errors = self.check(Path(directory) / "fixtures", manifest)
            self.assertTrue(
                any("notification-strong.json 的 schemaVersion 与双端常量不一致" in error for error in errors)
            )
        finally:
            import shutil

            shutil.rmtree(directory)

    def test_detects_invalid_fixture_not_wired_into_cpp_tests(self) -> None:
        directory, manifest = make_fixture_tree()
        try:
            cpp = [text.replace("notification-invalid-time.json", "ignored.json") for text in CPP_TEST_SOURCES]
            errors = self.check(Path(directory) / "fixtures", manifest, cpp_sources=cpp)
            self.assertTrue(any("notification-invalid-time.json 未接入 C++ 主机测试" in error for error in errors))
        finally:
            import shutil

            shutil.rmtree(directory)

    def test_detects_valid_fixture_not_wired_into_ts_tests(self) -> None:
        directory, manifest = make_fixture_tree()
        try:
            ts = TS_TEST_SOURCE.replace("notification-weak.json", "ignored.json")
            errors = self.check(Path(directory) / "fixtures", manifest, ts_source=ts)
            self.assertTrue(any("notification-weak.json 未接入 TypeScript 测试" in error for error in errors))
        finally:
            import shutil

            shutil.rmtree(directory)

    def test_comment_mention_does_not_count_as_coverage(self) -> None:
        directory, manifest = make_fixture_tree()
        try:
            cpp = ["// 注释里提到 notification-strong.json，但不是真实引用\n"]
            ts = "// 同上 notification-strong.json\n"
            errors = self.check(Path(directory) / "fixtures", manifest, cpp_sources=cpp, ts_source=ts)
            self.assertTrue(any("notification-strong.json 未接入 C++ 主机测试" in error for error in errors))
            self.assertTrue(any("notification-strong.json 未接入 TypeScript 测试" in error for error in errors))
        finally:
            import shutil

            shutil.rmtree(directory)

    def test_quoted_comment_does_not_count_as_coverage(self) -> None:
        directory, manifest = make_fixture_tree()
        try:
            cpp = ['// 参见 "notification-strong.json" 但这不是真实引用\n']
            ts = '// 同上 "notification-strong.json"\n'
            errors = self.check(Path(directory) / "fixtures", manifest, cpp_sources=cpp, ts_source=ts)
            self.assertTrue(any("notification-strong.json 未接入 C++ 主机测试" in error for error in errors))
            self.assertTrue(any("notification-strong.json 未接入 TypeScript 测试" in error for error in errors))
        finally:
            import shutil

            shutil.rmtree(directory)

    def test_extract_version_parses_marker(self) -> None:
        text = 'inline constexpr const char* kDeviceContractVersion = "7";'
        self.assertEqual(gate.extract_version(text, "kDeviceContractVersion"), "7")

    def test_load_manifest_rejects_malformed_json(self) -> None:
        directory, _manifest = make_fixture_tree()
        try:
            bad = Path(directory) / "manifest.json"
            bad.write_text("{ not json", encoding="utf-8")
            manifest, errors = gate.load_manifest(bad)
            self.assertIsNone(manifest)
            self.assertTrue(any("manifest 无法解析" in error for error in errors))
        finally:
            import shutil

            shutil.rmtree(directory)

    def test_load_manifest_rejects_missing_contracts(self) -> None:
        directory, _manifest = make_fixture_tree()
        try:
            bad = Path(directory) / "manifest.json"
            bad.write_text(json.dumps({"schemaVersion": VERSION}), encoding="utf-8")
            manifest, errors = gate.load_manifest(bad)
            self.assertIsNone(manifest)
            self.assertTrue(any("缺少 contracts" in error for error in errors))
        finally:
            import shutil

            shutil.rmtree(directory)

    def test_versionless_valid_fixture_skips_version_check(self) -> None:
        directory, manifest = make_fixture_tree()
        try:
            manifest["versionless"] = ["notification"]
            target = Path(directory) / "fixtures" / "notification-strong.json"
            target.write_text(json.dumps({"businessEventId": "event"}), encoding="utf-8")
            errors = self.check(Path(directory) / "fixtures", manifest)
            self.assertFalse(any("schemaVersion 与双端常量不一致" in error for error in errors))
        finally:
            import shutil

            shutil.rmtree(directory)

    def test_outbound_contract_requires_only_cpp_coverage(self) -> None:
        directory, manifest = make_fixture_tree()
        try:
            manifest["outbound"] = ["notification"]
            ts = TS_TEST_SOURCE.replace("notification-weak.json", "ignored.json")
            errors = self.check(Path(directory) / "fixtures", manifest, ts_source=ts)
            self.assertFalse(any("notification-weak.json 未接入 TypeScript 测试" in error for error in errors))
        finally:
            import shutil

            shutil.rmtree(directory)


if __name__ == "__main__":
    unittest.main()
