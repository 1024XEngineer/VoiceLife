#!/usr/bin/env python3
"""Provision Wi-Fi into the board's encrypted Linx NVS partition over local serial."""

from __future__ import annotations

import argparse
import getpass
import time

MAGIC = b"VLW1"
READY_MARKER = b"LINX_WIFI_PROVISION_READY=1"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="locally attached serial device")
    parser.add_argument("--ssid", required=True, help="Wi-Fi SSID; it is never persisted by this tool")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=50.0)
    return parser.parse_args()


def request_payload(ssid: str, password: str) -> bytes:
    ssid_bytes = ssid.encode("utf-8")
    password_bytes = password.encode("utf-8")
    if not 1 <= len(ssid_bytes) <= 32:
        raise ValueError("SSID must encode to 1..32 bytes")
    if not 1 <= len(password_bytes) <= 64:
        raise ValueError("password must encode to 1..64 bytes")
    return MAGIC + bytes((len(ssid_bytes), len(password_bytes))) + ssid_bytes + password_bytes


def main() -> None:
    args = parse_args()
    password = getpass.getpass("Wi-Fi password (hidden): ")
    payload = request_payload(args.ssid, password)
    password = ""
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
        observed: list[str] = []
        safe_markers = (
            b"LINX_WIFI_PROVISIONED=1",
            b"WIFI_STA_GOT_IP=1",
            b"WIFI_STA_DISCONNECTED",
            b"STARTUP_ERROR",
            b"NVS",
            b"LINX_WIFI_PROVISION_RESULT=",
            "保存 Wi-Fi 加密凭据".encode(),
        )
        while time.monotonic() < deadline:
            line = device.readline()
            if line:
                # Keep provisioning diagnostics credential-free. These markers make
                # encryption, association, and startup failures distinguishable
                # without forwarding arbitrary serial output or Wi-Fi secrets.
                for marker in safe_markers:
                    if marker in line:
                        text = line.decode("utf-8", "replace").strip()
                        if text not in observed:
                            observed.append(text)
                            print(text)
            if READY_MARKER not in line:
                continue
            device.write(payload)
            device.flush()
            payload = b""
            print("Provisioning request sent; waiting for a credential-free result.")
            while time.monotonic() < deadline:
                line = device.readline()
                if line and any(marker in line for marker in safe_markers):
                    text = line.decode("utf-8", "replace").strip()
                    if text not in observed:
                        observed.append(text)
                        print(text)
                if b"LINX_WIFI_PROVISIONED=1" in line:
                    print("Provisioning completed.")
                    return
                if b"\xe5\x90\xaf\xe5\x8a\xa8\xe5\xa4\xb1\xe8\xb4\xa5" in line:
                    raise SystemExit("Board rejected provisioning; inspect sanitized serial status.")
            break
    detail = "; ".join(observed[-4:])
    raise SystemExit("Timed out waiting for the board provisioning window" + (f": {detail}" if detail else ""))


if __name__ == "__main__":
    main()
