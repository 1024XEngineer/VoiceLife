#!/usr/bin/env python3
"""Contract-only adapters proving that Host and HIL share one E2E lifecycle."""

from __future__ import annotations

import socket
from pathlib import Path

from e2e_runner import AssertionResult, RunContext


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
