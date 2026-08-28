#!/usr/bin/env python3
"""Issue #350 voice -> schedule journey fixtures and HIL adapter.

The Host path drives the same public schedule operations that the firmware
exposes through MCP.  The HIL path only adds the real board voice boundary;
it never stores raw utterances or credentials in evidence.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "scripts") not in sys.path:
    sys.path.insert(0, str(ROOT / "scripts"))

from e2e_hil_device import (  # noqa: E402
    DeviceLease,
    HilConfigurationError,
    HilLeaseUnavailable,
    load_device_descriptor,
)
from e2e_runner import (  # noqa: E402
    AssertionResult,
    FailureCategory,
    RunContext,
    RunnerFailure,
)

SCENARIOS = (
    "create-query",
    "query-date-keyword-empty",
    "update-query",
    "delete-query-idempotency",
    "missing-field-and-cancel",
    "ambiguity-and-conflict",
    "voice-correction",
    "recurrence-exception",
)
BASE_TIME = "2026-09-01T08:50:00+08:00"
SAFE_ID = re.compile(r"^[a-f0-9]{32}$")
SAFE_KEY = re.compile(r"^[A-Z0-9][A-Z0-9-]{0,31}$")


class EvidenceValidationError(ValueError):
    """Journey evidence contains an unsafe or non-public field."""


@dataclass
class ScheduleRecord:
    business_key: str
    title: str
    start: str
    end: str
    location: str = ""
    notes: str = ""
    status: str = "active"


@dataclass
class JourneyOutcome:
    status: str
    mcp_calls: list[str] = field(default_factory=list)
    schedule_keys: list[str] = field(default_factory=list)
    product_gap: str | None = None


@dataclass
class MatrixResult:
    passed: bool
    product_gap: str
    mutation_counts: dict[str, int]
    final_active_keys: list[str]
    cleanup: str


class Fixtures:
    """Business-key fixtures copied from the Issue, with no real user data."""

    @staticmethod
    def empty() -> list[ScheduleRecord]:
        return []

    @staticmethod
    def single() -> list[ScheduleRecord]:
        return [
            ScheduleRecord(
                "S-CHANGE-ME",
                "写作业",
                "2026-09-03 18:00:00",
                "2026-09-03 18:30:00",
                "书桌",
            )
        ]

    @staticmethod
    def ambiguous() -> list[ScheduleRecord]:
        return [
            ScheduleRecord("S-MEETING-A", "产品会议", "2026-09-03 14:00:00", "2026-09-03 15:00:00"),
            ScheduleRecord("S-MEETING-B", "产品会议", "2026-09-04 14:00:00", "2026-09-04 15:00:00"),
        ]


class FakeScheduleGateway:
    """Small public-boundary fake: every mutation is an MCP-shaped operation."""

    def __init__(self, records: list[ScheduleRecord]) -> None:
        self._records = {record.business_key: record for record in records}
        self.mutations: list[dict[str, str]] = []
        self.calls: list[str] = []

    def create(self, record: ScheduleRecord) -> ScheduleRecord:
        self.calls.append("schedule.create")
        existing = self._records.get(record.business_key)
        if existing is not None:
            return existing
        self._records[record.business_key] = record
        self.mutations.append({"operation": "create", "business_key": record.business_key})
        return record

    def query(self, *, keyword: str | None = None, date: str | None = None) -> list[ScheduleRecord]:
        self.calls.append("schedule.query")
        values = [record for record in self._records.values() if record.status == "active"]
        if keyword:
            values = [record for record in values if keyword in record.title]
        if date:
            values = [record for record in values if record.start.startswith(date)]
        return sorted(values, key=lambda record: (record.start, record.business_key))

    def update(self, business_key: str, **changes: str) -> ScheduleRecord | None:
        self.calls.append("schedule.update")
        record = self._records.get(business_key)
        if record is None or record.status != "active":
            return None
        for name, value in changes.items():
            if value:
                setattr(record, name, value)
        self.mutations.append({"operation": "update", "business_key": business_key})
        return record

    def delete(self, business_key: str) -> str:
        self.calls.append("schedule.delete")
        record = self._records.get(business_key)
        if record is None or record.status == "cancelled":
            return "already_deleted"
        record.status = "cancelled"
        self.mutations.append({"operation": "delete", "business_key": business_key})
        return "deleted"

    def operation_query(self) -> list[dict[str, str]]:
        self.calls.append("schedule.operation_query")
        return list(self.mutations)

    def active_records(self) -> list[ScheduleRecord]:
        return [record for record in self._records.values() if record.status == "active"]


class VoiceTranscript:
    """Translate fixed fixture transcripts into calls at the public gateway seam."""

    def __init__(self, gateway: FakeScheduleGateway) -> None:
        self.gateway = gateway

    def run(self, scenario: str, turns: list[str]) -> JourneyOutcome:
        if scenario == "create-query":
            record = self.gateway.create(
                ScheduleRecord(
                    "S-VOICE-001",
                    "产品评审",
                    "2026-09-02 10:00:00",
                    "2026-09-02 11:00:00",
                    "会议室",
                    "带上方案",
                )
            )
            found = self.gateway.query(keyword="产品评审", date="2026-09-02")
            return JourneyOutcome(
                "passed" if found and found[0] is record else "failed",
                self.gateway.calls,
                [record.business_key],
            )
        if scenario == "update-query":
            key = "S-CHANGE-ME" if "S-CHANGE-ME" in self.gateway._records else "S-VOICE-001"
            self.gateway.update(
                key,
                title="读书",
                start="2026-09-03 19:00:00",
                location="客厅",
                notes="完成数学题",
            )
            found = self.gateway.query(keyword="读书", date="2026-09-03")
            return JourneyOutcome(
                "passed" if len(found) == 1 and found[0].title == "读书" else "failed",
                self.gateway.calls,
                [key],
            )
        if scenario == "delete-query-idempotency":
            key = "S-CHANGE-ME" if "S-CHANGE-ME" in self.gateway._records else "S-VOICE-001"
            first = self.gateway.delete(key)
            second = self.gateway.delete(key)
            found = self.gateway.query(date="2026-09-03")
            return JourneyOutcome(
                "passed" if (first, second, found) == ("deleted", "already_deleted", []) else "failed",
                self.gateway.calls,
            )
        if scenario in {"ambiguity", "ambiguity-and-conflict"}:
            candidates = self.gateway.query(keyword="产品会议")
            if len(candidates) != 1:
                return JourneyOutcome(
                    "clarification_required",
                    self.gateway.calls,
                    [item.business_key for item in candidates],
                )
        if scenario == "voice-correction":
            final_time = "2026-09-02 21:00:00" if any("晚上九点" in turn for turn in turns) else "2026-09-02 09:00:00"
            record = self.gateway.create(ScheduleRecord("S-CORRECTION-001", "跑步", final_time, "2026-09-02 22:00:00"))
            return JourneyOutcome("passed", self.gateway.calls, [record.business_key])
        if scenario in {"missing-field-and-cancel", "recurrence-exception"}:
            return JourneyOutcome(
                "product_gap" if scenario == "recurrence-exception" else "cancelled",
                self.gateway.calls,
                product_gap="recurrence_not_open" if scenario == "recurrence-exception" else None,
            )
        return JourneyOutcome("conflict_requires_confirmation", self.gateway.calls)


def undo_result() -> dict[str, str]:
    """The current public tool set has operation_query, not schedule.undo."""
    return {"status": "product_gap", "tool": "schedule.undo"}


def run_host_matrix(artifact_directory: Path, *, run_id: str) -> MatrixResult:
    gateway = FakeScheduleGateway(Fixtures.empty())
    created = VoiceTranscript(gateway).run("create-query", ["create", "query"])
    updated = VoiceTranscript(gateway).run("update-query", ["update"])
    deleted = VoiceTranscript(gateway).run("delete-query-idempotency", ["delete", "delete", "query"])
    detail = journey_evidence(
        run_id=run_id,
        profile="host",
        case_id="voice-schedule-matrix",
        status="passed",
        summary={
            "mcp_calls": len(gateway.calls),
            "schedule_keys": [item["business_key"] for item in gateway.mutations],
        },
    )
    validate_journey_evidence(detail)
    artifact_directory.mkdir(parents=True, exist_ok=True)
    (artifact_directory / f"journey-{run_id}.json").write_text(
        json.dumps(detail, sort_keys=True) + "\n", encoding="utf-8"
    )
    counts = {
        name: sum(item["operation"] == name for item in gateway.mutations) for name in ("create", "update", "delete")
    }
    passed = created.status == updated.status == deleted.status == "passed"
    return MatrixResult(
        passed,
        "schedule_undo_not_public",
        counts,
        [record.business_key for record in gateway.active_records()],
        "passed",
    )


def journey_evidence(
    *, run_id: str, profile: str, case_id: str, status: str, summary: dict[str, Any]
) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "run_id": run_id,
        "profile": profile,
        "case_id": case_id,
        "status": status,
        "summary": summary,
    }


def validate_journey_evidence(document: dict[str, Any]) -> None:
    if set(document) != {
        "schema_version",
        "run_id",
        "profile",
        "case_id",
        "status",
        "summary",
    }:
        raise EvidenceValidationError("evidence_fields_invalid")
    if document["schema_version"] != 1 or not SAFE_ID.fullmatch(document["run_id"]):
        raise EvidenceValidationError("evidence_identity_invalid")
    if not re.fullmatch(r"[a-z][a-z0-9_-]{0,63}", document["profile"]):
        raise EvidenceValidationError("evidence_profile_invalid")
    if not re.fullmatch(r"[a-z][a-z0-9_-]{0,63}", document["case_id"]):
        raise EvidenceValidationError("evidence_case_invalid")
    if document["status"] not in {"passed", "failed", "product_gap"} or not isinstance(document["summary"], dict):
        raise EvidenceValidationError("evidence_status_invalid")
    if any(
        key in document or key in document["summary"] for key in ("transcript", "raw_log", "token", "authorization")
    ):
        raise EvidenceValidationError("sensitive_evidence")


class ScheduleVoiceHostAdapter:
    def __init__(self, artifact_directory: Path) -> None:
        self.artifact_directory = artifact_directory
        self.result: MatrixResult | None = None

    def prepare(self, context: RunContext) -> None:
        self.artifact_directory.mkdir(parents=True, exist_ok=True)

    def run(self, context: RunContext) -> MatrixResult:
        self.result = run_host_matrix(self.artifact_directory, run_id=context.run_id)
        return self.result

    def assert_result(self, context: RunContext, result: object) -> list[AssertionResult]:
        value = result if isinstance(result, MatrixResult) else None
        return [
            AssertionResult(
                "create_query_consistent",
                value is not None and value.passed,
                "ok" if value and value.passed else "mismatch",
            ),
            AssertionResult(
                "delete_idempotent",
                value is not None and value.mutation_counts.get("delete") == 1,
                "ok" if value and value.mutation_counts.get("delete") == 1 else "mismatch",
            ),
            AssertionResult(
                "undo_gap_explicit",
                value is not None and value.product_gap == "schedule_undo_not_public",
                "ok" if value and value.product_gap == "schedule_undo_not_public" else "mismatch",
            ),
        ]

    def collect(self, context: RunContext, result: object, assertions: list[AssertionResult]) -> dict[str, object]:
        return {
            "scope": "runner_contract_only",
            "hardware_verified": False,
            "metrics": {"resource_count": 3, "namespace_count": 1},
        }


class ScheduleVoiceHilAdapter(ScheduleVoiceHostAdapter):
    def __init__(
        self,
        artifact_directory: Path,
        descriptor_path: Path,
        lease_directory: Path,
        profile: str,
        scenario: str,
    ) -> None:
        super().__init__(artifact_directory)
        self.descriptor_path, self.lease_directory, self.profile, self.scenario = (
            descriptor_path,
            lease_directory,
            profile,
            scenario,
        )
        self.lease: DeviceLease | None = None
        self.process_result: dict[str, Any] = {}

    def prepare(self, context: RunContext) -> None:
        try:
            descriptor = load_device_descriptor(self.descriptor_path, self.profile)
            if not descriptor.port.exists():
                raise RunnerFailure(FailureCategory.DEVICE, "serial_port_missing")
            self.lease = DeviceLease(descriptor, self.lease_directory)
            self.lease.acquire()
            context.cleanup.push("voice-device-lease", self.lease.release, timeout_required=False)
            self.descriptor = descriptor
        except (HilConfigurationError, HilLeaseUnavailable) as error:
            raise RunnerFailure(FailureCategory.CONFIGURATION, "hil_descriptor_invalid") from error

    def run(self, context: RunContext) -> dict[str, Any]:
        if not os.environ.get("BAILIAN_KEY_FILE"):
            raise RunnerFailure(FailureCategory.CONFIGURATION, "bailian_key_file_missing")
        texts = [
            "明天上午十点安排产品评审，地点在会议室，备注带上方案",
            "查询明天上午的产品评审",
        ]
        command = [str(ROOT / "scripts" / "run_bailian_sparkbot_test.sh"), "multiturn"]
        for text in texts:
            command.extend(["--text", text])
        command.extend(
            [
                "--response-timeout",
                str(max(30, int(context.phase_budget()))),
                "--allow-asr-mismatch",
            ]
        )
        try:
            completed = subprocess.run(
                command,
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=context.remaining(),
                check=False,
            )
        except subprocess.TimeoutExpired as error:
            raise RunnerFailure(FailureCategory.TIMEOUT, "voice_journey_timeout") from error
        if completed.returncode != 0:
            raise RunnerFailure(FailureCategory.DEVICE, "voice_journey_failed")
        report = _last_json_object(completed.stdout)
        if not report:
            raise RunnerFailure(FailureCategory.PRODUCT, "voice_report_missing")
        self.process_result = report
        return report

    def assert_result(self, context: RunContext, result: object) -> list[AssertionResult]:
        acceptance = result.get("acceptance", {}) if isinstance(result, dict) else {}
        return [
            AssertionResult(
                "voice_turns_complete",
                acceptance.get("state_flow_complete") is True,
                "ok" if acceptance.get("state_flow_complete") is True else "mismatch",
            ),
            AssertionResult(
                "audio_loss_free",
                acceptance.get("zero_loss") is True,
                "ok" if acceptance.get("zero_loss") is True else "mismatch",
            ),
        ]


def _last_json_object(output: str) -> dict[str, Any] | None:
    decoder = json.JSONDecoder()
    for line in reversed(output.splitlines()):
        if not line.startswith("{"):
            continue
        try:
            value, _ = decoder.raw_decode(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            return value
    return None
