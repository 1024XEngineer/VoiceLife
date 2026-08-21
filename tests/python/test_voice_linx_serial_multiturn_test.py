from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "voice_linx_serial_multiturn_test.py"


def load() -> object:
    spec = importlib.util.spec_from_file_location("voice_linx_serial_multiturn_test", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


MODULE = load()


class DisplayEvidenceTest(unittest.TestCase):
    def test_sparkbot_requires_complete_text_trace_and_observes_scroll(self) -> None:
        line = (
            "SPARKBOT_TEXT_RENDER generation=1 revision=2 content_height=16 viewport_height=120 "
            "overflow_width=4 manual_line_breaks=0 status=说话 content=你好 content_visible=1"
        )
        self.assertEqual(MODULE.display_evidence("sparkbot", [line], [], 0), (True, 1, True))

    def test_pcb_requires_a_draw_for_every_turn_after_the_turn_starts(self) -> None:
        results = [
            MODULE.TurnResult(index=1, input_text="", log_start=1, log_end=3),
            MODULE.TurnResult(index=2, input_text="", log_start=3, log_end=5),
        ]
        lines = ["DISPLAY_DRAW=1 boot", "state", "DISPLAY_DRAW=1 first", "state", "DISPLAY_DRAW=1 second"]
        self.assertEqual(MODULE.display_evidence("pcb", lines, results, 2), (True, 2, False))
        self.assertEqual(MODULE.display_evidence("pcb", lines, results, 3), (False, 2, False))


if __name__ == "__main__":
    unittest.main()
