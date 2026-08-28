#!/usr/bin/env python3
# ruff: noqa: E402
"""Issue #351 reminder-chain Host/HIL E2E fixtures."""

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
)  # noqa: E402
from e2e_runner import (  # noqa: E402
    AssertionResult,
    FailureCategory,
    RunContext,
    RunnerFailure,
)

SCENARIOS = (
    "attempt-chain-terminal",
    "im-ack-single-consume",
    "voice-ack-single-consume",
    "concurrent-ack-idempotency",
    "im-snooze-window",
    "restart-pending-chain",
    "outbox-retry-no-false-success",
    "voice-output-failure",
)


@dataclass
class ReminderTask:
    chain_id: str = "C-REM-001"
    attempt: int = 0
    status: str = "pending"
    next_at: int = 0


class FakeClock:
    def __init__(self, now: int = 0) -> None:
        self.now = now

    def advance(self, seconds: int) -> None:
        self.now += seconds


class ReminderChain:
    def __init__(self, clock: FakeClock | None = None) -> None:
        self.clock = clock or FakeClock()
        self.task = ReminderTask(next_at=self.clock.now)
        self.deliveries: list[dict[str, Any]] = []
        self.winners: list[str] = []

    def trigger(self) -> dict[str, Any]:
        if self.task.status != "pending" or self.task.next_at > self.clock.now:
            return {"status": self.task.status, "attempt": self.task.attempt}
        if self.task.attempt >= 3:
            self.task.status = "exhausted"
            return {"status": "exhausted", "attempt": self.task.attempt}
        self.task.attempt += 1
        delivery = {
            "chain_id": self.task.chain_id,
            "attempt": self.task.attempt,
            "status": "sent",
        }
        self.deliveries.append(delivery)
        if self.task.attempt == 3:
            self.task.status = "exhausted"
        else:
            self.task.next_at = self.clock.now + 60
        return delivery

    def acknowledge(self, source: str) -> str:
        if self.task.status in {"acknowledged", "exhausted"}:
            return "already_processed"
        self.task.status = "acknowledged"
        self.winners.append(source)
        return "acknowledged"

    def snooze(self, minutes: int) -> str:
        if self.task.status != "pending" or minutes <= 0:
            return "rejected"
        self.task.next_at = self.clock.now + minutes * 60
        return "snoozed"

    def restart(self) -> str:
        return "restored" if self.task.status == "pending" else "terminal"


def run_host_matrix() -> dict[str, Any]:
    chain = ReminderChain()
    chain.trigger()
    chain.clock.advance(60)
    chain.trigger()
    chain.clock.advance(60)
    chain.trigger()
    chain.clock.advance(60)
    fourth = chain.trigger()
    terminal = chain.task.status == "exhausted" and fourth["attempt"] == 3

    single = ReminderChain()
    single.trigger()
    im_result = single.acknowledge("im")
    voice_result = single.acknowledge("voice")

    concurrent = ReminderChain()
    concurrent.trigger()
    first = concurrent.acknowledge("im")
    second = concurrent.acknowledge("voice")

    snooze = ReminderChain()
    snooze.trigger()
    snooze.task.status = "pending"
    snooze_result = snooze.snooze(10)
    snooze.clock.advance(600)
    snooze_delivery = snooze.trigger()
    return {
        "terminal": terminal,
        "delivery_count": len(chain.deliveries),
        "single_consume": (im_result, voice_result) == ("acknowledged", "already_processed"),
        "concurrent_winner": (first, second) == ("acknowledged", "already_processed"),
        "winner_count": len(concurrent.winners),
        "snooze": snooze_result == "snoozed" and snooze_delivery["attempt"] == 2,
        "no_attempt_four": all(item["attempt"] <= 3 for item in chain.deliveries),
        "status": "passed" if terminal and len(chain.deliveries) == 3 else "failed",
    }


class ReminderHostAdapter:
    def __init__(self, artifact_directory: Path) -> None:
        self.artifact_directory = artifact_directory

    def prepare(self, context: RunContext) -> None:
        self.artifact_directory.mkdir(parents=True, exist_ok=True)

    def run(self, context: RunContext) -> dict[str, Any]:
        result = run_host_matrix()
        (self.artifact_directory / f"reminder-{context.run_id}.json").write_text(
            json.dumps(
                {
                    "run_id": context.run_id,
                    "status": result["status"],
                    "metrics": result,
                },
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
        return result

    def assert_result(self, context: RunContext, result: object) -> list[AssertionResult]:
        values = result if isinstance(result, dict) else {}
        return [
            AssertionResult(
                name,
                values.get(key) is True,
                "ok" if values.get(key) is True else "mismatch",
            )
            for name, key in (
                ("three_attempts_terminal", "terminal"),
                ("single_consume", "single_consume"),
                ("concurrent_single_winner", "concurrent_winner"),
                ("snooze_requeues_once", "snooze"),
                ("no_attempt_four", "no_attempt_four"),
            )
        ]

    def collect(self, context: RunContext, result: object, assertions: list[AssertionResult]) -> dict[str, object]:
        return {
            "scope": "runner_contract_only",
            "hardware_verified": False,
            "metrics": {"resource_count": 4, "namespace_count": 1},
        }


class ReminderHilAdapter(ReminderHostAdapter):
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
            context.cleanup.push("reminder-device-lease", self.lease.release, timeout_required=False)
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
            "知道了",
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
            raise RunnerFailure(FailureCategory.TIMEOUT, "reminder_voice_timeout") from error
        if completed.returncode != 0:
            raise RunnerFailure(FailureCategory.DEVICE, "reminder_voice_failed")
        return {"status": "passed", "voice_report": "sanitized"}
