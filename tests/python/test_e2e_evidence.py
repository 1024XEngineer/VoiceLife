from __future__ import annotations

import importlib.util
import json
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "scripts" / "e2e_evidence.py"
SPEC = importlib.util.spec_from_file_location("e2e_evidence", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
EVIDENCE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = EVIDENCE
SPEC.loader.exec_module(EVIDENCE)


class E2eEvidenceTest(unittest.TestCase):
    def document(self) -> dict[str, object]:
        return {
            "schema_version": 1,
            "run_id": "a" * 32,
            "correlation_id": "b" * 32,
            "scope": "runner_contract_only",
            "layer": "host",
            "journey": "lifecycle-example",
            "profile": "host",
            "started_at": "2026-08-18T00:00:00.000Z",
            "finished_at": "2026-08-18T00:00:01.000Z",
            "duration_ms": 1000,
            "status": "passed",
            "failure_category": None,
            "failed_phase": None,
            "message_code": "run_passed",
            "stages": [
                {"name": "prepare", "status": "passed", "code": "phase_passed"},
                {"name": "run", "status": "passed", "code": "phase_passed"},
                {"name": "assert", "status": "passed", "code": "phase_passed"},
                {"name": "collect", "status": "passed", "code": "phase_passed"},
                {"name": "cleanup", "status": "passed", "code": "phase_passed"},
            ],
            "assertions": [{"name": "lifecycle_complete", "passed": True, "code": "ok"}],
            "metrics": {"resource_count": 2},
            "cleanup": {"status": "passed", "error_codes": []},
            "hardware_verified": False,
            "hil": None,
        }

    def test_valid_document_passes_and_canonicalizes(self) -> None:
        document = self.document()
        EVIDENCE.validate_evidence(document)
        payload = EVIDENCE.canonical_json(document)
        self.assertEqual(json.loads(payload), document)
        self.assertTrue(payload.endswith("\n"))
        self.assertEqual(payload, EVIDENCE.canonical_json(document))

    def test_rejects_missing_unknown_or_invalid_top_level_values(self) -> None:
        cases = []
        missing = self.document()
        missing.pop("run_id")
        cases.append(missing)
        unknown = self.document()
        unknown["raw_log"] = "serial content"
        cases.append(unknown)
        bad_status = self.document()
        bad_status["status"] = "maybe"
        cases.append(bad_status)
        bad_run_id = self.document()
        bad_run_id["run_id"] = "not-opaque"
        cases.append(bad_run_id)
        bad_duration = self.document()
        bad_duration["duration_ms"] = -1
        cases.append(bad_duration)
        too_many_stages = self.document()
        too_many_stages["stages"] = too_many_stages["stages"] * 20
        cases.append(too_many_stages)

        for document in cases:
            with self.subTest(document=document), self.assertRaises(EVIDENCE.EvidenceValidationError):
                EVIDENCE.validate_evidence(document)

    def test_rejects_unknown_nested_keys_and_invalid_relations(self) -> None:
        unknown_assertion = self.document()
        unknown_assertion["assertions"][0]["detail"] = "free text"
        passed_with_failure = self.document()
        passed_with_failure["failure_category"] = "timeout"
        failed_without_phase = self.document()
        failed_without_phase["status"] = "failed"
        failed_without_phase["failure_category"] = "product"
        failed_without_phase["message_code"] = "journey_assertion_failed"

        for document in (unknown_assertion, passed_with_failure, failed_without_phase):
            with self.subTest(document=document), self.assertRaises(EVIDENCE.EvidenceValidationError):
                EVIDENCE.validate_evidence(document)

    def test_rejects_incomplete_out_of_order_or_contradictory_stages(self) -> None:
        incomplete = self.document()
        incomplete["stages"] = incomplete["stages"][:1]
        out_of_order = self.document()
        out_of_order["stages"][0], out_of_order["stages"][1] = out_of_order["stages"][1], out_of_order["stages"][0]
        contradictory = self.document()
        contradictory["stages"][1] = {"name": "run", "status": "failed", "code": "run_timeout"}
        failed_without_matching_stage = self.document()
        failed_without_matching_stage["status"] = "failed"
        failed_without_matching_stage["failure_category"] = "timeout"
        failed_without_matching_stage["failed_phase"] = "run"
        failed_without_matching_stage["message_code"] = "run_timeout"

        for document in (incomplete, out_of_order, contradictory, failed_without_matching_stage):
            with self.subTest(document=document), self.assertRaises(EVIDENCE.EvidenceValidationError):
                EVIDENCE.validate_evidence(document)

    def test_accepts_consistent_timeout_evidence(self) -> None:
        document = self.document()
        document["status"] = "failed"
        document["failure_category"] = "timeout"
        document["failed_phase"] = "run"
        document["message_code"] = "run_timeout"
        document["stages"] = [
            {"name": "prepare", "status": "passed", "code": "phase_passed"},
            {"name": "run", "status": "failed", "code": "run_timeout"},
            {"name": "assert", "status": "skipped", "code": "phase_skipped"},
            {"name": "collect", "status": "skipped", "code": "phase_skipped"},
            {"name": "cleanup", "status": "passed", "code": "phase_passed"},
        ]
        document["assertions"] = []
        EVIDENCE.validate_evidence(document)

    def test_sensitive_key_and_value_scan_fails_closed(self) -> None:
        sensitive_values = (
            {"token": "canary"},
            {"safe": "Authorization: Bearer canary-value"},
            {"safe": "password=canary-value"},
            {"safe": "device_id=device-1"},
            {"safe": "ssid=private-network"},
            {"safe": "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJ1c2VyIn0.signature"},
        )
        for value in sensitive_values:
            with self.subTest(value=value):
                findings = EVIDENCE.scan_sensitive(value)
                self.assertTrue(findings)
                self.assertNotIn("canary-value", repr(findings))

    def test_allowed_identifiers_do_not_trigger_sensitive_scan(self) -> None:
        document = self.document()
        self.assertEqual(EVIDENCE.scan_sensitive(document), [])

    def hil_document(self) -> dict[str, object]:
        document = self.document()
        document.update(
            {
                "scope": "hil_im_pairing",
                "layer": "hil",
                "journey": "im-pairing",
                "profile": "sparkbot",
                "hardware_verified": True,
                "assertions": [
                    {"name": "readiness_complete", "passed": True, "code": "ok"},
                    {"name": "pairing_complete", "passed": True, "code": "ok"},
                ],
                "hil": {
                    "firmware_sha256": "a" * 64,
                    "gateway_commit": "b" * 40,
                    "device_fingerprint": "c" * 16,
                    "readiness_markers": ["provisioned", "wifi_ready", "sntp_synced", "ready"],
                    "pairing_markers": ["scope_matched", "code_valid", "pending", "expired"],
                },
            }
        )
        return document

    def test_accepts_strict_hil_pairing_evidence(self) -> None:
        document = self.hil_document()
        EVIDENCE.validate_evidence(document)
        self.assertEqual(json.loads(EVIDENCE.canonical_json(document)), document)

    def test_hil_evidence_rejects_missing_gates_false_success_claim_and_sensitive_values(self) -> None:
        missing = self.hil_document()
        missing["hil"]["readiness_markers"] = ["ready"]
        false_success_claim = self.hil_document()
        false_success_claim["hardware_verified"] = False
        display_code = self.hil_document()
        display_code["hil"]["pairing_markers"] = ["scope_matched", "123456", "pending", "expired"]
        raw_identity = self.hil_document()
        raw_identity["hil"]["device_id"] = "device-1"

        for document in (missing, false_success_claim, display_code, raw_identity):
            with self.subTest(document=document), self.assertRaises(EVIDENCE.EvidenceValidationError):
                EVIDENCE.validate_evidence(document)

    def test_contract_only_evidence_cannot_claim_hardware_or_carry_hil_metadata(self) -> None:
        claimed = self.document()
        claimed["hardware_verified"] = True
        metadata = self.document()
        metadata["hil"] = self.hil_document()["hil"]
        for document in (claimed, metadata):
            with self.assertRaises(EVIDENCE.EvidenceValidationError):
                EVIDENCE.validate_evidence(document)

    def test_atomic_write_creates_private_file_without_partial_temp_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            destination = root / "evidence.json"
            EVIDENCE.write_evidence(root, destination, self.document())
            self.assertEqual(json.loads(destination.read_text(encoding="utf-8")), self.document())
            self.assertEqual(stat.S_IMODE(destination.stat().st_mode), 0o600)
            self.assertEqual(list(root.glob(".evidence-*.tmp")), [])

    def test_atomic_write_rejects_symlink_and_path_escape(self) -> None:
        with tempfile.TemporaryDirectory() as directory, tempfile.TemporaryDirectory() as outside:
            root = Path(directory)
            target = Path(outside) / "outside.json"
            symlink = root / "evidence.json"
            symlink.symlink_to(target)
            with self.assertRaises(EVIDENCE.EvidenceWriteError):
                EVIDENCE.write_evidence(root, symlink, self.document())
            with self.assertRaises(EVIDENCE.EvidenceWriteError):
                EVIDENCE.write_evidence(root, target, self.document())
            self.assertFalse(target.exists())

    def test_validation_failure_does_not_create_destination(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            destination = root / "evidence.json"
            document = self.document()
            document["authorization"] = "Bearer canary"
            with self.assertRaises(EVIDENCE.EvidenceValidationError):
                EVIDENCE.write_evidence(root, destination, document)
            self.assertFalse(destination.exists())

    def test_replace_failure_removes_private_temporary_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            destination = root / "evidence.json"
            with (
                mock.patch.object(os, "replace", side_effect=OSError("private path detail")),
                self.assertRaises(EVIDENCE.EvidenceWriteError) as raised,
            ):
                EVIDENCE.write_evidence(root, destination, self.document())
            self.assertEqual(raised.exception.code, "evidence_atomic_replace_failed")
            self.assertFalse(destination.exists())
            self.assertEqual(list(root.glob(".evidence-*.tmp")), [])
            self.assertNotIn("private", repr(raised.exception))


if __name__ == "__main__":
    unittest.main()
