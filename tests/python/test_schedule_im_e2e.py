from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("schedule_im_e2e", ROOT / "scripts" / "schedule_im_e2e.py")
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class ScheduleImJourneyTest(unittest.TestCase):
    def test_host_matrix_covers_binding_scope_and_retry(self) -> None:
        result = MODULE.run_host_matrix()
        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["mixed_count"], 3)
        self.assertEqual(result["duplicate_delivery"], "already_delivered")
        self.assertEqual(result["retry"], ("retryable_failed", "submitted"))
        self.assertEqual(result["scope_rejected"], "scope_rejected")

    def test_wrong_scope_does_not_create_message(self) -> None:
        gateway = MODULE.FakeImGateway()
        gateway.bind("D-SPARK-01", "U-001")
        result = MODULE.QueryJourney(gateway).query("Q-SCOPE", "D-SPARK-01", "U-002")
        self.assertEqual(result["delivery"], "scope_rejected")
        self.assertEqual(gateway.messages, {})


if __name__ == "__main__":
    unittest.main()
