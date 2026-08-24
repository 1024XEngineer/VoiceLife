from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import voice_linx_serial_multiturn_test as harness  # noqa: E402


class FakeDevice:
    def __init__(self) -> None:
        self.writes: list[bytes] = []

    def write(self, payload: bytes) -> None:
        self.writes.append(payload)

    def flush(self) -> None:
        pass


class ReorderedReadyLog:
    def mark(self) -> int:
        return 0

    def wait_for(self, marker: str, after: int, timeout: float) -> tuple[int, str]:
        del timeout
        positions = {
            "SERIAL_VOICE_CAPTURE_READY": 1,
            "SERIAL_VOICE_TURN_BEGIN=ok": 2,
        }
        position = positions[marker]
        if position < after:
            raise TimeoutError(marker)
        return position + 1, marker

    def wait_for_any(self, markers: tuple[str, ...], after: int, timeout: float) -> tuple[int, str]:
        del markers, after, timeout
        raise TimeoutError("endpoint")

    def contains_since(self, marker: str, after: int) -> bool:
        del marker, after
        return False


class RunTurnTest(unittest.TestCase):
    def test_first_turn_accepts_capture_ready_before_begin_ack(self) -> None:
        device = FakeDevice()
        prepared = harness.PreparedTurn(input_text="测试", tts_ms=0, frames=[bytes(640), bytes(640)])

        with mock.patch.object(harness, "serial", mock.Mock(SerialException=OSError)):
            result = harness.run_turn(
                device,
                ReorderedReadyLog(),
                index=1,
                prepared=prepared,
                response_timeout=1,
                first_turn=True,
                expect_terminal=False,
                guard_observation_seconds=0,
            )

        self.assertEqual(result.pcm_frames_sent, 2)
        self.assertEqual(result.error, "endpoint")


if __name__ == "__main__":
    unittest.main()
