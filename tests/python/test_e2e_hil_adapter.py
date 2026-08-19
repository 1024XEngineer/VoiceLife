from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))

import e2e_hil_adapters as HIL  # noqa: E402
import e2e_runner as RUNNER  # noqa: E402


class FakeHardware:
    def __init__(
        self, *, readiness: list[dict[str, object]] | None = None, pairing: list[dict[str, str]] | None = None
    ):
        self.calls: list[str] = []
        self.readiness = readiness or [
            {"signal": "provisioned"},
            {"signal": "wifi_ready"},
            {"signal": "sntp_synced"},
            {"signal": "ready"},
        ]
        self.pairing = pairing or [
            {"device_id": "e2e-device", "user_id": "user-test"},
            {"code": "123456", "expires_at": "2026-08-03T00:01:00.000Z"},
            {"status": "pending"},
            {"status": "expired"},
        ]
        self.token = bytearray(b"A" * 43)
        self.device_revoked = False
        self.serial_open = False
        self.reset_count = 0

    def inspect(self, descriptor: object, temporary_directory: Path) -> object:
        self.calls.append("inspect")
        return [
            HIL.Partition("nvs", 1, 2, 0x9000, 0x4000, 0),
            HIL.Partition("otadata", 1, 0, 0xD000, 0x2000, 0),
            HIL.Partition("phy_init", 1, 1, 0xF000, 0x1000, 0),
            HIL.Partition("factory", 0, 0, 0x10000, 0x2D0000, 0),
            HIL.Partition("linx_secrets", 1, 2, 0x2E0000, 0x10000, 0),
            HIL.Partition("assets", 1, 0x82, 0x300000, 0x100000, 0),
            HIL.Partition("model", 1, 0x82, 0x400000, 0x300000, 0),
        ]

    def build(self, descriptor: object) -> Path:
        self.calls.append("build")
        return Path("/safe/build")

    def image(self, build_directory: Path, descriptor: object, partitions: object) -> object:
        self.calls.append("image")
        return HIL.ApplicationImage(Path("/safe/voicelife.bin"), 0x10000, 8, "a" * 64)

    def flash(self, descriptor: object, image: object, timeout_s: float) -> None:
        self.calls.append("flash")

    def register(self, run_id: str) -> object:
        self.calls.append("register")
        return HIL.TemporaryIdentity("e2e-device", "user-test", self.token, "b" * 40)

    def revoke(self, identity: object) -> None:
        self.calls.append("revoke")
        self.device_revoked = True
        for index in range(len(identity.token)):
            identity.token[index] = 0

    def revoke_device_id(self, device_id: str) -> None:
        self.calls.append("revoke-id")
        self.device_revoked = True

    def provision(self, descriptor: object, identity: object, timeout_s: float) -> None:
        self.calls.append("provision")
        self.serial_open = True

    def reboot_and_readiness(self, descriptor: object, timeout_s: float) -> list[dict[str, object]]:
        self.calls.append("readiness")
        return self.readiness

    def pair(self, descriptor: object, identity: object, timeout_s: float) -> list[dict[str, str]]:
        self.calls.append("pair")
        return self.pairing

    def recover(self, descriptor: object) -> None:
        self.calls.append("recover")
        self.serial_open = False
        self.reset_count += 1


class HilPairingAdapterTest(unittest.TestCase):
    def descriptor_file(self, directory: str) -> Path:
        path = Path(directory) / "device.json"
        path.write_text(
            json.dumps({"schema_version": 1, "name": "bench-a", "port": "/dev/cu.test-a", "profile": "sparkbot"}),
            encoding="utf-8",
        )
        return path

    def config(self) -> object:
        return RUNNER.RunnerConfig(
            layer="hil",
            journey="im-pairing",
            profile="sparkbot",
            hard_timeout_s=2.0,
            phase_timeout_s=1.0,
            cleanup_timeout_s=0.5,
        )

    def execute(self, hardware: FakeHardware) -> tuple[object, object]:
        with tempfile.TemporaryDirectory() as directory:
            adapter = HIL.HilPairingAdapter(
                self.descriptor_file(directory), Path(directory) / "leases", hardware=hardware
            )
            result = RUNNER.run_e2e(self.config(), adapter)
        return adapter, result

    def test_complete_hil_journey_requires_all_boundaries_and_cleans_up(self) -> None:
        hardware = FakeHardware()
        adapter, result = self.execute(hardware)
        self.assertEqual(result.exit_code, RUNNER.ExitCode.SUCCESS)
        self.assertEqual(
            hardware.calls,
            ["inspect", "build", "image", "flash", "register", "provision", "readiness", "pair", "recover", "revoke"],
        )
        self.assertTrue(hardware.device_revoked)
        self.assertFalse(hardware.serial_open)
        self.assertEqual(hardware.reset_count, 1)
        self.assertEqual(result.collected["scope"], "hil_im_pairing")
        self.assertTrue(result.collected["hardware_verified"])
        self.assertEqual(result.collected["readiness_markers"], ["provisioned", "wifi_ready", "sntp_synced", "ready"])
        self.assertEqual(result.collected["pairing_markers"], ["scope_matched", "code_valid", "pending", "expired"])
        self.assertNotIn("e2e-device", repr(result.collected))
        self.assertNotIn("123456", repr(result.collected))
        self.assertTrue(all(byte == 0 for byte in hardware.token))
        self.assertFalse(adapter.lease_held)

    def test_only_runtime_ready_is_a_product_failure_and_cleanup_still_runs(self) -> None:
        hardware = FakeHardware(readiness=[{"signal": "ready"}])
        adapter, result = self.execute(hardware)
        self.assertEqual(result.failure_category, RUNNER.FailureCategory.PRODUCT)
        self.assertEqual(result.message_code, "hil_readiness_incomplete")
        self.assertTrue(hardware.device_revoked)
        self.assertEqual(hardware.reset_count, 1)
        self.assertFalse(adapter.lease_held)
        self.assertNotIn("pair", hardware.calls)

    def test_wrong_pairing_terminal_is_product_failure_and_cleanup_still_runs(self) -> None:
        hardware = FakeHardware(
            pairing=[
                {"device_id": "e2e-device", "user_id": "user-test"},
                {"code": "123456", "expires_at": "2026-08-03T00:01:00.000Z"},
                {"status": "pending"},
                {"status": "confirmed"},
            ]
        )
        adapter, result = self.execute(hardware)
        self.assertEqual(result.failure_category, RUNNER.FailureCategory.PRODUCT)
        self.assertEqual(result.message_code, "pairing_lifecycle_invalid")
        self.assertTrue(hardware.device_revoked)
        self.assertFalse(adapter.lease_held)

    def test_profile_mismatch_prevents_flash(self) -> None:
        hardware = FakeHardware()
        hardware.inspect = mock.Mock(return_value=[])
        adapter, result = self.execute(hardware)
        self.assertEqual(result.failure_category, RUNNER.FailureCategory.DEVICE)
        self.assertEqual(result.message_code, "device_profile_mismatch")
        self.assertNotIn("flash", hardware.calls)
        self.assertFalse(adapter.lease_held)

    def test_registration_failure_revokes_run_scoped_device_and_releases_lease(self) -> None:
        hardware = FakeHardware()
        hardware.register = mock.Mock(
            side_effect=RUNNER.RunnerFailure(RUNNER.FailureCategory.INFRASTRUCTURE, "invalid")
        )
        adapter, result = self.execute(hardware)
        self.assertEqual(result.failure_category, RUNNER.FailureCategory.INFRASTRUCTURE)
        self.assertEqual(result.message_code, "invalid")
        self.assertIn("revoke-id", hardware.calls)
        self.assertFalse(adapter.lease_held)

    def test_cleanup_timeout_does_not_prevent_local_lease_release(self) -> None:
        hardware = FakeHardware()

        def timeout_revoke(identity: object) -> None:
            hardware.calls.append("revoke")
            raise RUNNER.RunnerDeadlineExceeded

        hardware.revoke = timeout_revoke
        adapter, result = self.execute(hardware)
        self.assertEqual(result.failure_category, RUNNER.FailureCategory.CLEANUP)
        self.assertEqual(result.message_code, "cleanup_timeout")
        self.assertFalse(adapter.lease_held)

    def test_device_inspection_and_build_deadlines_preserve_timeout_classification(self) -> None:
        for operation in ("inspect", "build"):
            with self.subTest(operation=operation):
                hardware = FakeHardware()
                setattr(hardware, operation, mock.Mock(side_effect=RUNNER.RunnerDeadlineExceeded))
                adapter, result = self.execute(hardware)
                self.assertEqual(result.failure_category, RUNNER.FailureCategory.TIMEOUT)
                self.assertEqual(result.message_code, f"{'prepare' if operation == 'inspect' else 'run'}_timeout")
                self.assertFalse(adapter.lease_held)

    def test_device_identifier_fingerprint_is_run_scoped_and_non_reversible(self) -> None:
        first = HIL.device_fingerprint("e2e-device", "a" * 32)
        second = HIL.device_fingerprint("e2e-device", "b" * 32)
        self.assertNotEqual(first, second)
        self.assertEqual(len(first), 16)
        self.assertNotIn("device", first)
        self.assertEqual(first, hashlib.sha256(("a" * 32 + ":e2e-device").encode()).hexdigest()[:16])

    def test_real_hardware_classifies_missing_command_as_infrastructure(self) -> None:
        hardware = HIL.RealHilHardware("runner@example.test", "/srv/voicelife", "https://gateway.example.test", "user-test")
        with mock.patch.object(HIL.subprocess, "run", side_effect=FileNotFoundError):
            with self.assertRaises(RUNNER.RunnerFailure) as raised:
                hardware._run(["missing-command"], 1.0)
        self.assertEqual(raised.exception.category, RUNNER.FailureCategory.INFRASTRUCTURE)
        self.assertEqual(raised.exception.message_code, "hil_command_unavailable")


if __name__ == "__main__":
    unittest.main()
