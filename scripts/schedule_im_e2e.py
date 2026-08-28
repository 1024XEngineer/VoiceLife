#!/usr/bin/env python3
# ruff: noqa: E402
"""Issue #352 IM binding and schedule-query Host/HIL E2E fixtures."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from e2e_hil_device import (
    DeviceLease,
    HilConfigurationError,
    HilLeaseUnavailable,
    load_device_descriptor,
)
from e2e_runner import AssertionResult, FailureCategory, RunContext, RunnerFailure

SCENARIOS = (
    "bind-query-mixed",
    "query-date-keyword-empty",
    "dual-scope-isolation",
    "query-retry-idempotency",
    "gateway-unavailable",
    "binding-replace",
    "scope-reject-no-side-effect",
    "unbind-cleanup",
)


@dataclass(frozen=True)
class ScheduleItem:
    key: str
    title: str
    user: str
    kind: str = "once"


class FakeImGateway:
    def __init__(self) -> None:
        self.bindings: dict[str, str] = {}
        self.messages: dict[str, dict[str, Any]] = {}
        self.available = True

    def bind(self, device: str, user: str, code: str = "PAIR-001") -> str:
        if code != "PAIR-001":
            return "rejected"
        self.bindings[device] = user
        return "confirmed"

    def deliver(self, query_id: str, device: str, user: str, payload: list[ScheduleItem]) -> str:
        if self.bindings.get(device) != user:
            return "scope_rejected"
        if not self.available:
            return "retryable_failed"
        if query_id in self.messages:
            return "already_delivered"
        self.messages[query_id] = {
            "device": device,
            "user": user,
            "keys": [item.key for item in payload],
        }
        return "submitted"


class QueryJourney:
    def __init__(self, gateway: FakeImGateway) -> None:
        self.gateway = gateway
        self.items = [
            ScheduleItem("S-ONCE-001", "产品评审", "U-001"),
            ScheduleItem("R-DAILY-READING:2026-09-01", "阅读", "U-001", "occurrence"),
            ScheduleItem("EX-READING-001", "阅读", "U-001", "exception"),
            ScheduleItem("S-ONCE-002", "产品评审", "U-002"),
        ]

    def query(self, query_id: str, device: str, user: str, *, keyword: str | None = None) -> dict[str, Any]:
        scoped = [item for item in self.items if item.user == user]
        if keyword:
            scoped = [item for item in scoped if keyword in item.title]
        delivery = self.gateway.deliver(query_id, device, user, scoped)
        return {
            "query_id": query_id,
            "keys": [item.key for item in scoped],
            "delivery": delivery,
            "count": len(scoped),
        }


def run_host_matrix() -> dict[str, Any]:
    gateway = FakeImGateway()
    bind_status = gateway.bind("D-SPARK-01", "U-001")
    journey = QueryJourney(gateway)
    mixed = journey.query("Q-001", "D-SPARK-01", "U-001")
    duplicate = journey.query("Q-001", "D-SPARK-01", "U-001")
    other = gateway.bind("D-PCB-01", "U-002")
    isolated = journey.query("Q-002", "D-PCB-01", "U-002")
    gateway.available = False
    failed = journey.query("Q-RETRY-001", "D-SPARK-01", "U-001")
    gateway.available = True
    retried = journey.query("Q-RETRY-001", "D-SPARK-01", "U-001")
    rejected = journey.query("Q-SCOPE-REJECT", "D-SPARK-01", "U-002")
    gateway.bind("D-SPARK-01", "U-002")
    replaced = journey.query("Q-REPLACE", "D-SPARK-01", "U-002")
    return {
        "status": "passed"
        if bind_status == "confirmed"
        and mixed["delivery"] == "submitted"
        and duplicate["delivery"] == "already_delivered"
        and other == "confirmed"
        and isolated["keys"] != mixed["keys"]
        and failed["delivery"] == "retryable_failed"
        and retried["delivery"] == "submitted"
        and rejected["delivery"] == "scope_rejected"
        and replaced["delivery"] == "submitted"
        else "failed",
        "mixed_count": mixed["count"],
        "duplicate_delivery": duplicate["delivery"],
        "retry": (failed["delivery"], retried["delivery"]),
        "scope_rejected": rejected["delivery"],
        "message_count": len(gateway.messages),
        "binding_count": len(gateway.bindings),
    }


class ImHostAdapter:
    def __init__(self, artifact_directory: Path) -> None:
        self.artifact_directory = artifact_directory

    def prepare(self, context: RunContext) -> None:
        self.artifact_directory.mkdir(parents=True, exist_ok=True)

    def run(self, context: RunContext) -> dict[str, Any]:
        result = run_host_matrix()
        (self.artifact_directory / f"im-query-{context.run_id}.json").write_text(
            json.dumps(result, sort_keys=True) + "\n", encoding="utf-8"
        )
        return result

    def assert_result(self, context: RunContext, result: object) -> list[AssertionResult]:
        values = result if isinstance(result, dict) else {}
        return [
            AssertionResult(
                name,
                values.get(key) == expected,
                "ok" if values.get(key) == expected else "mismatch",
            )
            for name, key, expected in (
                ("mixed_query_complete", "mixed_count", 3),
                (
                    "retryable_delivery_recovered",
                    "retry",
                    ("retryable_failed", "submitted"),
                ),
                ("scope_rejected_without_delivery", "scope_rejected", "scope_rejected"),
                ("same_query_deduplicated", "duplicate_delivery", "already_delivered"),
            )
        ]

    def collect(self, context: RunContext, result: object, assertions: list[AssertionResult]) -> dict[str, object]:
        return {
            "scope": "runner_contract_only",
            "hardware_verified": False,
            "metrics": {"resource_count": 5, "namespace_count": 1},
        }


class ImHilAdapter(ImHostAdapter):
    def __init__(
        self,
        artifact_directory: Path,
        descriptor_path: Path,
        lease_directory: Path,
        profile: str,
    ) -> None:
        super().__init__(artifact_directory)
        self.descriptor_path, self.lease_directory, self.profile = (
            descriptor_path,
            lease_directory,
            profile,
        )
        self.lease: DeviceLease | None = None

    def prepare(self, context: RunContext) -> None:
        try:
            descriptor = load_device_descriptor(self.descriptor_path, self.profile)
            if not descriptor.port.exists():
                raise RunnerFailure(FailureCategory.DEVICE, "serial_port_missing")
            self.lease = DeviceLease(descriptor, self.lease_directory)
            self.lease.acquire()
            context.cleanup.push("im-device-lease", self.lease.release, timeout_required=False)
            self.descriptor = descriptor
        except (HilConfigurationError, HilLeaseUnavailable) as error:
            raise RunnerFailure(FailureCategory.CONFIGURATION, "hil_descriptor_invalid") from error

    def run(self, context: RunContext) -> dict[str, Any]:
        if not os.environ.get("BAILIAN_KEY_FILE"):
            raise RunnerFailure(FailureCategory.CONFIGURATION, "bailian_key_file_missing")
        command = [
            str(ROOT / "scripts" / "run_bailian_sparkbot_test.sh"),
            "multiturn",
            "--text",
            "查询今天的日程",
            "--allow-asr-mismatch",
            "--response-timeout",
            str(max(30, int(context.phase_budget()))),
        ]
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
            raise RunnerFailure(FailureCategory.TIMEOUT, "im_query_voice_timeout") from error
        if completed.returncode != 0:
            raise RunnerFailure(FailureCategory.DEVICE, "im_query_voice_failed")
        return {"status": "passed", "binding": "sanitized", "delivery": "observed"}
