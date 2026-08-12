#!/usr/bin/env python3
"""Collect one sanitized Linx voice-flow and IM readiness evidence record from a serial port."""

from __future__ import annotations

import argparse
import json
import re
import time
from datetime import datetime, timezone
from pathlib import Path

EVENT_PATTERN = re.compile(
    r"VOICE_EVENT\b.*?\bgeneration=(?P<generation>\d+)\b.*?\bevent=(?P<event>[a-z_]+)\b"
    r".*?\bdetail_present=(?P<detail_present>[01])\b.*?\blatency_from_capture_ms=(?P<latency_ms>\d+)\b"
    r".*?\baudio_captured=(?P<audio_captured>\d+)\b.*?\baudio_dropped=(?P<audio_dropped>\d+)\b"
    r".*?\baudio_played=(?P<audio_played>\d+)\b.*?\baudio_rejected=(?P<audio_rejected>\d+)\b"
    r".*?\bmin_heap=(?P<min_heap>\d+)\b"
)
# IM 生命周期标记：只提取脱敏状态与数字，绝不透传原始行。
IM_MARKER_PATTERN = re.compile(
    r"(IM_RUNTIME_READY|IM_RUNTIME_DEGRADED|IM_PROVISION_READY|IM_PROVISIONED|SNTP_SYNCED)=1\b"
)
# parse_im_signal 产出的短信号名中，属于「IM 未就绪」的失败信号。
IM_FAILURE_SIGNALS = frozenset({"degraded", "failure"})
LABEL_PATTERN = re.compile(r"^[a-z0-9][a-z0-9_-]{0,63}$")
FAILURE_EVENTS = frozenset(
    {"provider_error", "capture_stop_failed", "tts_capture_stop_failed", "stop_disconnect_failed"}
)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="locally attached serial device")
    parser.add_argument("--evidence", required=True, type=Path, help="destination JSON file")
    parser.add_argument("--label", required=True, help="non-sensitive scenario label, for example create-1")
    parser.add_argument("--expect", action="append", default=[], help="required lifecycle event; repeat as needed")
    parser.add_argument(
        "--require-im-ready",
        action="store_true",
        help="require IM_RUNTIME_READY=1 and reject IM failure markers in the recorded evidence",
    )
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=90.0)
    return parser.parse_args(argv)


def parse_voice_event(line: str) -> dict[str, int | str] | None:
    """Extract only allowlisted numeric metrics from one Runtime lifecycle line."""
    match = EVENT_PATTERN.search(line)
    if match is None:
        return None
    values: dict[str, int | str] = {"event": match.group("event")}
    for name, value in match.groupdict().items():
        if name != "event":
            values[name] = int(value)
    return values


def parse_im_signal(line: str) -> dict[str, int | str] | None:
    """Extract a sanitized IM lifecycle signal from one Runtime line."""
    match = IM_MARKER_PATTERN.search(line)
    if match is None:
        return None
    marker = match.group(1)
    if marker == "IM_RUNTIME_READY":
        return {"signal": "ready"}
    if marker == "IM_RUNTIME_DEGRADED":
        state = re.search(r"\bstate=(\d+)", line)
        code = re.search(r"\bcode=(\d+)", line)
        result: dict[str, int | str] = {"signal": "degraded"}
        if state is not None:
            result["state"] = int(state.group(1))
        if code is not None:
            result["code"] = int(code.group(1))
        return result
    if marker == "IM_PROVISION_READY":
        return {"signal": "provisioning"}
    if marker == "IM_PROVISIONED":
        return {"signal": "provisioned"}
    if marker == "SNTP_SYNCED":
        return {"signal": "sntp_synced"}
    return None


def im_readiness_status(signals: list[dict[str, int | str]]) -> tuple[bool, str]:
    """Require the latest terminal IM lifecycle signal to be ready."""
    if not signals:
        return False, "no IM readiness signal observed"
    terminal = [str(signal["signal"]) for signal in signals if str(signal["signal"]) in IM_FAILURE_SIGNALS | {"ready"}]
    if not terminal:
        return False, "IM ready signal (IM_RUNTIME_READY=1) not observed"
    if terminal[-1] != "ready":
        return False, f"latest IM terminal signal is not ready: {terminal[-1]}"
    return True, ""


def validate_label(label: str) -> str:
    if not LABEL_PATTERN.fullmatch(label):
        raise ValueError("label must be 1..64 lowercase ASCII letters, digits, _ or -")
    return label


def validate_event_sequence(events: list[dict[str, int | str]], expected: list[str]) -> tuple[bool, str]:
    """Require expected events as an ordered subsequence and reject failures."""
    observed = [str(event["event"]) for event in events]
    failures = sorted(set(observed) & FAILURE_EVENTS)
    if failures:
        return False, f"failure lifecycle event observed: {', '.join(failures)}"
    position = 0
    for required in expected:
        try:
            position = observed.index(required, position) + 1
        except ValueError:
            return False, f"ordered lifecycle event missing: {required}"
    return True, ""


def write_evidence(
    path: Path,
    label: str,
    expected: list[str],
    events: list[dict[str, int | str]],
    im_signals: list[dict[str, int | str]] | None = None,
) -> None:
    seen = {str(event["event"]) for event in events}
    sequence_valid, sequence_error = validate_event_sequence(events, expected)
    im_signals = im_signals or []
    im_ready_valid, im_ready_error = im_readiness_status(im_signals)
    document = {
        "schema_version": 2,
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "label": validate_label(label),
        "expected_events": expected,
        "all_expected_events_seen": set(expected).issubset(seen),
        "ordered_lifecycle_valid": sequence_valid,
        "validation_error": sequence_error,
        "events": events,
        "im_signals": im_signals,
        "im_ready_seen": any(str(signal["signal"]) == "ready" for signal in im_signals),
        "im_failure_observed": bool({str(signal["signal"]) for signal in im_signals} & IM_FAILURE_SIGNALS),
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        validate_label(args.label)
        import serial
    except ImportError as error:
        raise SystemExit("pyserial is required; use the ESP-IDF Python environment") from error
    except ValueError as error:
        raise SystemExit(str(error)) from error

    events: list[dict[str, int | str]] = []
    im_signals: list[dict[str, int | str]] = []
    deadline = time.monotonic() + args.timeout
    with serial.Serial(args.port, args.baud, timeout=0.2) as device:
        while time.monotonic() < deadline:
            line = device.readline().decode("utf-8", "replace")
            event = parse_voice_event(line)
            if event is not None:
                events.append(event)
            im_signal = parse_im_signal(line)
            if im_signal is not None:
                im_signals.append(im_signal)

    write_evidence(args.evidence, args.label, args.expect, events, im_signals)
    seen = {str(event["event"]) for event in events}
    missing = sorted(set(args.expect) - seen)
    if missing:
        print(f"Evidence recorded, but expected events were missing: {', '.join(missing)}")
        return 1
    sequence_valid, sequence_error = validate_event_sequence(events, args.expect)
    if not sequence_valid:
        print(f"Evidence recorded, but lifecycle validation failed: {sequence_error}")
        return 1
    if args.require_im_ready:
        ready_valid, ready_error = im_readiness_status(im_signals)
        if not ready_valid:
            print(f"Evidence recorded, but IM readiness validation failed: {ready_error}")
            return 1
    print("Sanitized Linx voice-flow and IM readiness evidence recorded.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
