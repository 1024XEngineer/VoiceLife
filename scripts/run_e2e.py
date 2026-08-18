#!/usr/bin/env python3
"""Run one VoiceLife E2E lifecycle through a registered Host or HIL adapter."""

from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

from e2e_evidence import EvidenceValidationError, EvidenceWriteError, write_evidence
from e2e_example_adapters import HilLifecycleExampleAdapter, HostLifecycleExampleAdapter
from e2e_runner import ExitCode, FailureCategory, RunnerConfig, RunnerResult, exit_code_for, run_e2e

PROFILES = {"host": frozenset({"host"}), "hil": frozenset({"sparkbot", "pcb"})}


class SafeArgumentParser(argparse.ArgumentParser):
    """Avoid echoing attacker-controlled command-line values in public logs."""

    def error(self, message: str) -> None:
        raise ValueError("invalid command-line arguments")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = SafeArgumentParser(description=__doc__)
    parser.add_argument("--layer", choices=("host", "hil"), required=True)
    parser.add_argument("--journey", required=True)
    parser.add_argument("--profile", required=True)
    parser.add_argument("--artifact-dir", type=Path, required=True)
    parser.add_argument("--timeout", type=float, required=True)
    parser.add_argument("--retries", type=int, default=0)
    return parser.parse_args(argv)


def validated_profile(layer: str, profile: str) -> str:
    if profile not in PROFILES.get(layer, frozenset()):
        raise ValueError("profile is not valid for the selected layer")
    return profile


def build_adapter(layer: str, journey: str) -> object:
    if journey != "lifecycle-example":
        raise ValueError("unknown journey")
    if layer == "host":
        return HostLifecycleExampleAdapter()
    if layer == "hil":
        return HilLifecycleExampleAdapter()
    raise ValueError("unknown layer")


def _utc_timestamp(monotonic_value: float, reference_monotonic: float, reference_utc: float) -> str:
    timestamp = reference_utc + monotonic_value - reference_monotonic
    return datetime.fromtimestamp(timestamp, timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def build_evidence(result: RunnerResult, config: RunnerConfig) -> dict[str, object]:
    reference_monotonic = result.finished_monotonic
    reference_utc = datetime.now(timezone.utc).timestamp()
    failed_phase = result.failed_phase
    stage_failure_phase = result.primary_failed_phase or failed_phase
    stage_failure_code = result.primary_message_code if result.primary_failed_phase is not None else result.message_code
    stages = []
    failed_seen = False
    for phase in ("prepare", "run", "assert", "collect"):
        if failed_seen:
            stage_status, code = "skipped", "phase_skipped"
        elif phase == stage_failure_phase:
            stage_status, code = "failed", stage_failure_code
            failed_seen = True
        else:
            stage_status, code = "passed", "phase_passed"
        stages.append({"name": phase, "status": stage_status, "code": code})
    cleanup_failed = failed_phase == "cleanup"
    stages.append(
        {
            "name": "cleanup",
            "status": "failed" if cleanup_failed else "passed",
            "code": result.message_code if cleanup_failed else "phase_passed",
        }
    )
    collected = result.collected
    metrics = collected.get("metrics", {}) if isinstance(collected, dict) else {}
    return {
        "schema_version": 1,
        "run_id": result.run_id,
        "correlation_id": result.correlation_id,
        "scope": "runner_contract_only",
        "layer": config.layer,
        "journey": config.journey,
        "profile": config.profile,
        "started_at": _utc_timestamp(result.started_monotonic, reference_monotonic, reference_utc),
        "finished_at": _utc_timestamp(result.finished_monotonic, reference_monotonic, reference_utc),
        "duration_ms": max(0, int((result.finished_monotonic - result.started_monotonic) * 1000)),
        "status": result.status.value,
        "failure_category": result.failure_category.value if result.failure_category is not None else None,
        "failed_phase": result.failed_phase,
        "message_code": result.message_code,
        "stages": stages,
        "assertions": [
            {"name": assertion.name, "passed": assertion.passed, "code": assertion.code}
            for assertion in result.assertions
        ],
        "metrics": metrics,
        "cleanup": {
            "status": "failed" if result.cleanup_errors else "passed",
            "error_codes": [error.code for error in result.cleanup_errors],
        },
        "hardware_verified": False,
    }


def safe_summary(
    *,
    run_id: str,
    exit_code: int,
    failure_category: str | None,
    failed_phase: str | None,
    message_code: str,
) -> str:
    return json.dumps(
        {
            "exit_code": exit_code,
            "failure_category": failure_category,
            "failed_phase": failed_phase,
            "message_code": message_code,
            "run_id": run_id,
        },
        ensure_ascii=True,
        sort_keys=True,
        separators=(",", ":"),
    )


def _write_result_evidence(artifact_dir: Path, result: RunnerResult, config: RunnerConfig) -> None:
    artifact_dir.mkdir(parents=True, exist_ok=True)
    destination = artifact_dir / f"evidence-{result.run_id}.json"
    write_evidence(artifact_dir, destination, build_evidence(result, config))


def main(argv: list[str] | None = None) -> int:
    try:
        args = parse_args(argv)
        profile = validated_profile(args.layer, args.profile)
        config = RunnerConfig(
            layer=args.layer,
            journey=args.journey,
            profile=profile,
            hard_timeout_s=args.timeout,
            phase_timeout_s=args.timeout,
            cleanup_timeout_s=min(5.0, args.timeout),
            retries=args.retries,
        )
        adapter = build_adapter(args.layer, args.journey)
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return int(ExitCode.CONFIGURATION)

    result = run_e2e(config, adapter)
    exit_code = int(result.exit_code)
    category = result.failure_category.value if result.failure_category is not None else None
    failed_phase = result.failed_phase
    message_code = result.message_code
    try:
        _write_result_evidence(args.artifact_dir, result, config)
    except (EvidenceValidationError, EvidenceWriteError, OSError):
        exit_code = int(exit_code_for(FailureCategory.INFRASTRUCTURE))
        category = FailureCategory.INFRASTRUCTURE.value
        failed_phase = "collect"
        message_code = "evidence_write_failed"

    print(
        safe_summary(
            run_id=result.run_id,
            exit_code=exit_code,
            failure_category=category,
            failed_phase=failed_phase,
            message_code=message_code,
        )
    )
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
