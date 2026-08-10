#!/usr/bin/env python3
"""Tests for the sanitized Linx voice evidence collector."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "collect_linx_e2e_evidence.py"
SPEC = importlib.util.spec_from_file_location("collect_linx_e2e_evidence", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class CollectLinxE2eEvidenceTest(unittest.TestCase):
    def test_parses_allowlisted_metrics_only(self) -> None:
        line = (
            "I (1) VoiceLifeRuntime: VOICE_EVENT session=local generation=7 event=tts_stopped detail_present=1 "
            "latency_from_capture_ms=321 audio_captured=12 audio_dropped=1 audio_played=8 "
            "audio_rejected=0 min_heap=100000 token=must-not-be-captured"
        )
        parsed = MODULE.parse_voice_event(line)
        self.assertEqual(
            parsed,
            {
                "event": "tts_stopped",
                "generation": 7,
                "detail_present": 1,
                "latency_ms": 321,
                "audio_captured": 12,
                "audio_dropped": 1,
                "audio_played": 8,
                "audio_rejected": 0,
                "min_heap": 100000,
            },
        )
        self.assertIsNone(MODULE.parse_voice_event("ssid=secret password=secret"))

    def test_writes_no_raw_serial_line_or_device_identity(self) -> None:
        event = MODULE.parse_voice_event(
            "VOICE_EVENT session=local generation=1 event=stt_text_received detail_present=1 "
            "latency_from_capture_ms=12 audio_captured=1 audio_dropped=0 audio_played=0 "
            "audio_rejected=0 min_heap=2"
        )
        assert event is not None
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "evidence.json"
            MODULE.write_evidence(path, "create-1", ["stt_text_received"], [event])
            content = path.read_text(encoding="utf-8")
            document = json.loads(content)
        self.assertTrue(document["all_expected_events_seen"])
        self.assertNotIn("session=", content)
        self.assertNotIn("token", content)

    def test_rejects_sensitive_or_ambiguous_labels(self) -> None:
        self.assertEqual(MODULE.validate_label("query_3"), "query_3")
        with self.assertRaises(ValueError):
            MODULE.validate_label("create meeting at 9")


if __name__ == "__main__":
    unittest.main()
