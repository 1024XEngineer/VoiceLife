#!/usr/bin/env python3
"""Tests for the sanitized Linx voice evidence collector."""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "collect_linx_e2e_evidence.py"
SPEC = importlib.util.spec_from_file_location("collect_linx_e2e_evidence", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def fake_serial_module(device: object) -> types.ModuleType:
    """A serial module whose Serial(...) is a context manager yielding `device`."""

    class _SerialContext:
        def __init__(self, *args: object, **kwargs: object) -> None:
            pass

        def __enter__(self) -> object:
            return device

        def __exit__(self, *args: object) -> bool:
            return False

    return types.SimpleNamespace(Serial=_SerialContext)


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

    def test_requires_ordered_lifecycle_and_rejects_failures(self) -> None:
        lifecycle = (
            "wake_detected",
            "capture_started",
            "stt_text_received",
            "tool_call_received",
            "tts_started",
            "tts_stopped",
            "standby_ready",
        )
        events = [{"event": name} for name in lifecycle]
        valid, error = MODULE.validate_event_sequence(events, [event["event"] for event in events])
        self.assertTrue(valid, error)

        out_of_order = [{"event": name} for name in ("capture_started", "wake_detected")]
        valid, error = MODULE.validate_event_sequence(out_of_order, ["wake_detected", "capture_started"])
        self.assertFalse(valid)
        self.assertIn("capture_started", error)

        failed = [{"event": "wake_detected"}, {"event": "provider_error"}]
        valid, error = MODULE.validate_event_sequence(failed, ["wake_detected"])
        self.assertFalse(valid)
        self.assertIn("provider_error", error)

    def test_parses_im_readiness_signal_without_token(self) -> None:
        ready = MODULE.parse_im_signal(
            "I (10737) VoiceLifeRuntime: IM_RUNTIME_READY=1 device_token=replace-with-a-distinct-test-only-token"
        )
        self.assertEqual(ready, {"signal": "ready"})
        degraded = MODULE.parse_im_signal("W (12000) VoiceLifeRuntime: IM_RUNTIME_DEGRADED=1 state=2 code=5")
        self.assertEqual(degraded, {"signal": "degraded", "state": 2, "code": 5})
        synced = MODULE.parse_im_signal("I (9000) VoiceLifeRuntime: SNTP_SYNCED=1")
        self.assertEqual(synced, {"signal": "sntp_synced"})
        provisioned = MODULE.parse_im_signal("W (5000) VoiceLifeRuntime: IM_PROVISION_READY=1 timeout_ms=60000")
        self.assertEqual(provisioned, {"signal": "provisioning"})
        failed = MODULE.parse_im_signal("I (9999) VoiceLifeRuntime: IM_PROVISIONED=1")
        self.assertEqual(failed, {"signal": "provisioned"})
        self.assertIsNone(MODULE.parse_im_signal("unrelated line with token=secret"))
        self.assertIsNone(MODULE.parse_im_signal(""))

    def test_im_readiness_status_requires_ready_and_rejects_failures(self) -> None:
        self.assertTrue(MODULE.im_readiness_status([{"signal": "ready"}])[0])
        self.assertTrue(
            MODULE.im_readiness_status([{"signal": "sntp_synced"}, {"signal": "ready"}, {"signal": "provisioned"}])[0]
        )
        ok, error = MODULE.im_readiness_status([{"signal": "sntp_synced"}])
        self.assertFalse(ok)
        self.assertIn("ready", error)
        ok, error = MODULE.im_readiness_status([{"signal": "provisioning"}, {"signal": "ready"}])
        self.assertTrue(ok, error)
        ok, error = MODULE.im_readiness_status([{"signal": "ready"}, {"signal": "failure"}])
        self.assertFalse(ok)
        self.assertIn("failure", error)

    def test_im_readiness_status_accepts_recovery_and_uses_latest_terminal_state(self) -> None:
        ok, error = MODULE.im_readiness_status([{"signal": "degraded"}, {"signal": "ready"}])
        self.assertTrue(ok, error)
        ok, error = MODULE.im_readiness_status([{"signal": "ready"}, {"signal": "degraded"}])
        self.assertFalse(ok)
        self.assertIn("degraded", error)

    def test_write_evidence_records_im_signals_without_raw_line(self) -> None:
        voice = MODULE.parse_voice_event(
            "VOICE_EVENT session=local generation=1 event=stt_text_received detail_present=1 "
            "latency_from_capture_ms=12 audio_captured=1 audio_dropped=0 audio_played=0 "
            "audio_rejected=0 min_heap=2"
        )
        assert voice is not None
        ready = MODULE.parse_im_signal("I (1) VoiceLifeRuntime: IM_RUNTIME_READY=1")
        assert ready is not None
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "evidence.json"
            MODULE.write_evidence(path, "im-ready-1", ["stt_text_received"], [voice], [ready])
            content = path.read_text(encoding="utf-8")
            document = json.loads(content)
        self.assertEqual(document["schema_version"], 2)
        self.assertEqual(document["im_signals"], [{"signal": "ready"}])
        self.assertTrue(document["im_ready_seen"])
        self.assertFalse(document["im_failure_observed"])
        self.assertNotIn("IM_RUNTIME_READY=1", content)

    def _readline_repeating(self, lines: list[bytes]) -> object:
        iterator = iter(lines)

        def readline(*args: object, **kwargs: object) -> bytes:
            try:
                return next(iterator)
            except StopIteration:
                return b""

        return readline

    def test_require_im_ready_gates_main_success(self) -> None:
        fake_device = mock.MagicMock()
        fake_device.readline.side_effect = self._readline_repeating(
            [
                b"I (1) VoiceLifeRuntime: VOICE_EVENT session=local generation=1 event=wake_detected "
                b"detail_present=0 latency_from_capture_ms=0 audio_captured=0 audio_dropped=0 "
                b"audio_played=0 audio_rejected=0 min_heap=10\n",
                b"I (2) VoiceLifeRuntime: IM_RUNTIME_READY=1\n",
            ]
        )
        with (
            mock.patch.dict(sys.modules, {"serial": fake_serial_module(fake_device)}),
            tempfile.TemporaryDirectory() as directory,
        ):
            evidence_path = Path(directory) / "evidence.json"
            status = MODULE.main(
                [
                    "--port",
                    "FAKE",
                    "--evidence",
                    str(evidence_path),
                    "--label",
                    "im-ready-1",
                    "--require-im-ready",
                    "--timeout",
                    "0.1",
                ]
            )
            self.assertEqual(status, 0)
            document = json.loads(evidence_path.read_text(encoding="utf-8"))
        self.assertTrue(document["im_ready_seen"])

    def test_require_im_ready_fails_when_not_seen(self) -> None:
        fake_device = mock.MagicMock()
        fake_device.readline.side_effect = self._readline_repeating(
            [b"I (1) VoiceLifeRuntime: IM_RUNTIME_DEGRADED=1 state=2 code=5\n"]
        )
        with (
            mock.patch.dict(sys.modules, {"serial": fake_serial_module(fake_device)}),
            tempfile.TemporaryDirectory() as directory,
        ):
            evidence_path = Path(directory) / "evidence.json"
            status = MODULE.main(
                [
                    "--port",
                    "FAKE",
                    "--evidence",
                    str(evidence_path),
                    "--label",
                    "im-degraded-1",
                    "--require-im-ready",
                    "--timeout",
                    "0.1",
                ]
            )
            self.assertNotEqual(status, 0)
            document = json.loads(evidence_path.read_text(encoding="utf-8"))
        self.assertFalse(document["im_ready_seen"])
        self.assertTrue(document["im_failure_observed"])


if __name__ == "__main__":
    unittest.main()
