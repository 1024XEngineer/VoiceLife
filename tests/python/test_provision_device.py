from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import provision_device  # noqa: E402


class BoardConfigurationTest(unittest.TestCase):
    def test_supports_only_sparkbot_and_pcb(self) -> None:
        self.assertEqual(set(provision_device.BOARD_CONFIGS), {"sparkbot", "pcb"})

    def test_sparkbot_profile_and_flash_offset(self) -> None:
        config = provision_device.BOARD_CONFIGS["sparkbot"]
        self.assertEqual(config["profile"], "esp32s3-esp-sparkbot")
        self.assertEqual(config["default_flash_offset"], "0x10000")

    def test_pcb_profile_and_flash_offset(self) -> None:
        config = provision_device.BOARD_CONFIGS["pcb"]
        self.assertEqual(config["profile"], "esp32s3-voicelife-pcb-pcm")
        self.assertEqual(config["default_flash_offset"], "0x20000")

    def test_app_flash_offset_reads_flasher_args_when_available(self) -> None:
        build_dir = Path("/tmp/provision-device-test-build")
        build_dir.mkdir(exist_ok=True)
        try:
            (build_dir / "flasher_args.json").write_text(
                json.dumps(
                    {
                        "flash_settings": {},
                        "flash_files": {
                            "0x0": "bootloader/bootloader.bin",
                            "0x10000": "voicelife.bin",
                        },
                    }
                )
            )
            self.assertEqual(provision_device.app_flash_offset(build_dir, "sparkbot"), "0x10000")
        finally:
            (build_dir / "flasher_args.json").unlink(missing_ok=True)
            build_dir.rmdir()

    def test_app_flash_offset_falls_back_when_flasher_args_missing(self) -> None:
        build_dir = Path("/tmp/provision-device-test-build-missing")
        self.assertEqual(provision_device.app_flash_offset(build_dir, "pcb"), "0x20000")


class GatewayOriginTest(unittest.TestCase):
    def test_extracts_origin_from_action_ui_base_url(self) -> None:
        self.assertEqual(
            provision_device.gateway_origin_from_base_url("https://voicelife.xengineer.cn/voicelife/reminder-actions"),
            "https://voicelife.xengineer.cn",
        )

    def test_rejects_non_https_origin(self) -> None:
        with self.assertRaises(ValueError):
            provision_device.gateway_origin_from_base_url("http://voicelife.xengineer.cn")


class CredentialValidationTest(unittest.TestCase):
    def test_accepts_registered_device_credential(self) -> None:
        credential = {
            "deviceId": "9d5c7d9e-7f2a-4b3c-8d1e-2f6a9c0b4e5a",
            "userId": "user-test",
            "deviceToken": "A" * 43,
            "status": "active",
        }
        self.assertEqual(
            provision_device.validate_credential(credential, "user-test")["deviceId"], credential["deviceId"]
        )

    def test_rejects_non_uuid_device_id(self) -> None:
        credential = {
            "deviceId": "wechat-test",
            "userId": "user-test",
            "deviceToken": "A" * 43,
            "status": "active",
        }
        with self.assertRaises(ValueError):
            provision_device.validate_credential(credential, "user-test")

    def test_rejects_token_not_exactly_43_base64url(self) -> None:
        credential = {
            "deviceId": "9d5c7d9e-7f2a-4b3c-8d1e-2f6a9c0b4e5a",
            "userId": "user-test",
            "deviceToken": "B" * 64,
            "status": "active",
        }
        with self.assertRaises(ValueError):
            provision_device.validate_credential(credential, "user-test")

    def test_rejects_status_not_active(self) -> None:
        credential = {
            "deviceId": "9d5c7d9e-7f2a-4b3c-8d1e-2f6a9c0b4e5a",
            "userId": "user-test",
            "deviceToken": "A" * 43,
            "status": "revoked",
        }
        with self.assertRaises(ValueError):
            provision_device.validate_credential(credential, "user-test")

    def test_rejects_user_id_mismatch(self) -> None:
        credential = {
            "deviceId": "9d5c7d9e-7f2a-4b3c-8d1e-2f6a9c0b4e5a",
            "userId": "user-other",
            "deviceToken": "A" * 43,
            "status": "active",
        }
        with self.assertRaises(ValueError):
            provision_device.validate_credential(credential, "user-test")


class CommandConstructionTest(unittest.TestCase):
    def test_build_command_uses_profile(self) -> None:
        command = provision_device.build_command("sparkbot")
        self.assertIn("esp32s3-esp-sparkbot", command)
        self.assertIn("scripts/firmware.py", command)

    def test_flash_command_contains_app_binary_offset_and_verify(self) -> None:
        build_dir = Path("/tmp/provision-device-test-build-flash")
        build_dir.mkdir(exist_ok=True)
        try:
            (build_dir / "flasher_args.json").write_text(
                json.dumps({"flash_settings": {}, "flash_files": {"0x20000": "voicelife.bin"}})
            )
            (build_dir / "voicelife.bin").write_bytes(b"firmware")
            command = provision_device.flash_command("pcb", "/dev/cu.usbmodem14401", build_dir)
            self.assertIn("--port", command)
            self.assertIn("/dev/cu.usbmodem14401", command)
            self.assertIn("0x20000", command)
            self.assertIn("voicelife.bin", command)
            self.assertIn("write-flash", command)
        finally:
            (build_dir / "flasher_args.json").unlink(missing_ok=True)
            (build_dir / "voicelife.bin").unlink(missing_ok=True)
            build_dir.rmdir()

    def test_server_register_script_creates_device_with_user(self) -> None:
        script = provision_device.server_register_script("/root/XE6-15", "user-test")
        self.assertIn("cd /root/XE6-15", script)
        self.assertIn("pnpm --silent device -- create --user-id", script)
        self.assertIn("user-test", script)


if __name__ == "__main__":
    unittest.main()
