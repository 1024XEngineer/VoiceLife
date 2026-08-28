from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("schedule_voice_e2e", ROOT / "scripts" / "schedule_voice_e2e.py")
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class ScheduleVoiceJourneyTest(unittest.TestCase):
    def test_host_matrix_keeps_public_mutations_idempotent_and_sanitized(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            result = MODULE.run_host_matrix(Path(directory), run_id="a" * 32)

        self.assertTrue(result.passed)
        self.assertEqual(result.product_gap, "schedule_undo_not_public")
        self.assertEqual(result.mutation_counts["create"], 1)
        self.assertEqual(result.mutation_counts["delete"], 1)
        self.assertEqual(result.final_active_keys, [])
        self.assertEqual(result.cleanup, "passed")

    def test_voice_correction_writes_only_the_final_time(self) -> None:
        gateway = MODULE.FakeScheduleGateway(MODULE.Fixtures.empty())

        MODULE.VoiceTranscript(gateway).run(
            "voice-correction",
            ["明天上午九点安排跑步", "不是上午，是晚上九点"],
        )

        record = gateway.active_records()[0]
        self.assertEqual(record.start, "2026-09-02 21:00:00")
        self.assertEqual(len(gateway.mutations), 1)

    def test_ambiguous_delete_does_not_mutate_and_undo_is_explicit_gap(self) -> None:
        gateway = MODULE.FakeScheduleGateway(MODULE.Fixtures.ambiguous())
        outcome = MODULE.VoiceTranscript(gateway).run("ambiguity", ["取消那个会"])

        self.assertEqual(outcome.status, "clarification_required")
        self.assertEqual(gateway.mutations, [])
        self.assertEqual(MODULE.undo_result(), {"status": "product_gap", "tool": "schedule.undo"})

    def test_evidence_rejects_raw_transcript_and_accepts_summary(self) -> None:
        safe = MODULE.journey_evidence(
            run_id="a" * 32,
            profile="host",
            case_id="create-query",
            status="passed",
            summary={"mcp_calls": 2, "schedule_keys": ["S-VOICE-001"]},
        )
        MODULE.validate_journey_evidence(safe)
        unsafe = dict(safe)
        unsafe["transcript"] = "明天上午十点安排产品评审"
        with self.assertRaises(MODULE.EvidenceValidationError):
            MODULE.validate_journey_evidence(unsafe)

    def test_cli_scenario_list_is_stable(self) -> None:
        self.assertEqual(
            MODULE.SCENARIOS,
            (
                "create-query",
                "query-date-keyword-empty",
                "update-query",
                "delete-query-idempotency",
                "missing-field-and-cancel",
                "ambiguity-and-conflict",
                "voice-correction",
                "recurrence-exception",
            ),
        )


if __name__ == "__main__":
    unittest.main()
