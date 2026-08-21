from __future__ import annotations

import importlib.util
import math
import signal
import sys
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "scripts" / "e2e_runner.py"
SPEC = importlib.util.spec_from_file_location("e2e_runner", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
RUNNER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = RUNNER
SPEC.loader.exec_module(RUNNER)


class SpyAdapter:
    def __init__(self, *, fail_phase: str | None = None, failure: BaseException | None = None) -> None:
        self.calls: list[str] = []
        self.fail_phase = fail_phase
        self.failure = failure
        self.resources: list[str] = []

    def _record(self, phase: str) -> None:
        self.calls.append(phase)
        if self.fail_phase == phase:
            if self.failure is not None:
                raise self.failure
            raise RuntimeError(f"{phase} failed with secret detail")

    def prepare(self, context: object) -> None:
        self._record("prepare")
        context.cleanup.push("resource-a", lambda: self.resources.append("a"))
        context.cleanup.push("resource-b", lambda: self.resources.append("b"))

    def run(self, context: object) -> dict[str, object]:
        self._record("run")
        return {"observed": True}

    def assert_result(self, context: object, result: object) -> list[object]:
        self._record("assert")
        return [RUNNER.AssertionResult(name="lifecycle_complete", passed=True, code="ok")]

    def collect(self, context: object, result: object, assertions: list[object]) -> dict[str, object]:
        self._record("collect")
        return {"metrics": {"resource_count": 2}}


class CleanupStackTest(unittest.TestCase):
    def test_cleanup_is_lifo_idempotent_and_continues_after_error(self) -> None:
        calls: list[str] = []
        stack = RUNNER.CleanupStack()
        stack.push("a", lambda: calls.append("a"))

        def fail_b() -> None:
            calls.append("b")
            raise RuntimeError("private cleanup detail")

        stack.push("b", fail_b)
        stack.push("c", lambda: calls.append("c"))

        errors = stack.cleanup(time.monotonic() + 1.0)
        second_errors = stack.cleanup(time.monotonic() + 1.0)

        self.assertEqual(calls, ["c", "b", "a"])
        self.assertEqual([error.code for error in errors], ["cleanup_callback_failed"])
        self.assertEqual(second_errors, [])
        self.assertNotIn("private", repr(errors))

    def test_cleanup_deadline_does_not_start_more_callbacks(self) -> None:
        calls: list[str] = []
        stack = RUNNER.CleanupStack()
        stack.push("a", lambda: calls.append("a"))
        errors = stack.cleanup(time.monotonic() - 1.0)
        self.assertEqual(calls, [])
        self.assertEqual([error.code for error in errors], ["cleanup_timeout"])

    def test_cleanup_timeout_continues_with_unbounded_local_callbacks(self) -> None:
        calls: list[str] = []
        stack = RUNNER.CleanupStack()
        stack.push("local-release", lambda: calls.append("local"), timeout_required=False)

        def remote_timeout() -> None:
            calls.append("remote")
            raise RUNNER.RunnerDeadlineExceeded

        stack.push("remote-revoke", remote_timeout)
        errors = stack.cleanup(time.monotonic() + 1.0)
        self.assertEqual(calls, ["remote", "local"])
        self.assertEqual([error.code for error in errors], ["cleanup_timeout"])


class E2eRunnerTest(unittest.TestCase):
    def config(self, **changes: object) -> object:
        values = {
            "layer": "host",
            "journey": "lifecycle-example",
            "profile": "host",
            "hard_timeout_s": 1.0,
            "phase_timeout_s": 0.5,
            "cleanup_timeout_s": 0.2,
            "retries": 0,
        }
        values.update(changes)
        return RUNNER.RunnerConfig(**values)

    def test_failure_categories_have_stable_exit_codes(self) -> None:
        self.assertEqual(RUNNER.ExitCode.SUCCESS, 0)
        self.assertEqual(RUNNER.exit_code_for(RUNNER.FailureCategory.CONFIGURATION), 2)
        self.assertEqual(RUNNER.exit_code_for(RUNNER.FailureCategory.INFRASTRUCTURE), 10)
        self.assertEqual(RUNNER.exit_code_for(RUNNER.FailureCategory.PRODUCT), 20)
        self.assertEqual(RUNNER.exit_code_for(RUNNER.FailureCategory.DEVICE), 30)
        self.assertEqual(RUNNER.exit_code_for(RUNNER.FailureCategory.LEASE), 31)
        self.assertEqual(RUNNER.exit_code_for(RUNNER.FailureCategory.EXTERNAL), 40)
        self.assertEqual(RUNNER.exit_code_for(RUNNER.FailureCategory.TIMEOUT), 60)
        self.assertEqual(RUNNER.exit_code_for(RUNNER.FailureCategory.INTERRUPTED), 70)
        self.assertEqual(RUNNER.exit_code_for(RUNNER.FailureCategory.CLEANUP), 80)

    def test_config_rejects_invalid_values(self) -> None:
        for changes in (
            {"layer": "unknown"},
            {"journey": ""},
            {"profile": "UPPER"},
            {"hard_timeout_s": 0},
            {"phase_timeout_s": -1},
            {"cleanup_timeout_s": 0},
            {"hard_timeout_s": math.nan},
            {"phase_timeout_s": math.inf},
            {"cleanup_timeout_s": -math.inf},
            {"retries": 1},
        ):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.config(**changes)

    def test_success_runs_phases_in_order_and_always_cleans_up(self) -> None:
        adapter = SpyAdapter()
        result = RUNNER.run_e2e(self.config(), adapter)
        self.assertEqual(adapter.calls, ["prepare", "run", "assert", "collect"])
        self.assertEqual(adapter.resources, ["b", "a"])
        self.assertEqual(result.status, RUNNER.RunStatus.PASSED)
        self.assertIsNone(result.failure_category)
        self.assertEqual(result.exit_code, RUNNER.ExitCode.SUCCESS)
        self.assertRegex(result.run_id, r"^[0-9a-f]{32}$")
        self.assertRegex(result.correlation_id, r"^[0-9a-f]{32}$")
        self.assertNotEqual(result.run_id, result.correlation_id)

    def test_each_phase_failure_stops_business_phases_and_cleans_up(self) -> None:
        expected_calls = {
            "prepare": ["prepare"],
            "run": ["prepare", "run"],
            "assert": ["prepare", "run", "assert"],
            "collect": ["prepare", "run", "assert", "collect"],
        }
        for phase, calls in expected_calls.items():
            with self.subTest(phase=phase):
                adapter = SpyAdapter(fail_phase=phase)
                result = RUNNER.run_e2e(self.config(), adapter)
                self.assertEqual(adapter.calls, calls)
                self.assertEqual(result.status, RUNNER.RunStatus.FAILED)
                self.assertEqual(result.failure_category, RUNNER.FailureCategory.INFRASTRUCTURE)
                self.assertEqual(result.failed_phase, phase)
                self.assertEqual(result.message_code, f"{phase}_unexpected_error")
                self.assertNotIn("secret", repr(result))

    def test_controlled_product_failure_keeps_category_separate_from_phase(self) -> None:
        adapter = SpyAdapter(
            fail_phase="assert",
            failure=RUNNER.RunnerFailure(RUNNER.FailureCategory.PRODUCT, "journey_assertion_failed"),
        )
        result = RUNNER.run_e2e(self.config(), adapter)
        self.assertEqual(result.failure_category, RUNNER.FailureCategory.PRODUCT)
        self.assertEqual(result.failed_phase, "assert")
        self.assertEqual(result.exit_code, RUNNER.ExitCode.PRODUCT)

    def test_timeout_is_not_reported_as_assertion_failure(self) -> None:
        class BlockingAdapter(SpyAdapter):
            def run(self, context: object) -> dict[str, object]:
                self.calls.append("run")
                time.sleep(2.0)
                return {}

        started = time.monotonic()
        adapter = BlockingAdapter()
        result = RUNNER.run_e2e(self.config(hard_timeout_s=0.1, phase_timeout_s=0.1), adapter)
        self.assertLess(time.monotonic() - started, 1.0)
        self.assertEqual(result.failure_category, RUNNER.FailureCategory.TIMEOUT)
        self.assertEqual(result.failed_phase, "run")
        self.assertEqual(result.exit_code, RUNNER.ExitCode.TIMEOUT)
        self.assertEqual(adapter.resources, ["b", "a"])

    def test_keyboard_interrupt_is_classified_and_cleanup_runs(self) -> None:
        adapter = SpyAdapter(fail_phase="run", failure=KeyboardInterrupt())
        result = RUNNER.run_e2e(self.config(), adapter)
        self.assertEqual(result.failure_category, RUNNER.FailureCategory.INTERRUPTED)
        self.assertEqual(result.failed_phase, "run")
        self.assertEqual(result.exit_code, RUNNER.ExitCode.INTERRUPTED)
        self.assertEqual(adapter.resources, ["b", "a"])

    def test_cleanup_failure_overrides_success_without_leaking_detail(self) -> None:
        class CleanupFailureAdapter(SpyAdapter):
            def prepare(self, context: object) -> None:
                self.calls.append("prepare")

                def fail() -> None:
                    raise RuntimeError("token=must-not-leak")

                context.cleanup.push("private-name", fail)

        result = RUNNER.run_e2e(self.config(), CleanupFailureAdapter())
        self.assertEqual(result.failure_category, RUNNER.FailureCategory.CLEANUP)
        self.assertEqual(result.failed_phase, "cleanup")
        self.assertEqual(result.exit_code, RUNNER.ExitCode.CLEANUP)
        self.assertEqual([error.code for error in result.cleanup_errors], ["cleanup_callback_failed"])
        self.assertNotIn("token", repr(result))
        self.assertNotIn("private-name", repr(result))

    def test_cleanup_failure_preserves_primary_failure_for_evidence(self) -> None:
        class ProductAndCleanupFailureAdapter(SpyAdapter):
            def prepare(self, context: object) -> None:
                self.calls.append("prepare")
                context.cleanup.push("cleanup-failure", lambda: (_ for _ in ()).throw(RuntimeError("private")))

            def run(self, context: object) -> dict[str, object]:
                raise RUNNER.RunnerFailure(RUNNER.FailureCategory.PRODUCT, "journey_assertion_failed")

        result = RUNNER.run_e2e(self.config(), ProductAndCleanupFailureAdapter())
        self.assertEqual(result.failure_category, RUNNER.FailureCategory.CLEANUP)
        self.assertEqual(result.failed_phase, "cleanup")
        self.assertEqual(result.primary_failure_category, RUNNER.FailureCategory.PRODUCT)
        self.assertEqual(result.primary_failed_phase, "run")
        self.assertEqual(result.primary_message_code, "journey_assertion_failed")

    def test_first_cleanup_interrupt_is_classified_and_remaining_cleanup_runs(self) -> None:
        calls: list[str] = []

        class CleanupInterruptAdapter(SpyAdapter):
            def prepare(self, context: object) -> None:
                self.calls.append("prepare")
                context.cleanup.push("last", lambda: calls.append("last"))
                context.cleanup.push("interrupt", lambda: (_ for _ in ()).throw(KeyboardInterrupt()))

        result = RUNNER.run_e2e(self.config(), CleanupInterruptAdapter())
        self.assertEqual(result.failure_category, RUNNER.FailureCategory.INTERRUPTED)
        self.assertEqual(result.failed_phase, "cleanup")
        self.assertEqual(result.exit_code, RUNNER.ExitCode.INTERRUPTED)
        self.assertEqual(calls, ["last"])

    @unittest.skipUnless(hasattr(signal, "setitimer"), "requires POSIX interval timers")
    def test_deadline_guard_restores_existing_timer_with_elapsed_time_deducted(self) -> None:
        previous_handler = signal.getsignal(signal.SIGALRM)
        signal.signal(signal.SIGALRM, lambda signum, frame: None)
        signal.setitimer(signal.ITIMER_REAL, 0.3)
        try:
            with RUNNER._deadline_guard(0.2):
                time.sleep(0.05)
            remaining, _ = signal.getitimer(signal.ITIMER_REAL)
        finally:
            signal.setitimer(signal.ITIMER_REAL, 0)
            signal.signal(signal.SIGALRM, previous_handler)
        self.assertGreater(remaining, 0.15)
        self.assertLess(remaining, 0.28)

    def test_context_creation_failure_returns_safe_infrastructure_result(self) -> None:
        with mock.patch.object(RUNNER.tempfile, "mkdtemp", side_effect=OSError("/private/path token=secret")):
            result = RUNNER.run_e2e(self.config(), SpyAdapter())
        self.assertEqual(result.failure_category, RUNNER.FailureCategory.INFRASTRUCTURE)
        self.assertEqual(result.failed_phase, "prepare")
        self.assertEqual(result.message_code, "context_initialization_failed")
        self.assertEqual(result.exit_code, RUNNER.ExitCode.INFRASTRUCTURE)
        self.assertNotIn("private", repr(result))
        self.assertNotIn("secret", repr(result))

        results: list[object] = []

        def worker() -> None:
            results.append(RUNNER.run_e2e(self.config(), SpyAdapter()))

        thread = threading.Thread(target=worker)
        thread.start()
        thread.join(timeout=1.0)
        self.assertFalse(thread.is_alive())
        self.assertEqual(results[0].failure_category, RUNNER.FailureCategory.INFRASTRUCTURE)
        self.assertEqual(results[0].message_code, "runner_requires_main_thread")


if __name__ == "__main__":
    unittest.main()
