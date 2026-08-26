from __future__ import annotations

import copy
import json
import sys
import tempfile
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

    def test_partition_tables_are_kept_under_config(self) -> None:
        defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8").splitlines()
        profiles = [
            json.loads(path.read_text(encoding="utf-8"))["sdkconfig"] for path in firmware.PROFILES.glob("*.json")
        ]
        settings = defaults + [setting for profile in profiles for setting in profile]
        partition_tables = [
            setting.split("=", 1)[1].strip('"')
            for setting in settings
            if setting.startswith("CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=")
        ]

        self.assertTrue(partition_tables)
        for table in partition_tables:
            path = ROOT / table
            self.assertTrue(path.is_file(), f"分区表不存在: {table}")
            self.assertEqual(path.parent, ROOT / "config" / "partitions")

    @mock.patch("firmware.subprocess.run", side_effect=FileNotFoundError)
    def test_reports_missing_tool_without_traceback(self, _: mock.Mock) -> None:
        with self.assertRaisesRegex(firmware.ProfileError, "找不到命令 idf.py"):
            firmware.run(["idf.py", "build"])

    def test_prepares_generated_sqlite_component_when_missing(self) -> None:
        with tempfile.TemporaryDirectory() as directory, mock.patch("firmware.run") as run:
            firmware.ensure_sqlite_component(Path(directory))

        run.assert_called_once_with([sys.executable, str(ROOT / "scripts" / "prepare_sqlite.py")])

    def test_reuses_existing_generated_sqlite_component(self) -> None:
        with tempfile.TemporaryDirectory() as directory, mock.patch("firmware.run") as run:
            component = Path(directory)
            for name in ("sqlite3.c", "sqlite3.h", "CMakeLists.txt"):
                (component / name).touch()

            firmware.ensure_sqlite_component(component)

        run.assert_not_called()

    def test_sparkbot_profile_enables_gateway_im_without_selecting_pcb(self) -> None:
        profile_path = ROOT / "config" / "profiles" / "esp32s3-esp-sparkbot.json"
        profile = json.loads(profile_path.read_text(encoding="utf-8"))

        self.assertEqual(profile["adapters"]["im"]["driver"], "voicelife-gateway")
        self.assertEqual(profile["adapters"]["im"]["configRef"], "nvs://im")
        self.assertIn("CONFIG_COMPILER_OPTIMIZATION_SIZE=y", profile["sdkconfig"])
        self.assertIn("CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT=y", profile["sdkconfig"])
        self.assertIn("CONFIG_VOICELIFE_BOARD_ESP_SPARKBOT=y", profile["sdkconfig"])
        self.assertIn("CONFIG_VOICELIFE_IM_GATEWAY=y", profile["sdkconfig"])
        self.assertEqual(profile["adapters"]["storage"]["driver"], "fatfs-sqlite")
        self.assertIn("persistent-sqlite", profile["adapters"]["storage"]["capabilities"])
        self.assertIn("durable-calendar", profile["adapters"]["storage"]["capabilities"])
        self.assertIn("CONFIG_VOICELIFE_STORAGE_FATFS=y", profile["sdkconfig"])
        self.assertIn("CONFIG_VOICELIFE_STORAGE_SQLITE=y", profile["sdkconfig"])
        self.assertIn("CONFIG_VOICELIFE_STORAGE_FATFS_EXPECTED_PARTITION_ADDRESS=0x700000", profile["sdkconfig"])
        self.assertIn("CONFIG_VOICELIFE_STORAGE_FATFS_EXPECTED_PARTITION_SIZE=0x900000", profile["sdkconfig"])
        self.assertNotIn("CONFIG_VOICELIFE_BOARD_VOICELIFE_PCB=y", profile["sdkconfig"])

    def test_sparkbot_profile_enables_persistent_sqlite_storage(self) -> None:
        profile_path = ROOT / "config" / "profiles" / "esp32s3-esp-sparkbot.json"
        profile = json.loads(profile_path.read_text(encoding="utf-8"))

        self.assertEqual(profile["adapters"]["storage"]["driver"], "fatfs-sqlite")
        self.assertIn("persistent-sqlite", profile["adapters"]["storage"]["capabilities"])
        self.assertIn("CONFIG_VOICELIFE_STORAGE_FATFS=y", profile["sdkconfig"])
        self.assertIn("CONFIG_VOICELIFE_STORAGE_SQLITE=y", profile["sdkconfig"])

    def test_rejects_volatile_device_storage_profile(self) -> None:
        profile = copy.deepcopy(self.profile)
        profile["adapters"]["storage"] = {
            "driver": "memory",
            "capabilities": ["atomic-calendar-write"],
        }
        profile["sdkconfig"] = [
            setting for setting in profile["sdkconfig"] if not setting.startswith("CONFIG_VOICELIFE_STORAGE_")
        ]

        with self.assertRaisesRegex(firmware.ProfileError, "必须使用 persistent-sqlite"):
            firmware.validate_profile(profile, Path("volatile.json"))

    def test_rejects_persistent_profile_without_storage_flags(self) -> None:
        profile = copy.deepcopy(self.profile)
        profile["sdkconfig"] = [
            setting for setting in profile["sdkconfig"] if not setting.startswith("CONFIG_VOICELIFE_STORAGE_")
        ]

        with self.assertRaisesRegex(firmware.ProfileError, "持久化存储缺少"):
            firmware.validate_profile(profile, Path("missing-storage-flags.json"))

    def test_sparkbot_serial_voice_profile_uses_persistent_storage_and_im_gateway(self) -> None:
        profile_path = ROOT / "config" / "profiles" / "esp32s3-esp-sparkbot-serial-voice.json"
        profile = json.loads(profile_path.read_text(encoding="utf-8"))

        self.assertEqual(len(profile["sdkconfig"]), len(set(profile["sdkconfig"])))
        self.assertEqual(profile["adapters"]["im"]["driver"], "voicelife-gateway")
        self.assertEqual(profile["adapters"]["im"]["capabilities"], ["https", "secure-credentials"])
        self.assertEqual(profile["adapters"]["im"]["configRef"], "nvs://im")
        self.assertIn("CONFIG_VOICELIFE_IM_GATEWAY=y", profile["sdkconfig"])
        self.assertIn("CONFIG_LWIP_DHCP_GET_NTP_SRV=y", profile["sdkconfig"])
        self.assertIn("CONFIG_LWIP_SNTP_MAX_SERVERS=2", profile["sdkconfig"])
        self.assertIn("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y", profile["sdkconfig"])
        self.assertIn("CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y", profile["sdkconfig"])
        self.assertEqual(profile["adapters"]["storage"]["driver"], "fatfs-sqlite")
        self.assertIn("persistent-sqlite", profile["adapters"]["storage"]["capabilities"])
        self.assertIn("CONFIG_VOICELIFE_STORAGE_FATFS=y", profile["sdkconfig"])
        self.assertIn("CONFIG_VOICELIFE_STORAGE_SQLITE=y", profile["sdkconfig"])
        self.assertIn("CONFIG_VOICELIFE_STORAGE_FATFS_EXPECTED_PARTITION_ADDRESS=0x700000", profile["sdkconfig"])
        self.assertIn("CONFIG_VOICELIFE_STORAGE_FATFS_EXPECTED_PARTITION_SIZE=0x900000", profile["sdkconfig"])
        self.assertIn("CONFIG_VOICELIFE_MCP_TOOLS=y", profile["sdkconfig"])

    def test_sparkbot_profiles_keep_websocket_rx_tx_locks_separate(self) -> None:
        for profile_name in ("esp32s3-esp-sparkbot", "esp32s3-esp-sparkbot-serial-voice"):
            profile_path = ROOT / "config" / "profiles" / f"{profile_name}.json"
            profile = json.loads(profile_path.read_text(encoding="utf-8"))

            self.assertIn("CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y", profile["sdkconfig"])
            self.assertIn("CONFIG_ESP_WS_CLIENT_TX_LOCK_TIMEOUT_MS=2000", profile["sdkconfig"])

    def test_sparkbot_partition_reserves_persistent_data_after_model(self) -> None:
        lines = (ROOT / "config" / "partitions" / "sparkbot.csv").read_text(encoding="utf-8").splitlines()
        data_line = next(line for line in lines if line.strip().startswith("voicelife,"))
        self.assertIn(", fat,", data_line)
        self.assertIn("0x700000", data_line)
        self.assertIn("0x900000", data_line)
        self.assertLessEqual(0x400000 + 0x300000, 0x700000)
        self.assertLessEqual(0x700000 + 0x900000, 0x1000000)

    def test_production_fatfs_never_formats_on_mount_failure(self) -> None:
        source = (ROOT / "components" / "voicelife_storage_fatfs" / "src" / "fatfs_volume.cc").read_text(
            encoding="utf-8"
        )
        self.assertIn("mount_config.format_if_mount_failed = false;", source)
        self.assertNotIn("mount_config.format_if_mount_failed = true;", source)

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


if __name__ == "__main__":
    unittest.main()
