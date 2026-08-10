#!/usr/bin/env python3
"""Collect one sanitized Linx voice-flow evidence record from a local serial port."""

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
LABEL_PATTERN = re.compile(r"^[a-z0-9][a-z0-9_-]{0,63}$")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="locally attached serial device")
    parser.add_argument("--evidence", required=True, type=Path, help="destination JSON file")
    parser.add_argument("--label", required=True, help="non-sensitive scenario label, for example create-1")
    parser.add_argument("--expect", action="append", default=[], help="required lifecycle event; repeat as needed")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=90.0)
    return parser.parse_args()


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


def validate_label(label: str) -> str:
    if not LABEL_PATTERN.fullmatch(label):
        raise ValueError("label must be 1..64 lowercase ASCII letters, digits, _ or -")
    return label


def write_evidence(path: Path, label: str, expected: list[str], events: list[dict[str, int | str]]) -> None:
    seen = {str(event["event"]) for event in events}
    document = {
        "schema_version": 1,
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "label": validate_label(label),
        "expected_events": expected,
        "all_expected_events_seen": set(expected).issubset(seen),
        "events": events,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    try:
        validate_label(args.label)
        import serial
    except ImportError as error:
        raise SystemExit("pyserial is required; use the ESP-IDF Python environment") from error
    except ValueError as error:
        raise SystemExit(str(error)) from error

    events: list[dict[str, int | str]] = []
    deadline = time.monotonic() + args.timeout
    with serial.Serial(args.port, args.baud, timeout=0.2) as device:
        while time.monotonic() < deadline:
            event = parse_voice_event(device.readline().decode("utf-8", "replace"))
            if event is not None:
                events.append(event)

    write_evidence(args.evidence, args.label, args.expect, events)
    seen = {str(event["event"]) for event in events}
    missing = sorted(set(args.expect) - seen)
    if missing:
        print(f"Evidence recorded, but expected events were missing: {', '.join(missing)}")
        return 1
    print("Sanitized Linx voice-flow evidence recorded.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
