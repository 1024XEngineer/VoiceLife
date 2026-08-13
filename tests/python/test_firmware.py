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

    @mock.patch("firmware.subprocess.run", side_effect=FileNotFoundError)
    def test_reports_missing_tool_without_traceback(self, _: mock.Mock) -> None:
        with self.assertRaisesRegex(firmware.ProfileError, "找不到命令 idf.py"):
            firmware.run(["idf.py", "build"])

    def test_im_pcb_profile_accepts_input_from_its_usb_provisioning_port(self) -> None:
        profile_path = ROOT / "config" / "profiles" / "esp32s3-voicelife-pcb-pcm.json"
        profile = json.loads(profile_path.read_text(encoding="utf-8"))

        self.assertIn("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y", profile["sdkconfig"])
        self.assertIn("CONFIG_ESP_CONSOLE_SECONDARY_NONE=y", profile["sdkconfig"])

    def test_im_pcb_profile_enables_dhcp_ntp_for_trusted_time(self) -> None:
        profile_path = ROOT / "config" / "profiles" / "esp32s3-voicelife-pcb-pcm.json"
        profile = json.loads(profile_path.read_text(encoding="utf-8"))

        self.assertIn("CONFIG_LWIP_DHCP_GET_NTP_SRV=y", profile["sdkconfig"])
        self.assertIn("CONFIG_LWIP_SNTP_MAX_SERVERS=2", profile["sdkconfig"])

    def test_im_pcb_profile_verifies_cross_signed_cloudflare_chain(self) -> None:
        profile_path = ROOT / "config" / "profiles" / "esp32s3-voicelife-pcb-pcm.json"
        profile = json.loads(profile_path.read_text(encoding="utf-8"))

        self.assertIn("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y", profile["sdkconfig"])

    def test_sparkbot_profile_declares_real_schedule_storage(self) -> None:
        profile_path = ROOT / "config" / "profiles" / "esp32s3-esp-sparkbot.json"
        profile = json.loads(profile_path.read_text(encoding="utf-8"))

        self.assertEqual(profile["adapters"]["storage"]["driver"], "fatfs-sqlite")
        self.assertIn("persistent-sqlite", profile["adapters"]["storage"]["capabilities"])
        self.assertIn("once-only-reminder", profile["adapters"]["storage"]["capabilities"])
        self.assertIn("CONFIG_VOICELIFE_STORAGE_FATFS=y", profile["sdkconfig"])
        self.assertIn("CONFIG_VOICELIFE_STORAGE_SQLITE=y", profile["sdkconfig"])
        self.assertIn("CONFIG_VOICELIFE_STORAGE_FATFS_EXPECTED_PARTITION_ADDRESS=0x700000", profile["sdkconfig"])


if __name__ == "__main__":
    unittest.main()
