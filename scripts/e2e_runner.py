#!/usr/bin/env python3
"""Shared lifecycle and failure contract for VoiceLife end-to-end runners."""

from __future__ import annotations

import math
import re
import secrets
import shutil
import signal
import tempfile
import threading
import time
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from dataclasses import dataclass, field
from enum import Enum, IntEnum
from pathlib import Path
from typing import Protocol

NAME_PATTERN = re.compile(r"^[a-z0-9][a-z0-9_-]{0,63}$")
PHASES = ("prepare", "run", "assert", "collect")


class FailureCategory(str, Enum):
    """Stable public categories used to distinguish product and environment failures."""

    CONFIGURATION = "configuration"
    INFRASTRUCTURE = "infrastructure"
    PRODUCT = "product"
    DEVICE = "device"
    EXTERNAL = "external"
    TIMEOUT = "timeout"
    INTERRUPTED = "interrupted"
    CLEANUP = "cleanup"


class ExitCode(IntEnum):
    """Stable process exit codes for the E2E command-line interface."""

    SUCCESS = 0
    CONFIGURATION = 2
    INFRASTRUCTURE = 10
    PRODUCT = 20
    DEVICE = 30
    EXTERNAL = 40
    TIMEOUT = 60
    INTERRUPTED = 70
    CLEANUP = 80


class RunStatus(str, Enum):
    PASSED = "passed"
    FAILED = "failed"


_EXIT_CODES = {
    FailureCategory.CONFIGURATION: ExitCode.CONFIGURATION,
    FailureCategory.INFRASTRUCTURE: ExitCode.INFRASTRUCTURE,
    FailureCategory.PRODUCT: ExitCode.PRODUCT,
    FailureCategory.DEVICE: ExitCode.DEVICE,
    FailureCategory.EXTERNAL: ExitCode.EXTERNAL,
    FailureCategory.TIMEOUT: ExitCode.TIMEOUT,
    FailureCategory.INTERRUPTED: ExitCode.INTERRUPTED,
    FailureCategory.CLEANUP: ExitCode.CLEANUP,
}


def exit_code_for(category: FailureCategory) -> ExitCode:
    return _EXIT_CODES[category]


class RunnerFailure(Exception):
    """Controlled failure carrying only a public category and stable message code."""

    def __init__(self, category: FailureCategory, message_code: str) -> None:
        if not NAME_PATTERN.fullmatch(message_code):
            raise ValueError("message_code must be a safe lowercase identifier")
        super().__init__(message_code)
        self.category = category
        self.message_code = message_code


class RunnerDeadlineExceeded(Exception):
    pass


@dataclass(frozen=True)
class RunnerConfig:
    layer: str
    journey: str
    profile: str
    hard_timeout_s: float
    phase_timeout_s: float
    cleanup_timeout_s: float
    retries: int = 0

    def __post_init__(self) -> None:
        if self.layer not in {"host", "hil"}:
            raise ValueError("layer must be host or hil")
        for name, value in (("journey", self.journey), ("profile", self.profile)):
            if not NAME_PATTERN.fullmatch(value):
                raise ValueError(f"{name} must be a safe lowercase identifier")
        for name, value in (
            ("hard_timeout_s", self.hard_timeout_s),
            ("phase_timeout_s", self.phase_timeout_s),
            ("cleanup_timeout_s", self.cleanup_timeout_s),
        ):
            if not math.isfinite(value) or value <= 0:
                raise ValueError(f"{name} must be a positive finite number")
        if self.retries != 0:
            raise ValueError("runner retries must remain zero")


@dataclass(frozen=True)
class AssertionResult:
    name: str
    passed: bool
    code: str

    def __post_init__(self) -> None:
        if not NAME_PATTERN.fullmatch(self.name) or not NAME_PATTERN.fullmatch(self.code):
            raise ValueError("assertion names and codes must be safe identifiers")


@dataclass(frozen=True)
class CleanupError:
    code: str


@dataclass
class _CleanupEntry:
    callback: Callable[[], None] = field(repr=False)
    done: bool = False


class CleanupStack:
    """A LIFO cleanup registry that is idempotent and continues after failures."""

    def __init__(self) -> None:
        self._entries: list[_CleanupEntry] = []

    def push(self, name: str, callback: Callable[[], None]) -> None:
        if not NAME_PATTERN.fullmatch(name):
            raise ValueError("cleanup name must be a safe lowercase identifier")
        self._entries.append(_CleanupEntry(callback=callback))

    def cleanup(self, deadline: float) -> list[CleanupError]:
        errors: list[CleanupError] = []
        for entry in reversed(self._entries):
            if entry.done:
                continue
            if time.monotonic() >= deadline:
                errors.append(CleanupError(code="cleanup_timeout"))
                break
            entry.done = True
            try:
                with _deadline_guard(deadline - time.monotonic()):
                    entry.callback()
            except KeyboardInterrupt:
                raise
            except RunnerDeadlineExceeded:
                errors.append(CleanupError(code="cleanup_timeout"))
                break
            except Exception:
                errors.append(CleanupError(code="cleanup_callback_failed"))
        return errors


@dataclass
class RunContext:
    config: RunnerConfig
    run_id: str
    correlation_id: str
    started_monotonic: float
    deadline: float
    temporary_directory: Path = field(repr=False)
    database_namespace: str
    cleanup: CleanupStack = field(repr=False)
    phase: str = "prepare"

    def remaining(self) -> float:
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise RunnerDeadlineExceeded
        return remaining

    def phase_budget(self) -> float:
        return min(self.config.phase_timeout_s, self.remaining())


class RunnerAdapter(Protocol):
    def prepare(self, context: RunContext) -> None: ...

    def run(self, context: RunContext) -> object: ...

    def assert_result(self, context: RunContext, result: object) -> list[AssertionResult]: ...

    def collect(self, context: RunContext, result: object, assertions: list[AssertionResult]) -> dict[str, object]: ...


@dataclass(frozen=True)
class RunnerResult:
    status: RunStatus
    failure_category: FailureCategory | None
    failed_phase: str | None
    exit_code: ExitCode
    message_code: str
    run_id: str
    correlation_id: str
    primary_failure_category: FailureCategory | None
    primary_failed_phase: str | None
    primary_message_code: str
    cleanup_errors: tuple[CleanupError, ...]
    assertions: tuple[AssertionResult, ...]
    collected: dict[str, object] = field(repr=False)
    started_monotonic: float = field(repr=False)
    finished_monotonic: float = field(repr=False)


@contextmanager
def _deadline_guard(timeout_s: float) -> Iterator[None]:
    if timeout_s <= 0:
        raise RunnerDeadlineExceeded
    if threading.current_thread() is not threading.main_thread() or not hasattr(signal, "setitimer"):
        raise RunnerFailure(FailureCategory.INFRASTRUCTURE, "runner_requires_main_thread")

    previous_handler = signal.getsignal(signal.SIGALRM)
    previous_delay, previous_interval = signal.getitimer(signal.ITIMER_REAL)
    started = time.monotonic()

    def handle_timeout(signum: int, frame: object) -> None:
        raise RunnerDeadlineExceeded

    signal.signal(signal.SIGALRM, handle_timeout)
    signal.setitimer(signal.ITIMER_REAL, timeout_s)
    try:
        yield
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous_handler)
        elapsed = time.monotonic() - started
        restored_delay = max(0.0, previous_delay - elapsed) if previous_delay > 0 else 0.0
        if restored_delay > 0 or previous_interval > 0:
            signal.setitimer(signal.ITIMER_REAL, restored_delay, previous_interval)


def _new_context(config: RunnerConfig) -> RunContext:
    started = time.monotonic()
    run_id = secrets.token_hex(16)
    cleanup = CleanupStack()
    temporary_directory = Path(tempfile.mkdtemp(prefix="voicelife-e2e-"))
    cleanup.push("temporary-directory", lambda: shutil.rmtree(temporary_directory, ignore_errors=False))
    return RunContext(
        config=config,
        run_id=run_id,
        correlation_id=secrets.token_hex(16),
        started_monotonic=started,
        deadline=started + config.hard_timeout_s,
        temporary_directory=temporary_directory,
        database_namespace=f"e2e_{run_id}",
        cleanup=cleanup,
    )


def _initialization_failure_result(started: float | None = None) -> RunnerResult:
    now = time.monotonic()
    started = now if started is None else started
    return RunnerResult(
        status=RunStatus.FAILED,
        failure_category=FailureCategory.INFRASTRUCTURE,
        failed_phase="prepare",
        exit_code=ExitCode.INFRASTRUCTURE,
        message_code="context_initialization_failed",
        run_id=secrets.token_hex(16),
        correlation_id=secrets.token_hex(16),
        primary_failure_category=FailureCategory.INFRASTRUCTURE,
        primary_failed_phase="prepare",
        primary_message_code="context_initialization_failed",
        cleanup_errors=(),
        assertions=(),
        collected={},
        started_monotonic=started,
        finished_monotonic=now,
    )


def _unsupported_thread_result() -> RunnerResult:
    now = time.monotonic()
    return RunnerResult(
        status=RunStatus.FAILED,
        failure_category=FailureCategory.INFRASTRUCTURE,
        failed_phase="prepare",
        exit_code=ExitCode.INFRASTRUCTURE,
        message_code="runner_requires_main_thread",
        run_id=secrets.token_hex(16),
        correlation_id=secrets.token_hex(16),
        primary_failure_category=FailureCategory.INFRASTRUCTURE,
        primary_failed_phase="prepare",
        primary_message_code="runner_requires_main_thread",
        cleanup_errors=(),
        assertions=(),
        collected={},
        started_monotonic=now,
        finished_monotonic=now,
    )


def run_e2e(config: RunnerConfig, adapter: RunnerAdapter) -> RunnerResult:
    """Run one adapter through the shared lifecycle and always finalize resources."""
    if threading.current_thread() is not threading.main_thread() or not hasattr(signal, "setitimer"):
        return _unsupported_thread_result()
    try:
        context = _new_context(config)
    except Exception:
        return _initialization_failure_result()
    category: FailureCategory | None = None
    failed_phase: str | None = None
    message_code = "run_passed"
    assertions: list[AssertionResult] = []
    collected: dict[str, object] = {}
    adapter_result: object = None

    try:
        for phase in PHASES:
            context.phase = phase
            with _deadline_guard(context.phase_budget()):
                if phase == "prepare":
                    adapter.prepare(context)
                elif phase == "run":
                    adapter_result = adapter.run(context)
                elif phase == "assert":
                    assertions = adapter.assert_result(context, adapter_result)
                    if not all(assertion.passed for assertion in assertions):
                        raise RunnerFailure(FailureCategory.PRODUCT, "journey_assertion_failed")
                else:
                    collected = adapter.collect(context, adapter_result, assertions)
    except RunnerFailure as error:
        category = error.category
        failed_phase = context.phase
        message_code = error.message_code
    except RunnerDeadlineExceeded:
        category = FailureCategory.TIMEOUT
        failed_phase = context.phase
        message_code = f"{context.phase}_timeout"
    except KeyboardInterrupt:
        category = FailureCategory.INTERRUPTED
        failed_phase = context.phase
        message_code = f"{context.phase}_interrupted"
    except Exception:
        category = FailureCategory.INFRASTRUCTURE
        failed_phase = context.phase
        message_code = f"{context.phase}_unexpected_error"

    primary_category = category
    primary_failed_phase = failed_phase
    primary_message_code = message_code
    cleanup_deadline = time.monotonic() + config.cleanup_timeout_s
    try:
        cleanup_errors = context.cleanup.cleanup(cleanup_deadline)
    except KeyboardInterrupt:
        cleanup_errors = context.cleanup.cleanup(cleanup_deadline)
        category = FailureCategory.INTERRUPTED
        failed_phase = "cleanup"
        message_code = "cleanup_interrupted"
    if cleanup_errors:
        category = FailureCategory.CLEANUP
        failed_phase = "cleanup"
        message_code = cleanup_errors[0].code

    finished = time.monotonic()
    status = RunStatus.PASSED if category is None else RunStatus.FAILED
    exit_code = ExitCode.SUCCESS if category is None else exit_code_for(category)
    return RunnerResult(
        status=status,
        failure_category=category,
        failed_phase=failed_phase,
        exit_code=exit_code,
        message_code=message_code,
        run_id=context.run_id,
        correlation_id=context.correlation_id,
        primary_failure_category=primary_category,
        primary_failed_phase=primary_failed_phase,
        primary_message_code=primary_message_code,
        cleanup_errors=tuple(cleanup_errors),
        assertions=tuple(assertions),
        collected=collected,
        started_monotonic=context.started_monotonic,
        finished_monotonic=finished,
    )
