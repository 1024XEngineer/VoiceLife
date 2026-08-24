from __future__ import annotations

import json
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

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


class EnvironmentConfigTest(unittest.TestCase):
    def test_load_dotenv_parses_key_values(self) -> None:
        env_file = Path("/tmp/provision-device-test.env")
        env_file.write_text(
            "# comment\n"
            "PROVISION_SERVER=root@example.com\n"
            "PROVISION_SERVER_DIR=/srv/voicelife\n"
            "\n"
            "PROVISION_GATEWAY_ORIGIN=https://gw.example.com\n"
        )
        try:
            values = provision_device.load_dotenv(env_file)
            self.assertEqual(values["PROVISION_SERVER"], "root@example.com")
            self.assertEqual(values["PROVISION_SERVER_DIR"], "/srv/voicelife")
            self.assertEqual(values["PROVISION_GATEWAY_ORIGIN"], "https://gw.example.com")
            self.assertNotIn("comment", values)
        finally:
            env_file.unlink(missing_ok=True)

    def test_load_dotenv_missing_file_returns_empty(self) -> None:
        self.assertEqual(provision_device.load_dotenv(Path("/tmp/provision-device-missing.env")), {})

    def test_resolve_server_config_from_dotenv(self) -> None:
        env_file = Path("/tmp/provision-device-server.env")
        env_file.write_text("PROVISION_SERVER=root@example.com\nPROVISION_SERVER_DIR=/srv/voicelife\n")
        try:
            server, server_dir = provision_device.resolve_server_config(None, None, {}, env_file)
            self.assertEqual(server, "root@example.com")
            self.assertEqual(server_dir, "/srv/voicelife")
        finally:
            env_file.unlink(missing_ok=True)

    def test_resolve_server_config_requires_server(self) -> None:
        env_file = Path("/tmp/provision-device-no-server.env")
        env_file.write_text("PROVISION_SERVER_DIR=/srv/voicelife\n")
        try:
            with self.assertRaises(SystemExit):
                provision_device.resolve_server_config(None, None, {}, env_file)
        finally:
            env_file.unlink(missing_ok=True)

    def test_resolve_server_config_prefers_arguments_over_environment(self) -> None:
        env_file = Path("/tmp/provision-device-priority.env")
        env_file.write_text("PROVISION_SERVER=root@dotenv.example\nPROVISION_SERVER_DIR=/srv/dotenv\n")
        try:
            server, server_dir = provision_device.resolve_server_config(
                "root@arg.example", "/srv/arg", {"PROVISION_SERVER": "root@env.example"}, env_file
            )
            self.assertEqual(server, "root@arg.example")
            self.assertEqual(server_dir, "/srv/arg")
        finally:
            env_file.unlink(missing_ok=True)

    def test_resolve_gateway_origin_prefers_explicit_then_provision_then_base_url(self) -> None:
        env_file = Path("/tmp/provision-device-origin.env")
        env_file.write_text("WECHAT_ACTION_UI_BASE_URL=https://base.example/voicelife/reminder-actions\n")
        try:
            self.assertEqual(
                provision_device.resolve_gateway_origin("https://arg.example", {}, env_file),
                "https://arg.example",
            )
            self.assertEqual(
                provision_device.resolve_gateway_origin(
                    None, {"PROVISION_GATEWAY_ORIGIN": "https://env.example"}, env_file
                ),
                "https://env.example",
            )
            self.assertEqual(
                provision_device.resolve_gateway_origin(None, {}, env_file),
                "https://base.example",
            )
        finally:
            env_file.unlink(missing_ok=True)

    def test_resolve_gateway_origin_requires_source(self) -> None:
        env_file = Path("/tmp/provision-device-no-origin.env")
        env_file.write_text("")
        try:
            with self.assertRaises(SystemExit):
                provision_device.resolve_gateway_origin(None, {}, env_file)
        finally:
            env_file.unlink(missing_ok=True)


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
        self.assertIn("cd '/root/XE6-15'", script)
        self.assertIn("pnpm --silent device -- create --user-id", script)
        self.assertIn("user-test", script)

    def test_server_register_script_safely_inherits_active_user(self) -> None:
        script = provision_device.server_register_script("/root/XE6-15", None)
        self.assertIn(r"status=\$\$active\$\$", script)
        self.assertNotIn("status='active'", script)
        self.assertIn('if [ -z "$user_id" ]', script)

    def test_hil_registration_uses_explicit_run_device_and_records_gateway_commit(self) -> None:
        script = provision_device.server_register_script(
            "/root/XE6-15", "user-test", device_id="e2e-" + "a" * 32, include_commit=True
        )
        self.assertIn("--device-id", script)
        self.assertIn("e2e-" + "a" * 32, script)
        self.assertIn("git rev-parse HEAD", script)
        self.assertIn("gatewayCommit", script)
        with self.assertRaises(ValueError):
            provision_device.server_register_script("/safe; touch /tmp/pwn", "user-test")

    def test_revoke_script_is_idempotent_and_does_not_contain_token(self) -> None:
        script = provision_device.server_revoke_script("/root/XE6-15", "e2e-" + "a" * 32)
        self.assertIn("device -- revoke --device-id", script)
        self.assertIn("e2e-" + "a" * 32, script)
        self.assertNotIn("token", script.lower())

    def test_run_remote_reports_safe_error_without_stderr(self) -> None:
        completed = subprocess.CompletedProcess(
            args=[], returncode=9, stdout="", stderr="Authorization: Bearer canary /private/path"
        )
        with (
            mock.patch.object(provision_device.subprocess, "run", return_value=completed),
            self.assertRaises(provision_device.RemoteOperationError) as raised,
        ):
            provision_device.run_remote("host", "safe script")
        self.assertEqual(raised.exception.code, "remote_operation_failed")
        self.assertNotIn("canary", repr(raised.exception))
        self.assertNotIn("private", repr(raised.exception))

    def test_hil_run_scoped_device_id_can_be_validated_without_uuid(self) -> None:
        credential = {
            "deviceId": "e2e-" + "a" * 32,
            "userId": "user-test",
            "deviceToken": "A" * 43,
            "status": "active",
        }
        self.assertEqual(
            provision_device.validate_credential(credential, "user-test", require_uuid=False)["deviceId"],
            credential["deviceId"],
        )


if __name__ == "__main__":
    unittest.main()
