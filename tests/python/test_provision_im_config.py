import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("provision_im_config", ROOT / "scripts" / "provision_im_config.py")
assert SPEC and SPEC.loader
PROVISION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROVISION)
TOKEN = "A" * 43


class ProvisionImConfigTest(unittest.TestCase):
    def test_extracts_sanitized_failure_code(self):
        self.assertEqual(PROVISION.provisioning_failure_code(b"IM_PROVISION_FAILED code=2\r\n"), 2)
        self.assertIsNone(PROVISION.provisioning_failure_code(b"unrelated log line\r\n"))

    def test_payload_matches_vli1_wire_format(self):
        payload = PROVISION.request_payload(
            "https://gateway.example", "device-test", TOKEN, "user-test"
        )

        self.assertIsInstance(payload, bytearray)
        self.assertEqual(payload[:4], b"VLI1")
        lengths = [int.from_bytes(payload[offset : offset + 2], "big") for offset in range(4, 12, 2)]
        self.assertEqual(lengths, [23, 11, 43, 9])
        self.assertEqual(len(payload), 12 + sum(lengths))

    def test_force_payload_uses_explicit_vli2_magic(self):
        payload = PROVISION.request_payload(
            "https://gateway.example", "device-test", TOKEN, "user-test", allow_overwrite=True
        )

        self.assertEqual(payload[:4], b"VLI2")

    def test_rejects_unsafe_origins_and_field_lengths(self):
        for origin in (
            "http://gateway.example",
            "https://gateway.example/path",
            "https://user@gateway.example",
            "https://gateway.example?query=1",
            "https://gateway.example#fragment",
            "https://gateway_example",
            "https://gateway.example:abc",
            "https://gateway.example:70000",
        ):
            with self.subTest(origin=origin), self.assertRaises(ValueError):
                PROVISION.request_payload(origin, "device-test", TOKEN, "user-test")

        with self.assertRaises(ValueError):
            PROVISION.request_payload("https://gateway.example", "", TOKEN, "user-test")
        with self.assertRaises(ValueError):
            PROVISION.request_payload("https://gateway.example", "device-test", "x" * 513, "user-test")
        with self.assertRaises(ValueError):
            PROVISION.request_payload("https://gateway.example", "device-test", TOKEN, "")
        for device_id, token in (
            ("device test", "credential"),
            ("device-test", "token with spaces"),
            ("设备", "token"),
        ):
            with self.subTest(device_id=device_id, token=token), self.assertRaises(ValueError):
                PROVISION.request_payload("https://gateway.example", device_id, token, "user-test")


if __name__ == "__main__":
    unittest.main()
