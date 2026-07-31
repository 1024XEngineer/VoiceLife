#!/usr/bin/env python3
"""Capture a manual PCB voice-reminder run without changing Flash."""

from __future__ import annotations

import argparse
import time
from pathlib import Path

import serial  # type: ignore


def hard_reset(device: serial.Serial) -> None:
    device.setDTR(False)
    device.setRTS(True)
    time.sleep(0.12)
    device.setRTS(False)
    time.sleep(0.25)


def pulse_button(device: serial.Serial) -> None:
    device.setRTS(False)
    device.setDTR(True)
    time.sleep(0.35)
    device.setDTR(False)


def open_serial(port: str, baud: int) -> serial.Serial:
    device = serial.Serial()
    device.port = port
    device.baudrate = baud
    device.timeout = 0.15
    device.dsrdtr = False
    device.rtscts = False
    device.dtr = False
    device.rts = False
    device.open()
    return device


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/cu.usbmodem5A840116301")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=int, default=420)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--no-reset", action="store_true")
    parser.add_argument("--append", action="store_true")
    parser.add_argument("--pulse-on-start", action="store_true")
    parser.add_argument("--pulse-when-ready", action="store_true")
    args = parser.parse_args()

    args.log.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + args.seconds
    ready_reported = False

    log_mode = "ab" if args.append else "wb"
    with open_serial(args.port, args.baud) as device, args.log.open(log_mode) as log:
        if args.no_reset:
            device.setDTR(False)
            device.setRTS(False)
        else:
            hard_reset(device)
        if args.pulse_on_start:
            pulse_button(device)
        print("SERIAL_CAPTURE_STARTED", flush=True)
        transcript = ""
        while time.monotonic() < deadline:
            data = device.read(device.in_waiting or 1)
            if not data:
                continue
            log.write(data)
            log.flush()
            text = data.decode("utf-8", errors="replace")
            print(text, end="", flush=True)
            transcript = (transcript + text)[-8192:]
            if not ready_reported and (
                "State: activating -> idle" in transcript
                or "State: starting -> idle" in transcript
                or "State: connecting -> idle" in transcript
            ):
                ready_reported = True
                print("\nMANUAL_VOICE_READY", flush=True)
                if args.pulse_when_ready:
                    pulse_button(device)
                    print("MANUAL_VOICE_BUTTON_PULSED", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
