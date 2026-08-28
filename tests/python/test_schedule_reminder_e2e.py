from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("schedule_reminder_e2e", ROOT / "scripts" / "schedule_reminder_e2e.py")
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class ScheduleReminderJourneyTest(unittest.TestCase):
    def test_host_matrix_has_three_attempt_terminal_and_one_winner(self) -> None:
        result = MODULE.run_host_matrix()
        self.assertEqual(result["status"], "passed")
        self.assertTrue(result["terminal"])
        self.assertEqual(result["delivery_count"], 3)
        self.assertTrue(result["single_consume"])
        self.assertTrue(result["concurrent_winner"])
        self.assertEqual(result["winner_count"], 1)
        self.assertTrue(result["snooze"])
        self.assertTrue(result["no_attempt_four"])

    def test_attempt_four_cannot_be_created_after_exhaustion(self) -> None:
        chain = MODULE.ReminderChain()
        for _ in range(4):
            chain.trigger()
            chain.clock.advance(60)
        self.assertEqual(chain.task.status, "exhausted")
        self.assertEqual(len(chain.deliveries), 3)


if __name__ == "__main__":
    unittest.main()
