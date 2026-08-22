from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "scripts"


def load(name: str, path: Path) -> object:
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


EVIDENCE = load("e2e_evidence", SCRIPTS / "e2e_evidence.py")
CHECK = load("check_e2e_artifacts", SCRIPTS / "check_e2e_artifacts.py")
SUMMARY = load("render_e2e_summary", SCRIPTS / "render_e2e_summary.py")


class E2EArtifactToolsTest(unittest.TestCase):
    def evidence(self) -> dict[str, object]:
        return {
            "schema_version": 1,
            "run_id": "a" * 32,
            "correlation_id": "b" * 32,
            "scope": "runner_contract_only",
            "layer": "host",
            "journey": "lifecycle-example",
            "profile": "host",
            "started_at": "2026-08-21T00:00:00.000Z",
            "finished_at": "2026-08-21T00:00:01.000Z",
            "duration_ms": 1000,
            "status": "passed",
            "failure_category": None,
            "failed_phase": None,
            "message_code": "run_passed",
            "stages": [
                {"name": phase, "status": "passed", "code": "phase_passed"}
                for phase in ("prepare", "run", "assert", "collect", "cleanup")
            ],
            "assertions": [],
            "metrics": {"resource_count": 1},
            "cleanup": {"status": "passed", "error_codes": []},
            "hardware_verified": False,
            "hil": None,
        }

    def test_validates_evidence_and_renders_only_public_fields(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "evidence-a.json").write_text(json.dumps(self.evidence()), encoding="utf-8")
            self.assertEqual(CHECK.validate_directory(root), (1, 1))
            summary = SUMMARY.render(root)
            self.assertIn("lifecycle-example", summary)
            self.assertNotIn("run_id", summary)

    def test_rejects_raw_logs_and_sensitive_json(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "serial.log").write_text("raw", encoding="utf-8")
            with self.assertRaises(ValueError):
                CHECK.validate_directory(root)
            (root / "serial.log").unlink()
            (root / "other.json").write_text(json.dumps({"token": "canary"}), encoding="utf-8")
            with self.assertRaises(ValueError):
                CHECK.validate_directory(root)


if __name__ == "__main__":
    unittest.main()
