#!/usr/bin/env python3
"""Contract-only adapters proving that Host and HIL share one E2E lifecycle."""

from __future__ import annotations

import json
import os
import socket
import subprocess
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

    def prepare(self, context: RunContext) -> None:
        context.remaining()

    def run(self, context: RunContext) -> dict[str, object]:
        remaining = max(1.0, context.remaining())
        environment = {**os.environ, "E2E_RUN_ID": context.run_id}
        process = subprocess.run(
            ["pnpm", "--dir", "services/im-gateway", "run", "e2e:host"],
            cwd=Path(__file__).resolve().parent.parent,
            env=environment,
            capture_output=True,
            text=True,
            timeout=remaining,
            check=False,
        )
        if process.returncode != 0:
            category = (
                FailureCategory.INFRASTRUCTURE
                if process.stderr.strip().endswith("host_e2e_infrastructure_failed")
                else FailureCategory.PRODUCT
            )
            raise RunnerFailure(category, "host_gateway_journey_failed")
        try:
            value = json.loads(process.stdout.strip().splitlines()[-1])
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
            "http_sse_journey": values.get("assertions") == 10,
            "one_send_per_delivery": values.get("sendCount") == 2,
            "accepted_then_delivered": values.get("receiptCount") == 1,
            "persistent_delivery": values.get("deliveryCount") == 2,
            "idempotent_action": values.get("actionCount") == 1,
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
                "resource_count": 4,
                "namespace_count": 1,
                "bound_port_count": 1,
            },
            "journey_values": {
                key: values[key]
                for key in ("deliveryCount", "sendCount", "receiptCount", "actionCount")
                if key in values
            },
        }


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
