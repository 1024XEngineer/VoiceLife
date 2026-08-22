#!/usr/bin/env python3
"""Contract-only adapters proving that Host and HIL share one E2E lifecycle."""

from __future__ import annotations

import json
import os
import signal
import socket
import subprocess
from contextlib import suppress
from pathlib import Path

from e2e_runner import AssertionResult, FailureCategory, RunContext, RunnerFailure


class HostLifecycleExampleAdapter:
    """Allocate only local resources; this is not a product end-to-end journey."""

    def __init__(self) -> None:
        self.observed_port = 0
        self.observed_namespace = ""
        self.observed_temp_directory = Path()
        self._listener: socket.socket | None = None

    def prepare(self, context: RunContext) -> None:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        context.cleanup.push("host-listener", listener.close)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        self._listener = listener
        self.observed_port = int(listener.getsockname()[1])
        self.observed_namespace = context.database_namespace
        self.observed_temp_directory = context.temporary_directory

    def run(self, context: RunContext) -> dict[str, bool]:
        context.remaining()
        return {
            "listener_bound": self._listener is not None and self.observed_port > 0,
            "namespace_allocated": self.observed_namespace == context.database_namespace,
            "temp_directory_allocated": self.observed_temp_directory == context.temporary_directory,
        }

    def assert_result(self, context: RunContext, result: object) -> list[AssertionResult]:
        values = result if isinstance(result, dict) else {}
        if os.environ.get("VOICELIFE_E2E_CONTRACT_FAILURE") == "1":
            return [AssertionResult(name="lifecycle_complete", passed=False, code="contract_failure")]
        passed = all(values.get(name) is True for name in values)
        return [AssertionResult(name="lifecycle_complete", passed=passed, code="ok" if passed else "incomplete")]

    def collect(self, context: RunContext, result: object, assertions: list[AssertionResult]) -> dict[str, object]:
        return {
            "scope": "runner_contract_only",
            "hardware_verified": False,
            "metrics": {"bound_port_count": 1, "namespace_count": 1, "resource_count": 3},
        }


class HostImGatewayE2EAdapter:
    """Drive the real IM Gateway HTTP/SSE journey from the shared Host runner."""

    def __init__(self) -> None:
        self.result: dict[str, object] = {}
        self._process: subprocess.Popen[str] | None = None

    def prepare(self, context: RunContext) -> None:
        environment = {**os.environ, "E2E_RUN_ID": context.run_id}
        self._process = subprocess.Popen(
            ["pnpm", "--dir", "services/im-gateway", "run", "e2e:host"],
            cwd=Path(__file__).resolve().parent.parent,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        context.cleanup.push("host-gateway-process", self._cleanup_process)

    def run(self, context: RunContext) -> dict[str, object]:
        process = self._process
        if process is None:
            raise RunnerFailure(FailureCategory.INFRASTRUCTURE, "host_gateway_process_missing")
        try:
            stdout, stderr = process.communicate(timeout=max(1.0, context.remaining()))
        except subprocess.TimeoutExpired as error:
            raise RunnerFailure(FailureCategory.TIMEOUT, "host_gateway_timeout") from error
        if process.returncode != 0:
            marker = stderr.strip().splitlines()[-1] if stderr.strip() else ""
            category = {
                "host_e2e_cleanup_failed": FailureCategory.CLEANUP,
                "host_e2e_infrastructure_failed": FailureCategory.INFRASTRUCTURE,
            }.get(marker, FailureCategory.PRODUCT)
            raise RunnerFailure(category, "host_gateway_journey_failed")
        try:
            value = json.loads(stdout.strip().splitlines()[-1])
        except (json.JSONDecodeError, TypeError) as error:
            raise RunnerFailure(FailureCategory.PRODUCT, "host_gateway_invalid_result") from error
        if not isinstance(value, dict):
            raise RunnerFailure(FailureCategory.PRODUCT, "host_gateway_invalid_result")
        self.result = value
        return value

    def assert_result(self, context: RunContext, result: object) -> list[AssertionResult]:
        context.remaining()
        values = result if isinstance(result, dict) else {}
        checks = {
            "http_sse_journey": values.get("assertions") == 22,
            "one_send_per_delivery": values.get("sendCount") == 3 and values.get("deliveryCount") == 3,
            "two_worker_dispatch": values.get("workerCount") == 2,
            "accepted_then_delivered": values.get("receiptCount") == 1,
            "persistent_delivery": values.get("deliveryCount") == 3,
            "idempotent_action": values.get("actionCount") == 2,
        }
        return [
            AssertionResult(name=name, passed=passed, code="ok" if passed else "mismatch")
            for name, passed in checks.items()
        ]

    def collect(self, context: RunContext, result: object, assertions: list[AssertionResult]) -> dict[str, object]:
        context.remaining()
        values = result if isinstance(result, dict) else {}
        return {
            "scope": "runner_contract_only",
            "hardware_verified": False,
            "metrics": {
                "resource_count": 8,
                "namespace_count": 1,
                "bound_port_count": 1,
            },
            "journey_values": {
                key: values[key]
                for key in ("deliveryCount", "sendCount", "receiptCount", "actionCount", "workerCount")
                if key in values
            },
        }

    def _cleanup_process(self) -> None:
        process = self._process
        if process is None:
            return
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            return
        if process.poll() is not None:
            return
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            with suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=2)


class HostImGatewayRecoveryE2EAdapter:
    """Run the process-level PostgreSQL recovery matrix and retain detailed evidence."""

    def __init__(self, artifact_directory: Path) -> None:
        self.artifact_directory = artifact_directory
        self.result: dict[str, object] = {}
        self._process: subprocess.Popen[str] | None = None

    def prepare(self, context: RunContext) -> None:
        # Detailed recovery snapshots may contain internal database fields; keep them
        # in the runner-owned temporary directory instead of the public artifact tree.
        detail_path = context.temporary_directory / "recovery-details" / f"recovery-{context.run_id}.json"
        environment = {
            **os.environ,
            "E2E_RUN_ID": context.run_id,
            "E2E_RECOVERY_EVIDENCE": str(detail_path),
        }
        self._process = subprocess.Popen(
            ["pnpm", "--dir", "services/im-gateway", "run", "e2e:recovery"],
            cwd=Path(__file__).resolve().parent.parent,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        context.cleanup.push("host-recovery-process", self._cleanup_process)

    def run(self, context: RunContext) -> dict[str, object]:
        process = self._process
        if process is None:
            raise RunnerFailure(FailureCategory.INFRASTRUCTURE, "host_recovery_process_missing")
        try:
            stdout, stderr = process.communicate(timeout=max(1.0, context.remaining()))
        except subprocess.TimeoutExpired as error:
            raise RunnerFailure(FailureCategory.TIMEOUT, "host_recovery_timeout") from error
        if process.returncode != 0:
            marker = stderr.strip().splitlines()[-1] if stderr.strip() else ""
            category = {
                "host_recovery_cleanup_failed": FailureCategory.CLEANUP,
                "host_recovery_infrastructure_failed": FailureCategory.INFRASTRUCTURE,
                "host_recovery_e2e_failed": FailureCategory.PRODUCT,
            }.get(marker, FailureCategory.INFRASTRUCTURE)
            raise RunnerFailure(category, "host_recovery_journey_failed")
        try:
            value = json.loads(stdout.strip().splitlines()[-1])
        except (json.JSONDecodeError, IndexError, TypeError) as error:
            raise RunnerFailure(FailureCategory.PRODUCT, "host_recovery_invalid_result") from error
        if not isinstance(value, dict):
            raise RunnerFailure(FailureCategory.PRODUCT, "host_recovery_invalid_result")
        self.result = value
        return value

    def assert_result(self, context: RunContext, result: object) -> list[AssertionResult]:
        context.remaining()
        values = result if isinstance(result, dict) else {}
        scenario_count = values.get("scenarioCount")
        valid_scenario_count = scenario_count if isinstance(scenario_count, int) else 0
        checks = {
            "recovery_matrix_complete": values.get("status") == "passed"
            and scenario_count == values.get("requestedCount"),
            "recovery_assertions_recorded": isinstance(values.get("assertions"), int)
            and values.get("assertions", 0) > 0,
            "persistent_recovery_observed": isinstance(values.get("deliveryCount"), int)
            and values.get("deliveryCount", 0) >= valid_scenario_count,
            "platform_side_effects_bounded": isinstance(values.get("platformSendCount"), int)
            and values.get("platformSendCount", 0) <= values.get("attemptCount", 0),
        }
        return [
            AssertionResult(name=name, passed=passed, code="ok" if passed else "mismatch")
            for name, passed in checks.items()
        ]

    def collect(self, context: RunContext, result: object, assertions: list[AssertionResult]) -> dict[str, object]:
        context.remaining()
        values = result if isinstance(result, dict) else {}
        raw_count = values.get("scenarioCount", 0)
        count = raw_count if isinstance(raw_count, int) else 0
        return {
            "scope": "runner_contract_only",
            "hardware_verified": False,
            "metrics": {
                "resource_count": count * 4,
                "namespace_count": count,
                "bound_port_count": count,
            },
        }

    def _cleanup_process(self) -> None:
        process = self._process
        if process is None:
            return
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            return
        if process.poll() is not None:
            return
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            with suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=2)


class HilLifecycleExampleAdapter:
    """Exercise a lease-like resource without opening or claiming real hardware."""

    def __init__(self) -> None:
        self.lease_held = False

    def prepare(self, context: RunContext) -> None:
        self.lease_held = True
        context.cleanup.push("example-hil-lease", self._release_lease)

    def _release_lease(self) -> None:
        self.lease_held = False

    def run(self, context: RunContext) -> dict[str, bool]:
        context.remaining()
        return {"lease_held": self.lease_held}

    def assert_result(self, context: RunContext, result: object) -> list[AssertionResult]:
        passed = isinstance(result, dict) and result.get("lease_held") is True
        return [AssertionResult(name="lifecycle_complete", passed=passed, code="ok" if passed else "incomplete")]

    def collect(self, context: RunContext, result: object, assertions: list[AssertionResult]) -> dict[str, object]:
        return {
            "scope": "runner_contract_only",
            "hardware_verified": False,
            "metrics": {"resource_count": 1},
        }
