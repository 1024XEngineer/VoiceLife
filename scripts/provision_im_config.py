#!/usr/bin/env python3
"""Provision VoiceLife IM credentials into encrypted NVS over a local serial cable."""

from __future__ import annotations

import argparse
import getpass
import ipaddress
import re
import time
from urllib.parse import urlsplit

CREATE_MAGIC = b"VLI1"
OVERWRITE_MAGIC = b"VLI2"
READY_MARKER = b"IM_PROVISION_READY=1"
FIELD_LIMITS = (255, 128, 512, 128)
FAILURE_PATTERN = re.compile(rb"\bIM_PROVISION_FAILED code=(\d+)\b")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="locally attached serial device")
    parser.add_argument("--gateway-origin", required=True, help="HTTPS origin without path/query/fragment")
    parser.add_argument("--device-id", required=True, help="Gateway device identifier")
    parser.add_argument("--user-id", required=True, help="non-secret Gateway user reference")
    parser.add_argument(
        "--force",
        action="store_true",
        help="explicitly overwrite an existing board IM configuration over the physical USB link",
    )
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=80.0)
    return parser.parse_args()


def validate_gateway_origin(value: str) -> None:
    parsed = urlsplit(value)
    try:
        port = parsed.port
    except ValueError as error:
        raise ValueError("gateway origin contains an invalid port") from error
    hostname = parsed.hostname or ""
    if ":" in hostname:
        try:
            ipaddress.IPv6Address(hostname)
        except ipaddress.AddressValueError as error:
            raise ValueError("gateway origin contains an invalid IPv6 host") from error
        hostname_valid = True
    else:
        try:
            hostname.encode("ascii")
        except UnicodeEncodeError:
            hostname_valid = False
        else:
            labels = hostname.split(".")
            hostname_valid = 0 < len(hostname) <= 253 and all(
                0 < len(label) <= 63
                and label[0].isalnum()
                and label[-1].isalnum()
                and all(character.isalnum() or character == "-" for character in label)
                for label in labels
            )
    if (
        parsed.scheme != "https"
        or not hostname_valid
        or parsed.username is not None
        or parsed.password is not None
        or parsed.path
        or parsed.query
        or parsed.fragment
        or any(character.isspace() for character in value)
        or (port is None and parsed.netloc.endswith(":"))
    ):
        raise ValueError("gateway origin must be HTTPS and contain no path, userinfo, query, or fragment")


def request_payload(
    gateway_origin: str,
    device_id: str,
    device_token: str,
    user_id: str = "",
    *,
    allow_overwrite: bool = False,
) -> bytearray:
    validate_gateway_origin(gateway_origin)
    if not re.fullmatch(r"[A-Za-z0-9_-]{43}", device_token):
        raise ValueError("device token must be exactly 43 base64url characters")
    fields = tuple(value.encode("utf-8") for value in (gateway_origin, device_id, device_token, user_id))
    for index, (field, maximum) in enumerate(zip(fields, FIELD_LIMITS, strict=True)):
        minimum = 1
        if not minimum <= len(field) <= maximum:
            raise ValueError(f"field {index + 1} must encode to {minimum}..{maximum} bytes")
        credential = index in (1, 2)
        if b"\x00" in field or any(
            byte < 0x20 or byte == 0x7F or (credential and (byte <= 0x20 or byte >= 0x7F)) for byte in field
        ):
            raise ValueError(f"field {index + 1} contains a control character")
    header = b"".join(len(field).to_bytes(2, "big") for field in fields)
    magic = OVERWRITE_MAGIC if allow_overwrite else CREATE_MAGIC
    return bytearray(magic + header + b"".join(fields))


def provisioning_failure_code(line: bytes) -> int | None:
    match = FAILURE_PATTERN.search(line)
    return int(match.group(1)) if match else None


def main() -> None:
    args = parse_args()
    token = getpass.getpass("IM device token (hidden): ")
    payload = request_payload(
        args.gateway_origin,
        args.device_id,
        token,
        args.user_id,
        allow_overwrite=args.force,
    )
    token = ""
    try:
        import serial
    except ImportError as error:
        for index in range(len(payload)):
            payload[index] = 0
        raise SystemExit("pyserial is required; use the ESP-IDF Python environment") from error

    try:
        with serial.Serial(args.port, args.baud, timeout=0.2, write_timeout=2) as device:
            device.dtr = False
            device.rts = True
            time.sleep(0.15)
            device.rts = False
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                line = device.readline()
                if READY_MARKER not in line:
                    continue
                device.write(payload)
                device.flush()
                for index in range(len(payload)):
                    payload[index] = 0
                print("Provisioning request sent; waiting for a credential-free result.")
                while time.monotonic() < deadline:
                    line = device.readline()
                    if b"IM_PROVISIONED=1" in line:
                        print("Provisioning completed; the board is restarting.")
                        return
                    failure_code = provisioning_failure_code(line)
                    if failure_code is not None:
                        raise SystemExit(f"Board rejected provisioning (sanitized status code {failure_code}).")
                break
    finally:
        for index in range(len(payload)):
            payload[index] = 0
    raise SystemExit("Timed out waiting for the board IM provisioning window")


if __name__ == "__main__":
    main()
