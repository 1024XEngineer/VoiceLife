#!/usr/bin/env python3
"""Run Issue #350 Host or ESP32-S3 voice/schedule E2E journey."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from e2e_evidence import write_evidence  # noqa: E402
from e2e_runner import ExitCode, RunnerConfig, run_e2e  # noqa: E402
from run_e2e import build_evidence  # noqa: E402
from schedule_voice_e2e import (  # noqa: E402
    SCENARIOS,
    ScheduleVoiceHilAdapter,
    ScheduleVoiceHostAdapter,
)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--layer", choices=("host", "hil"), required=True)
    parser.add_argument("--profile", choices=("host", "sparkbot", "pcb"), required=True)
    parser.add_argument("--artifact-dir", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--scenario", choices=SCENARIOS, default="create-query")
    parser.add_argument("--device", type=Path)
    parser.add_argument("--lease-dir", type=Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.layer == "host" and args.profile != "host":
        print("host layer requires --profile host", file=sys.stderr)
        return int(ExitCode.CONFIGURATION)
    if args.layer == "hil" and (args.profile == "host" or args.device is None):
        print("hil layer requires --profile sparkbot/pcb and --device", file=sys.stderr)
        return int(ExitCode.CONFIGURATION)
    if args.timeout <= 0:
        print("timeout must be positive", file=sys.stderr)
        return int(ExitCode.CONFIGURATION)

    if args.layer == "host":
        adapter = ScheduleVoiceHostAdapter(args.artifact_dir)
    else:
        adapter = ScheduleVoiceHilAdapter(
            args.artifact_dir,
            args.device,
            args.lease_dir or Path.home() / ".voicelife" / "hil-leases",
            args.profile,
            args.scenario,
        )
    config = RunnerConfig(
        layer=args.layer,
        journey="schedule-voice",
        profile=args.profile,
        hard_timeout_s=args.timeout,
        phase_timeout_s=args.timeout,
        cleanup_timeout_s=min(30.0, args.timeout),
    )
    result = run_e2e(config, adapter)
    args.artifact_dir.mkdir(parents=True, exist_ok=True)
    evidence_path = args.artifact_dir / f"evidence-{result.run_id}.json"
    write_evidence(args.artifact_dir, evidence_path, build_evidence(result, config))
    print(
        json.dumps(
            {
                "run_id": result.run_id,
                "status": result.status.value,
                "failure_category": result.failure_category.value if result.failure_category else None,
                "message_code": result.message_code,
            },
            sort_keys=True,
        )
    )
    return int(result.exit_code)


if __name__ == "__main__":
    raise SystemExit(main())
