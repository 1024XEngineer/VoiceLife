from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import e2e_hil_device as HIL  # noqa: E402


class HilDeviceDescriptorTest(unittest.TestCase):
    def descriptor_file(self, directory: str, **overrides: object) -> Path:
        document: dict[str, object] = {
            "schema_version": 1,
            "name": "bench-a",
            "port": "/dev/cu.test-a",
            "profile": "sparkbot",
        }
        document.update(overrides)
        path = Path(directory) / "device.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        return path

    def test_descriptor_accepts_only_official_profile_and_exact_keys(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            descriptor = HIL.load_device_descriptor(self.descriptor_file(directory), "sparkbot")
            self.assertEqual(descriptor.firmware_profile, "esp32s3-esp-sparkbot")
            self.assertEqual(descriptor.application_label, "factory")

            with self.assertRaises(HIL.HilConfigurationError):
                HIL.load_device_descriptor(self.descriptor_file(directory, profile="pcb"), "sparkbot")
            with self.assertRaises(HIL.HilConfigurationError):
                HIL.load_device_descriptor(self.descriptor_file(directory, token="canary"), "sparkbot")
            with self.assertRaises(HIL.HilConfigurationError):
                HIL.load_device_descriptor(self.descriptor_file(directory, port="relative-port"), "sparkbot")

    def test_same_device_or_serial_cannot_be_leased_twice(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            descriptor = HIL.load_device_descriptor(self.descriptor_file(directory), "sparkbot")
            lease_root = Path(directory) / "leases"
            first = HIL.DeviceLease(descriptor, lease_root)
            second = HIL.DeviceLease(descriptor, lease_root)
            first.acquire()
            try:
                with self.assertRaises(HIL.HilLeaseUnavailable):
                    second.acquire()
            finally:
                first.release()
                first.release()
            second.acquire()
            second.release()


class HilProfileGuardTest(unittest.TestCase):
    def partition(self, label: str, type_value: int, subtype: int, offset: int, size: int) -> object:
        return HIL.Partition(label=label, type=type_value, subtype=subtype, offset=offset, size=size, flags=0)

    def sparkbot_layout(self) -> list[object]:
        return [
            self.partition("nvs", 1, 2, 0x9000, 0x4000),
            self.partition("otadata", 1, 0, 0xD000, 0x2000),
            self.partition("phy_init", 1, 1, 0xF000, 0x1000),
            self.partition("factory", 0, 0, 0x10000, 0x2D0000),
            self.partition("linx_secrets", 1, 2, 0x2E0000, 0x10000),
            self.partition("assets", 1, 0x82, 0x300000, 0x100000),
            self.partition("model", 1, 0x82, 0x400000, 0x300000),
        ]

    def pcb_layout(self) -> list[object]:
        return [
            self.partition("nvs", 1, 2, 0x9000, 0x6000),
            self.partition("otadata", 1, 0, 0xF000, 0x2000),
            self.partition("phy_init", 1, 1, 0x11000, 0x1000),
            self.partition("ota_0", 0, 0x10, 0x20000, 0x3E0000),
            self.partition("ota_1", 0, 0x11, 0x400000, 0x3E0000),
            self.partition("voicelife", 1, 0x82, 0x7E0000, 0x200000),
            self.partition("linx_secrets", 1, 2, 0xA00000, 0x10000),
            self.partition("model", 1, 0x82, 0xA10000, 0x300000),
        ]

    def descriptor(self, profile: str) -> object:
        official = HIL.official_profile(profile)
        return HIL.DeviceDescriptor(
            name="bench-a",
            port=Path("/dev/cu.test-a"),
            profile=profile,
            firmware_profile=official.firmware_profile,
            application_label=official.application_label,
        )

    def test_profile_guard_rejects_cross_board_layout_before_flash(self) -> None:
        HIL.validate_device_layout(self.descriptor("sparkbot"), self.sparkbot_layout())
        HIL.validate_device_layout(self.descriptor("pcb"), self.pcb_layout())
        with self.assertRaises(HIL.HilProfileMismatch):
            HIL.validate_device_layout(self.descriptor("sparkbot"), self.pcb_layout())
        with self.assertRaises(HIL.HilProfileMismatch):
            HIL.validate_device_layout(self.descriptor("pcb"), self.sparkbot_layout())

    def test_application_image_must_match_profile_offset_and_partition_size(self) -> None:
        descriptor = self.descriptor("sparkbot")
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "voicelife.bin").write_bytes(b"firmware")
            (build / "flasher_args.json").write_text(
                json.dumps({"flash_files": {"0x10000": "voicelife.bin"}}), encoding="utf-8"
            )
            image = HIL.load_application_image(build, descriptor, self.sparkbot_layout())
            self.assertEqual(image.offset, 0x10000)
            self.assertEqual(len(image.sha256), 64)

            (build / "flasher_args.json").write_text(
                json.dumps({"flash_files": {"0x20000": "voicelife.bin"}}), encoding="utf-8"
            )
            with self.assertRaises(HIL.HilProfileMismatch):
                HIL.load_application_image(build, descriptor, self.sparkbot_layout())

    def test_flash_plan_writes_and_verifies_only_the_validated_application(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            image_path = Path(directory) / "voicelife.bin"
            image_path.write_bytes(b"firmware")
            image = HIL.ApplicationImage(path=image_path, offset=0x10000, size=8, sha256="a" * 64)
            operations = HIL.application_flash_operations(Path("/dev/cu.test-a"), image)
        self.assertEqual([operation.kind for operation in operations], ["write", "verify"])
        self.assertTrue(all(operation.offset == 0x10000 for operation in operations))
        rendered = " ".join(argument for operation in operations for argument in operation.argv)
        self.assertIn("write-flash", rendered)
        self.assertIn("verify-flash", rendered)
        self.assertNotIn("write_flash", rendered)
        self.assertNotIn("verify_flash", rendered)
        for forbidden in ("0x8000", "0xd000", "partition-table", "bootloader", "otadata", "nvs"):
            self.assertNotIn(forbidden, rendered.lower())


if __name__ == "__main__":
    unittest.main()
