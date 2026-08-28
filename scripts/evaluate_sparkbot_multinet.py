#!/usr/bin/env python3
"""Run a repeatable MultiNet real-board evaluation from an external audio dataset."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import subprocess
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--dataset", type=Path, required=True, help="含 manifest.json 与 normalized-eval-wav 的数据集目录。"
    )
    parser.add_argument("--port", default="/dev/cu.usbmodem14201")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--runs", type=int, default=1, help="每条语料独立复位后重复次数。")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--negative-observation-seconds", type=float, default=4.0)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    if args.runs <= 0 or args.baud <= 0 or args.timeout <= 0 or args.negative_observation_seconds <= 0:
        parser.error("runs、baud、timeout 和 negative-observation-seconds 必须为正数")
    return args


def wav_path(dataset: Path, source_name: str) -> Path:
    return dataset / "normalized-eval-wav" / (Path(source_name).stem + ".wav")


def main() -> int:
    args = parse_args()
    manifest_path = args.dataset / "manifest.json"
    if not manifest_path.is_file():
        print(f"dataset manifest missing: {manifest_path}", file=sys.stderr)
        return 2
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    samples = manifest.get("samples")
    if not isinstance(samples, list) or not samples:
        print("dataset manifest has no samples", file=sys.stderr)
        return 2

    wake_script = Path(__file__).with_name("voice_linx_wake_injection_test.py")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    results: list[dict[str, object]] = []
    for run in range(1, args.runs + 1):
        for sample in samples:
            if not isinstance(sample, dict):
                print("dataset sample is not an object", file=sys.stderr)
                return 2
            name = sample.get("file")
            label = sample.get("label")
            if not isinstance(name, str) or label not in ("positive", "near_negative"):
                print(f"unsupported dataset sample: {sample}", file=sys.stderr)
                return 2
            audio = wav_path(args.dataset, name)
            if not audio.is_file():
                print(f"normalized wav missing: {audio}", file=sys.stderr)
                return 2
            expect_detection = "yes" if label == "positive" else "no"
            serial_log = args.output_dir / f"run{run:02d}-{Path(name).stem}.log"
            command = [
                sys.executable,
                str(wake_script),
                "--port",
                args.port,
                "--baud",
                str(args.baud),
                "--input-audio",
                str(audio),
                "--expect-detection",
                expect_detection,
                "--timeout",
                str(args.timeout),
                "--negative-observation-seconds",
                str(args.negative_observation_seconds),
                "--reset-before-run",
                "--serial-log",
                str(serial_log),
            ]
            completed = subprocess.run(command, text=True, capture_output=True, check=False)
            result = {
                "run": run,
                "file": name,
                "label": label,
                "text": sample.get("text"),
                "voice": sample.get("voice"),
                "speech_rate": sample.get("speech_rate"),
                "pitch_rate": sample.get("pitch_rate"),
                "expected_detection": expect_detection == "yes",
                "passed": completed.returncode == 0,
                "returncode": completed.returncode,
                "stdout": completed.stdout,
                "stderr": completed.stderr,
                "serial_log": str(serial_log),
            }
            results.append(result)
            state = "PASS" if result["passed"] else "FAIL"
            print(f"[{state}] run={run} label={label} file={name}", flush=True)

    positives = [item for item in results if item["label"] == "positive"]
    negatives = [item for item in results if item["label"] == "near_negative"]
    report = {
        "created_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "dataset": str(args.dataset),
        "port": args.port,
        "runs": args.runs,
        "total": len(results),
        "passed": sum(item["passed"] for item in results),
        "positive_total": len(positives),
        "positive_passed": sum(item["passed"] for item in positives),
        "near_negative_total": len(negatives),
        "near_negative_passed": sum(item["passed"] for item in negatives),
        "results": results,
    }
    report_path = args.output_dir / "multinet-evaluation.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"report={report_path}")
    return 0 if report["passed"] == report["total"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
