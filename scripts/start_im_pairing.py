#!/usr/bin/env python3
"""Start and observe one VoiceLife WeChat pairing session over physical USB."""

from __future__ import annotations

import argparse
import re
import time

PAIRING_READY = b"IM_PAIRING_READY=1"
RUNTIME_READY = b"IM_RUNTIME_READY=1"
CODE_PATTERN = re.compile(rb"\bIM_PAIRING_CODE=(\d{6}) expires_at=([^\s]+)")
STATUS_PATTERN = re.compile(
    rb"\bIM_PAIRING_STATUS=(pending|retrying|confirmed|expired|cancelled|not_found|timed_out|credential_rejected|failed)\b"
)
FAILURE_PATTERN = re.compile(rb"\bIM_PAIRING_FAILED code=(\d+)\b")
TERMINAL_STATUSES = frozenset(
    {"confirmed", "expired", "cancelled", "not_found", "timed_out", "credential_rejected", "failed"}
)


def trigger_payload(expires_in_minutes: int) -> bytes:
    """Build the fixed-size, credential-free VLP1 physical trigger frame."""
    if not 1 <= expires_in_minutes <= 10:
        raise ValueError("expires-in-minutes must be between 1 and 10")
    return b"VLP1" + bytes((expires_in_minutes,)) + b"\x00" * 7


def parse_pairing_line(line: bytes) -> dict[str, str] | None:
    """Extract only the explicitly sanitized pairing markers emitted by firmware."""
    code = CODE_PATTERN.search(line)
    if code:
        return {"code": code.group(1).decode("ascii"), "expires_at": code.group(2).decode("ascii")}
    status = STATUS_PATTERN.search(line)
    if status:
        return {"status": status.group(1).decode("ascii")}
    failure = FAILURE_PATTERN.search(line)
    if failure:
        return {"failure_code": failure.group(1).decode("ascii")}
    return None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="locally attached serial device")
    parser.add_argument("--expires-in-minutes", type=int, default=10, choices=range(1, 11))
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=720.0)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    payload = trigger_payload(args.expires_in_minutes)
    try:
        import serial
    except ImportError as error:
        raise SystemExit("pyserial is required; use the ESP-IDF Python environment") from error

    with serial.Serial(args.port, args.baud, timeout=0.2, write_timeout=2) as device:
        device.dtr = False
        device.rts = True
        time.sleep(0.15)
        device.rts = False
        deadline = time.monotonic() + args.timeout
        pairing_ready = False
        runtime_ready = False
        sent = False
        while time.monotonic() < deadline:
            line = device.readline()
            pairing_ready = pairing_ready or PAIRING_READY in line
            runtime_ready = runtime_ready or RUNTIME_READY in line
            if pairing_ready and runtime_ready and not sent:
                device.write(payload)
                device.flush()
                sent = True
                print("Pairing request sent; enter the six-digit code in the WeChat Official Account.")
                continue
            if not sent:
                continue
            result = parse_pairing_line(line)
            if result is None:
                continue
            if "code" in result:
                print(f"Pairing code: {result['code']} (expires at {result['expires_at']})")
                continue
            if "failure_code" in result:
                raise SystemExit(f"Board rejected pairing (sanitized status code {result['failure_code']}).")
            status = result["status"]
            print(f"Pairing status: {status}")
            if status in TERMINAL_STATUSES:
                if status != "confirmed":
                    raise SystemExit(f"Pairing stopped with status: {status}")
                return
    raise SystemExit("Timed out waiting for the board pairing flow")


if __name__ == "__main__":
    main()
