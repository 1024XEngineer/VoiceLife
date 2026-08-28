#!/usr/bin/env python3
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
from schedule_im_e2e import ImHilAdapter, ImHostAdapter  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--layer", choices=("host", "hil"), required=True)
    parser.add_argument("--profile", choices=("host", "sparkbot", "pcb"), required=True)
    parser.add_argument("--artifact-dir", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--device", type=Path)
    parser.add_argument("--lease-dir", type=Path)
    args = parser.parse_args(argv)
    if args.layer == "host":
        if args.profile != "host":
            print("host layer requires --profile host", file=sys.stderr)
            return int(ExitCode.CONFIGURATION)
        adapter = ImHostAdapter(args.artifact_dir)
    else:
        if args.profile == "host" or args.device is None:
            print(
                "hil layer requires --profile sparkbot/pcb and --device",
                file=sys.stderr,
            )
            return int(ExitCode.CONFIGURATION)
        adapter = ImHilAdapter(
            args.artifact_dir,
            args.device,
            args.lease_dir or Path.home() / ".voicelife" / "hil-leases",
            args.profile,
        )
    config = RunnerConfig(
        args.layer,
        "schedule-im",
        args.profile,
        args.timeout,
        args.timeout,
        min(30.0, args.timeout),
    )
    result = run_e2e(config, adapter)
    args.artifact_dir.mkdir(parents=True, exist_ok=True)
    write_evidence(
        args.artifact_dir,
        args.artifact_dir / f"evidence-{result.run_id}.json",
        build_evidence(result, config),
    )
    print(
        json.dumps(
            {
                "run_id": result.run_id,
                "status": result.status.value,
                "message_code": result.message_code,
            },
            sort_keys=True,
        )
    )
    return int(result.exit_code)


if __name__ == "__main__":
    raise SystemExit(main())
