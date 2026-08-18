import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("start_im_pairing", ROOT / "scripts" / "start_im_pairing.py")
assert SPEC and SPEC.loader
PAIRING = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PAIRING)


class StartImPairingTest(unittest.TestCase):
    def test_payload_is_fixed_size_and_contains_no_credentials(self):
        self.assertEqual(PAIRING.trigger_payload(5), b"VLP1\x05" + b"\x00" * 7)
        self.assertEqual(len(PAIRING.trigger_payload(10)), 12)

    def test_expiry_is_bounded(self):
        for invalid in (0, 11, -1):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                PAIRING.trigger_payload(invalid)

    def test_extracts_only_sanitized_board_results(self):
        self.assertEqual(
            PAIRING.parse_pairing_line(
                b"I VoiceLifeIm: IM_PAIRING_CODE=123456 expires_at=2026-08-03T00:05:00.000Z\r\n"
            ),
            {"code": "123456", "expires_at": "2026-08-03T00:05:00.000Z"},
        )
        self.assertEqual(
            PAIRING.parse_pairing_line(b"I VoiceLifeIm: IM_PAIRING_STATUS=confirmed\r\n"),
            {"status": "confirmed"},
        )
        self.assertIsNone(PAIRING.parse_pairing_line(b"Authorization: Bearer secret\r\n"))


if __name__ == "__main__":
    unittest.main()
