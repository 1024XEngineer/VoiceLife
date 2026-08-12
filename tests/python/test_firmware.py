from __future__ import annotations

import copy
import json
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import firmware  # noqa: E402


class ProfileValidationTest(unittest.TestCase):
    def setUp(self) -> None:
        profile_path = ROOT / "config" / "profiles" / "esp32s3-dev.json"
        self.profile = json.loads(profile_path.read_text(encoding="utf-8"))

    def test_rejects_non_string_capability_without_type_error(self) -> None:
        profile = copy.deepcopy(self.profile)
        profile["adapters"]["im"]["capabilities"] = [{}]

        with self.assertRaisesRegex(firmware.ProfileError, "capabilities 格式错误"):
            firmware.validate_profile(profile, Path("invalid.json"))

    def test_rejects_non_string_config_reference(self) -> None:
        profile = copy.deepcopy(self.profile)
        profile["adapters"]["im"]["configRef"] = 42

        with self.assertRaisesRegex(firmware.ProfileError, "configRef 只能引用"):
            firmware.validate_profile(profile, Path("invalid.json"))

    def test_rejects_non_string_sdkconfig_without_type_error(self) -> None:
        profile = copy.deepcopy(self.profile)
        profile["sdkconfig"] = [{}]

        with self.assertRaisesRegex(firmware.ProfileError, "sdkconfig 只能包含"):
            firmware.validate_profile(profile, Path("invalid.json"))

    def test_rejects_missing_board_identity(self) -> None:
        profile = copy.deepcopy(self.profile)
        del profile["boardId"]

        with self.assertRaisesRegex(firmware.ProfileError, "boardId 只能使用"):
            firmware.validate_profile(profile, Path("invalid.json"))

    def test_rejects_legacy_schema_version(self) -> None:
        profile = copy.deepcopy(self.profile)
        profile["schemaVersion"] = 1

        with self.assertRaisesRegex(firmware.ProfileError, "仅支持 schemaVersion=2"):
            firmware.validate_profile(profile, Path("invalid.json"))

    def test_rejects_adapter_capability_absent_from_platform(self) -> None:
        profile = copy.deepcopy(self.profile)
        profile["adapters"]["display"]["capabilities"] = ["image-presentation"]
        profile["resourceBudget"]["psramBytes"] = 8 * 1024 * 1024

        with self.assertRaisesRegex(firmware.ProfileError, "未列入平台 capabilities"):
            firmware.validate_profile(profile, Path("invalid.json"))

    def test_accepts_adapter_capabilities_declared_by_platform(self) -> None:
        profile = copy.deepcopy(self.profile)

        firmware.validate_profile(profile, Path("valid.json"))

    def test_rejects_image_display_without_psram_budget(self) -> None:
        profile = copy.deepcopy(self.profile)
        profile["capabilities"].append("image-presentation")
        profile["adapters"]["display"]["capabilities"] = ["image-presentation"]

        with self.assertRaisesRegex(firmware.ProfileError, "image-presentation 显示能力"):
            firmware.validate_profile(profile, Path("invalid.json"))

    @mock.patch("firmware.subprocess.run", side_effect=FileNotFoundError)
    def test_reports_missing_tool_without_traceback(self, _: mock.Mock) -> None:
        with self.assertRaisesRegex(firmware.ProfileError, "找不到命令 idf.py"):
            firmware.run(["idf.py", "build"])


if __name__ == "__main__":
    unittest.main()
