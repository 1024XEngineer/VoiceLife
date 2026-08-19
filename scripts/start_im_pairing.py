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
SCOPE_PATTERN = re.compile(rb"\bIM_PAIRING_SCOPE device_id=([^\s]+) user_id=(.*?)\r?\n?$")
TERMINAL_STATUSES = frozenset(
    {"confirmed", "expired", "cancelled", "not_found", "timed_out", "credential_rejected", "failed"}
)


class PairingLifecycleError(RuntimeError):
    """Sanitized pairing markers do not form the expected HIL lifecycle."""


class PairingLifecycle:
    """Validate scope, code, pending and expected expiry without retaining identities."""

    def __init__(self, expected_device_id: str, expected_user_id: str) -> None:
        self._expected_device_id = expected_device_id
        self._expected_user_id = expected_user_id
        self.public_markers: list[str] = []
        self.complete = False

    def observe(self, event: dict[str, str]) -> None:
        if self.complete:
            raise PairingLifecycleError("pairing marker appeared after expiry")
        if event.get("status") == "pending" and "pending" in self.public_markers:
            return
        expected = ("scope_matched", "code_valid", "pending", "expired")
        position = len(self.public_markers)
        if "failure_code" in event:
            raise PairingLifecycleError("pairing failure marker observed")
        if "device_id" in event:
            if (
                position != 0
                or event["device_id"] != self._expected_device_id
                or event["user_id"] != self._expected_user_id
            ):
                raise PairingLifecycleError("pairing scope did not match run identity")
            marker = "scope_matched"
        elif "code" in event:
            if position != 1 or re.fullmatch(r"\d{6}", event["code"]) is None or not event.get("expires_at"):
                raise PairingLifecycleError("pairing code marker is invalid or out of order")
            marker = "code_valid"
        elif event.get("status") == "pending":
            if position != 2:
                raise PairingLifecycleError("pairing pending marker is out of order")
            marker = "pending"
        elif "status" in event:
            if event["status"] != "expired" or position != 3:
                raise PairingLifecycleError("pairing terminal status is not expected expiry")
            marker = "expired"
            self.complete = True
        else:
            raise PairingLifecycleError("unknown pairing marker")
        if marker != expected[position]:
            raise PairingLifecycleError("pairing marker order is invalid")
        self.public_markers.append(marker)


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
    scope = SCOPE_PATTERN.search(line)
    if scope:
        try:
            return {
                "device_id": scope.group(1).decode("ascii"),
                "user_id": scope.group(2).decode("utf-8"),
            }
        except UnicodeDecodeError:
            return None
    return None


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="locally attached serial device")
    parser.add_argument("--expires-in-minutes", type=int, default=10, choices=range(1, 11))
    parser.add_argument(
        "--auth-smoke",
        action="store_true",
        help="require real device create/query authentication and treat expected one-minute expiry as success",
    )
    parser.add_argument("--expected-device-id", help="registered deviceId required by --auth-smoke")
    parser.add_argument("--expected-user-id", help="registered immutable userId required by --auth-smoke")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=720.0)
    args = parser.parse_args(argv)
    if args.auth_smoke and (not args.expected_device_id or not args.expected_user_id):
        parser.error("--auth-smoke requires --expected-device-id and --expected-user-id")
    return args


def main() -> None:
    args = parse_args()
    expires_in_minutes = 1 if args.auth_smoke else args.expires_in_minutes
    payload = trigger_payload(expires_in_minutes)
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
        saw_session = False
        saw_pending = False
        saw_matching_scope = False
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
                saw_session = True
                print(f"Pairing code: {result['code']} (expires at {result['expires_at']})")
                continue
            if "failure_code" in result:
                raise SystemExit(f"Board rejected pairing (sanitized status code {result['failure_code']}).")
            if "device_id" in result:
                if args.auth_smoke and (
                    result["device_id"] != args.expected_device_id or result["user_id"] != args.expected_user_id
                ):
                    raise SystemExit("Authentication smoke failed: deviceId/userId do not match the order record")
                saw_matching_scope = True
                continue
            status = result["status"]
            print(f"Pairing status: {status}")
            saw_pending = saw_pending or status == "pending"
            if args.auth_smoke:
                if status == "expired" and saw_session and saw_pending and saw_matching_scope:
                    print("Authentication smoke passed: matching session was created, queried, and expired.")
                    return
                if status in TERMINAL_STATUSES:
                    raise SystemExit(f"Authentication smoke failed with status: {status}")
                continue
            if status in TERMINAL_STATUSES:
                if status != "confirmed":
                    raise SystemExit(f"Pairing stopped with status: {status}")
                return
    raise SystemExit("Timed out waiting for the board pairing flow")


if __name__ == "__main__":
    main()
