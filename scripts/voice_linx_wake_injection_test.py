#!/usr/bin/env python3
"""Inject a real DashScope TTS wake phrase into SparkBot's local wake detector."""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

from voice_linx_serial_multiturn_test import SerialLog, packet, synthesize, to_pcm_frames

try:
    import dashscope
    import serial
    from dashscope.audio.tts_v2 import SpeechSynthesizer
except ImportError:
    dashscope = None
    serial = None
    SpeechSynthesizer = None

WAKE_BEGIN, PCM, WAKE_END = 4, 2, 5


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/cu.usbmodem14401")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--text", default="你好牛牛")
    parser.add_argument("--tts-model", default="cosyvoice-v3-flash")
    parser.add_argument("--voice", default="longanhuan_v3")
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument(
        "--reset-before-run",
        action="store_true",
        help="Hard-reset the board after opening the serial port, then wait for its ready sequence.",
    )
    parser.add_argument("--serial-log", type=Path)
    args = parser.parse_args()
    if args.baud <= 0 or args.timeout <= 0:
        parser.error("baud 和 timeout 必须为正数")
    return args


def wait_for(log: SerialLog, marker: str, cursor: int, timeout: float) -> int:
    next_cursor, _ = log.wait_for(marker, cursor, timeout)
    return next_cursor


def main() -> int:
    args = parse_args()
    api_key = os.environ.get("DASHSCOPE_API_KEY")
    if not api_key:
        print("DASHSCOPE_API_KEY is required", file=sys.stderr)
        return 2
    if serial is None or dashscope is None or SpeechSynthesizer is None:
        print("pyserial and dashscope are required", file=sys.stderr)
        return 2

    dashscope.api_key = api_key
    try:
        audio = synthesize(args.text, args.tts_model, args.voice)
        frames = to_pcm_frames(audio)
    except (RuntimeError, OSError) as error:
        print(f"input_preparation_failed:{error}", file=sys.stderr)
        return 2
    if not frames:
        print("input_preparation_failed:empty_pcm", file=sys.stderr)
        return 2

    device = serial.Serial()
    device.port = args.port
    device.baudrate = args.baud
    device.timeout = 0.2
    device.write_timeout = 5
    device.dtr = False
    device.rts = False
    try:
        device.open()
    except serial.SerialException as error:
        print(f"cannot open serial port: {type(error).__name__}", file=sys.stderr)
        return 2

    log = SerialLog(device)
    log.start()
    cursor = 0
    try:
        if args.reset_before_run:
            # USB-Serial/JTAG maps RTS to EN. Keep DTR deasserted so the board
            # resets into the application rather than the ROM downloader.
            device.rts = True
            time.sleep(0.12)
            device.rts = False
        cursor = wait_for(log, "SERIAL_VOICE_TEST_READY=1", cursor, args.timeout)
        cursor = wait_for(log, "SERIAL_VOICE_EVIDENCE event=standby_ready ", cursor, args.timeout)
        device.write(packet(WAKE_BEGIN))
        device.flush()
        cursor = wait_for(log, "SERIAL_VOICE_WAKE_BEGIN=ok", cursor, 5)

        for frame in frames:
            device.write(packet(PCM, frame))
            device.flush()
            time.sleep(0.02)
        device.write(packet(WAKE_END))
        device.flush()
        cursor = wait_for(log, "SERIAL_VOICE_WAKE_END=ok", cursor, 5)
        cursor = wait_for(log, "WAKE_DETECTED word=你好牛牛", cursor, args.timeout)
        cursor = wait_for(log, "SERIAL_VOICE_EVIDENCE event=wake_detected ", cursor, 5)
        cursor = wait_for(log, "SERIAL_VOICE_EVIDENCE event=local_wake_ack_requested ", cursor, args.timeout)
        cursor = wait_for(log, "SERIAL_VOICE_EVIDENCE event=tts_started ", cursor, args.timeout)
        cursor = wait_for(log, "SERIAL_VOICE_EVIDENCE event=capture_started ", cursor, args.timeout)
        print(f"wake_injection_success text={args.text} frames={len(frames)}")
        return 0
    except TimeoutError as error:
        print(f"wake_injection_timeout:{error}", file=sys.stderr)
        return 1
    except serial.SerialException as error:
        print(f"serial_error:{type(error).__name__}", file=sys.stderr)
        return 1
    finally:
        time.sleep(1)
        log.stop()
        device.close()
        if args.serial_log:
            args.serial_log.write_text("\n".join(log.all_lines()) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
