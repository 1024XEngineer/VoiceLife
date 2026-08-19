from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import voice_bailian_load_test as load_test  # noqa: E402


class MultiTurnSummaryTest(unittest.TestCase):
    def test_counts_only_complete_serial_conversations(self) -> None:
        results = [
            load_test.Result(conversation_index=0, turn_index=0, end_to_end_ms=100.0),
            load_test.Result(conversation_index=0, turn_index=1, end_to_end_ms=120.0),
            load_test.Result(conversation_index=0, turn_index=2, end_to_end_ms=140.0),
            load_test.Result(conversation_index=1, turn_index=0, end_to_end_ms=90.0),
            load_test.Result(conversation_index=1, turn_index=1, error="stt:timeout"),
            load_test.Result(conversation_index=1, turn_index=2, end_to_end_ms=110.0),
        ]

        report = load_test.summarize(
            results,
            model="tts-test",
            asr_model="stt-test",
            concurrency=2,
            conversations=2,
            turns_per_conversation=3,
        )

        self.assertEqual(report["requested"], 6)
        self.assertEqual(report["successful"], 5)
        self.assertEqual(
            report["conversations"],
            {
                "requested": 2,
                "completed": 1,
                "failed": 1,
                "turns_per_conversation": 3,
            },
        )
        conversation_metric = next(
            metric for metric in report["latency"] if metric["metric"] == "conversation_end_to_end"
        )
        self.assertEqual(conversation_metric["count"], 1)
        self.assertEqual(conversation_metric["p50_ms"], 360.0)

    def test_fidelity_gate_rejects_successful_but_mismatched_transcripts(self) -> None:
        report = {"failed": 0, "successful": 5, "transcript_matches": 4}
        self.assertFalse(load_test.passes_acceptance(report, mode="tts-to-stt", allow_transcript_mismatch=False))
        self.assertTrue(load_test.passes_acceptance(report, mode="tts-to-stt", allow_transcript_mismatch=True))
        self.assertTrue(load_test.passes_acceptance(report, mode="tts", allow_transcript_mismatch=False))


if __name__ == "__main__":
    unittest.main()
