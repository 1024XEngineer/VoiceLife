from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "scripts"
CLI = SCRIPTS / "run_e2e.py"


def load_module(name: str, path: Path) -> object:
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


RUNNER = load_module("e2e_runner", SCRIPTS / "e2e_runner.py")
EVIDENCE = load_module("e2e_evidence", SCRIPTS / "e2e_evidence.py")
ADAPTERS = load_module("e2e_example_adapters", SCRIPTS / "e2e_example_adapters.py")
COLLECTOR = load_module("collect_linx_e2e_evidence", SCRIPTS / "collect_linx_e2e_evidence.py")
PAIRING = load_module("start_im_pairing", SCRIPTS / "start_im_pairing.py")
HIL_DEVICE = load_module("e2e_hil_device", SCRIPTS / "e2e_hil_device.py")
HIL_ADAPTERS = load_module("e2e_hil_adapters", SCRIPTS / "e2e_hil_adapters.py")
RUN_E2E = load_module("run_e2e", CLI)


class ExampleAdapterTest(unittest.TestCase):
    def config(self, layer: str, profile: str) -> object:
        return RUNNER.RunnerConfig(
            layer=layer,
            journey="lifecycle-example",
            profile=profile,
            hard_timeout_s=1.0,
            phase_timeout_s=0.5,
            cleanup_timeout_s=0.2,
        )

    def test_host_adapter_uses_random_port_namespace_and_temp_directory(self) -> None:
        first = ADAPTERS.HostLifecycleExampleAdapter()
        second = ADAPTERS.HostLifecycleExampleAdapter()
        first_result = RUNNER.run_e2e(self.config("host", "host"), first)
        second_result = RUNNER.run_e2e(self.config("host", "host"), second)
        self.assertEqual(first_result.exit_code, 0)
        self.assertEqual(second_result.exit_code, 0)
        self.assertNotEqual(first.observed_port, second.observed_port)
        self.assertNotEqual(first.observed_namespace, second.observed_namespace)
        self.assertNotEqual(first.observed_temp_directory, second.observed_temp_directory)
        self.assertFalse(first.observed_temp_directory.exists())
        self.assertFalse(second.observed_temp_directory.exists())
        self.assertEqual(first_result.collected["metrics"]["bound_port_count"], 1)

    def test_hil_adapter_uses_same_contract_but_never_claims_hardware(self) -> None:
        adapter = ADAPTERS.HilLifecycleExampleAdapter()
        result = RUNNER.run_e2e(self.config("hil", "sparkbot"), adapter)
        self.assertEqual(result.exit_code, 0)
        self.assertEqual([item.name for item in result.assertions], ["lifecycle_complete"])
        self.assertFalse(result.collected["hardware_verified"])
        self.assertEqual(result.collected["scope"], "runner_contract_only")
        self.assertFalse(adapter.lease_held)

    def test_hil_never_falls_back_to_host(self) -> None:
        with self.assertRaises(ValueError):
            RUN_E2E.build_adapter("hil", "unknown-journey")
        with self.assertRaises(ValueError):
            RUN_E2E.validated_profile("hil", "host")

    def test_recovery_journey_uses_dedicated_host_adapter(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            adapter = RUN_E2E.build_adapter("host", "im-gateway-recovery", Path(directory))
            self.assertIsInstance(adapter, ADAPTERS.HostImGatewayRecoveryE2EAdapter)
            self.assertEqual(adapter.artifact_directory, Path(directory))

    def test_recovery_adapter_classifies_process_failures_before_parsing_output(self) -> None:
        class CompletedProcess:
            returncode = 1

            def __init__(self, stderr: str) -> None:
                self.stderr = stderr

            def communicate(self, timeout: float) -> tuple[str, str]:
                del timeout
                return "", self.stderr

        class Context:
            def remaining(self) -> float:
                return 1.0

        for stderr in (
            "host_recovery_infrastructure_failed\n",
            "",
            "ERR_PNPM_RECURSIVE_EXEC_FIRST_FAIL build failed\n",
        ):
            adapter = ADAPTERS.HostImGatewayRecoveryE2EAdapter(Path("."))
            adapter._process = CompletedProcess(stderr)
            with self.subTest(stderr=stderr):
                with self.assertRaises(ADAPTERS.RunnerFailure) as raised:
                    adapter.run(Context())
                self.assertEqual(raised.exception.category, RUNNER.FailureCategory.INFRASTRUCTURE)
                self.assertEqual(raised.exception.message_code, "host_recovery_journey_failed")


class RunE2eCliTest(unittest.TestCase):
    def run_cli(self, *arguments: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(CLI), *arguments],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
            timeout=10,
            env=env,
        )

    def base_args(self, artifact_dir: Path, layer: str = "host", profile: str = "host") -> list[str]:
        return [
            "--layer",
            layer,
            "--journey",
            "lifecycle-example",
            "--profile",
            profile,
            "--artifact-dir",
            str(artifact_dir),
            "--timeout",
            "2",
            "--retries",
            "0",
        ]

    def test_host_and_hil_cli_write_valid_sanitized_evidence(self) -> None:
        for layer, profile in (("host", "host"), ("hil", "sparkbot")):
            with self.subTest(layer=layer), tempfile.TemporaryDirectory() as directory:
                result = self.run_cli(*self.base_args(Path(directory), layer, profile))
                self.assertEqual(result.returncode, 0, result.stderr)
                summary = json.loads(result.stdout)
                self.assertEqual(
                    set(summary), {"exit_code", "failure_category", "failed_phase", "message_code", "run_id"}
                )
                evidence_paths = list(Path(directory).glob("evidence-*.json"))
                self.assertEqual(len(evidence_paths), 1)
                document = json.loads(evidence_paths[0].read_text(encoding="utf-8"))
                EVIDENCE.validate_evidence(document)
                self.assertEqual(document["layer"], layer)
                self.assertFalse(document["hardware_verified"])
                self.assertNotIn(str(evidence_paths[0]), result.stdout)

    def test_contract_failure_writes_failed_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_cli(
                *self.base_args(Path(directory)), env={**os.environ, "VOICELIFE_E2E_CONTRACT_FAILURE": "1"}
            )
            self.assertEqual(result.returncode, 20, result.stderr)
            document = json.loads(next(Path(directory).glob("evidence-*.json")).read_text(encoding="utf-8"))
            EVIDENCE.validate_evidence(document)
            self.assertEqual(document["failure_category"], "product")
            self.assertEqual(document["failed_phase"], "assert")

    def test_cli_rejects_nonzero_retries_and_hil_host_profile(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            retry_args = self.base_args(Path(directory))
            retry_args[-1] = "1"
            retry = self.run_cli(*retry_args)
            self.assertEqual(retry.returncode, 2)
            self.assertEqual(retry.stdout, "")

            hil = self.run_cli(*self.base_args(Path(directory), "hil", "host"))
            self.assertEqual(hil.returncode, 2)
            self.assertEqual(hil.stdout, "")

    def test_cli_rejects_non_finite_timeout_as_configuration(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            for value in ("nan", "inf", "-inf"):
                arguments = self.base_args(Path(directory))
                arguments[arguments.index("--timeout") + 1] = value
                result = self.run_cli(*arguments)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, "")
                self.assertNotIn(value, result.stderr.lower())

    def test_cli_argument_errors_do_not_echo_sensitive_values(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            arguments = self.base_args(Path(directory))
            arguments[arguments.index("--layer") + 1] = "Authorization: Bearer canary"
            result = self.run_cli(*arguments)
            self.assertEqual(result.returncode, 2)
            self.assertNotIn("Authorization", result.stderr)
            self.assertNotIn("canary", result.stderr)

    def test_cli_rejects_hil_only_options_for_contract_examples(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            arguments = self.base_args(Path(directory)) + ["--server", "runner@example.test"]
            result = self.run_cli(*arguments)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, "")

    def test_failed_and_timeout_results_build_consistent_evidence(self) -> None:
        config = RUNNER.RunnerConfig(
            layer="host",
            journey="lifecycle-example",
            profile="host",
            hard_timeout_s=1.0,
            phase_timeout_s=0.5,
            cleanup_timeout_s=0.2,
        )

        class FailingAdapter(ADAPTERS.HostLifecycleExampleAdapter):
            def __init__(self, failure: BaseException) -> None:
                super().__init__()
                self.failure = failure

            def run(self, context: object) -> object:
                raise self.failure

        for failure, category, phase in (
            (
                RUNNER.RunnerFailure(RUNNER.FailureCategory.PRODUCT, "journey_assertion_failed"),
                "product",
                "run",
            ),
            (RUNNER.RunnerDeadlineExceeded(), "timeout", "run"),
        ):
            with self.subTest(category=category):
                result = RUNNER.run_e2e(config, FailingAdapter(failure))
                document = RUN_E2E.build_evidence(result, config)
                EVIDENCE.validate_evidence(document)
                self.assertEqual(document["failure_category"], category)
                self.assertEqual(document["failed_phase"], phase)
                stages = {stage["name"]: stage for stage in document["stages"]}
                self.assertEqual(stages[phase]["status"], "failed")
                self.assertEqual(stages["cleanup"]["status"], "passed")

    def test_primary_and_cleanup_failures_build_consistent_evidence(self) -> None:
        class FailingAdapter(ADAPTERS.HostLifecycleExampleAdapter):
            def prepare(self, context: object) -> None:
                context.cleanup.push("cleanup-failure", lambda: (_ for _ in ()).throw(RuntimeError("private")))

            def run(self, context: object) -> object:
                raise RUNNER.RunnerFailure(RUNNER.FailureCategory.PRODUCT, "journey_assertion_failed")

        config = RUNNER.RunnerConfig(
            layer="host",
            journey="lifecycle-example",
            profile="host",
            hard_timeout_s=1.0,
            phase_timeout_s=0.5,
            cleanup_timeout_s=0.2,
        )
        result = RUNNER.run_e2e(config, FailingAdapter())
        document = RUN_E2E.build_evidence(result, config)
        EVIDENCE.validate_evidence(document)
        self.assertEqual(document["failure_category"], "cleanup")
        self.assertEqual(document["failed_phase"], "cleanup")
        stages = {stage["name"]: stage for stage in document["stages"]}
        self.assertEqual(stages["run"]["status"], "failed")
        self.assertEqual(stages["run"]["code"], "journey_assertion_failed")
        self.assertEqual(stages["cleanup"]["status"], "failed")

    def test_hil_collect_then_cleanup_failure_downgrades_evidence_scope(self) -> None:
        class HilAdapter:
            def prepare(self, context: object) -> None:
                context.cleanup.push("cleanup-failure", lambda: (_ for _ in ()).throw(RuntimeError("private")))

            def run(self, context: object) -> dict[str, object]:
                return {"complete": True}

            def assert_result(self, context: object, result: object) -> list[object]:
                return []

            def collect(self, context: object, result: object, assertions: list[object]) -> dict[str, object]:
                return {
                    "scope": "hil_im_pairing",
                    "hardware_verified": True,
                    "firmware_sha256": "a" * 64,
                    "gateway_commit": "b" * 40,
                    "device_fingerprint": "c" * 16,
                    "readiness_markers": ["provisioned", "wifi_ready", "sntp_synced", "ready"],
                    "pairing_markers": ["scope_matched", "code_valid", "pending", "expired"],
                    "metrics": {"resource_count": 4},
                }

        config = RUNNER.RunnerConfig(
            layer="hil",
            journey="im-pairing",
            profile="pcb",
            hard_timeout_s=1.0,
            phase_timeout_s=0.5,
            cleanup_timeout_s=0.2,
        )
        result = RUNNER.run_e2e(config, HilAdapter())
        self.assertEqual(result.message_code, "cleanup_callback_failed")
        document = RUN_E2E.build_evidence(result, config)
        EVIDENCE.validate_evidence(document)
        self.assertEqual(document["scope"], "runner_contract_only")
        self.assertIsNone(document["hil"])

    def test_two_processes_run_in_parallel_without_artifact_collisions(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            command = [sys.executable, str(CLI), *self.base_args(Path(directory))]
            first = subprocess.Popen(command, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            second = subprocess.Popen(command, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            first_stdout, first_stderr = first.communicate(timeout=10)
            second_stdout, second_stderr = second.communicate(timeout=10)
            self.assertEqual(first.returncode, 0, first_stderr)
            self.assertEqual(second.returncode, 0, second_stderr)
            first_summary = json.loads(first_stdout)
            second_summary = json.loads(second_stdout)
            self.assertNotEqual(first_summary["run_id"], second_summary["run_id"])
            self.assertEqual(len(list(Path(directory).glob("evidence-*.json"))), 2)

    def test_artifact_write_failure_has_stable_safe_exit(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            artifact_file = Path(directory) / "not-a-directory"
            artifact_file.write_text("occupied", encoding="utf-8")
            result = self.run_cli(*self.base_args(artifact_file))
            self.assertEqual(result.returncode, 10)
            summary = json.loads(result.stdout)
            self.assertEqual(summary["failure_category"], "infrastructure")
            self.assertEqual(summary["failed_phase"], "collect")
            self.assertEqual(summary["message_code"], "evidence_write_failed")
            self.assertNotIn(str(artifact_file), result.stdout)

    def test_sensitive_environment_does_not_enter_output_or_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            env = dict(os.environ)
            env["CANARY_TOKEN"] = "must-not-appear"
            result = self.run_cli(*self.base_args(Path(directory)), env=env)
            self.assertEqual(result.returncode, 0, result.stderr)
            combined = result.stdout + result.stderr
            for evidence_path in Path(directory).glob("*.json"):
                combined += evidence_path.read_text(encoding="utf-8")
            self.assertNotIn("must-not-appear", combined)
            self.assertNotIn("CANARY_TOKEN", combined)

    def test_real_hil_pairing_requires_device_and_non_secret_gateway_options(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            arguments = self.base_args(Path(directory), "hil", "sparkbot")
            arguments[arguments.index("lifecycle-example")] = "im-pairing"
            result = self.run_cli(*arguments)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, "")
            self.assertNotIn(str(Path(directory)), result.stderr)

    def test_build_adapter_registers_real_hil_without_host_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            descriptor = Path(directory) / "device.json"
            descriptor.write_text(
                json.dumps({"schema_version": 1, "name": "bench-a", "port": "/dev/cu.test", "profile": "sparkbot"}),
                encoding="utf-8",
            )
            args = RUN_E2E.parse_args(
                [
                    "--layer",
                    "hil",
                    "--journey",
                    "im-pairing",
                    "--profile",
                    "sparkbot",
                    "--artifact-dir",
                    directory,
                    "--timeout",
                    "2",
                    "--device",
                    str(descriptor),
                    "--server",
                    "runner@example.test",
                    "--server-dir",
                    "/srv/voicelife",
                    "--gateway-origin",
                    "https://gateway.example.test",
                    "--user-id",
                    "user-test",
                ]
            )
            adapter = RUN_E2E.build_adapter(args.layer, args.journey, args)
        self.assertEqual(type(adapter).__name__, "HilPairingAdapter")
        with self.assertRaises(ValueError):
            RUN_E2E.build_adapter("host", "im-pairing", args)


if __name__ == "__main__":
    unittest.main()
