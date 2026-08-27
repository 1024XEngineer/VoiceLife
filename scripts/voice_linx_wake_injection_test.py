#!/usr/bin/env python3
"""Inject a wake phrase into SparkBot's local MultiNet detector over USB PCM."""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

from voice_linx_serial_multiturn_test import (
    PreparedTurn,
    SerialLog,
    open_serial,
    packet,
    reset_usb_serial_jtag,
    run_turn,
    synthesize,
    to_pcm_frames,
)

try:
    import dashscope
    import serial
    from dashscope.audio.tts_v2 import SpeechSynthesizer
except ImportError:
    dashscope = None
    serial = None
    SpeechSynthesizer = None

TURN_BEGIN, PCM, TURN_END = 1, 2, 3
WAKE_BEGIN, WAKE_END = 4, 5


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/cu.usbmodem14401")
    parser.add_argument("--baud", type=int, default=115200)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--text", default="你好牛牛", help="由百炼合成后注入的唤醒词。")
    source.add_argument("--input-audio", type=Path, help="本地音频文件；支持 ffmpeg 可解码的格式。")
    parser.add_argument(
        "--expected-word",
        help="期望本地 MultiNet 命中的显示词；默认使用 --text。",
    )
    parser.add_argument(
        "--followup-text",
        action="append",
        help="唤醒后继续注入一条或多条真实语音，并等待 Linx STT/TTS 完成。",
    )
    parser.add_argument("--tts-model", default="cosyvoice-v3-flash")
    parser.add_argument("--voice", default="longanhuan_v3")
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument(
        "--expect-detection",
        choices=("yes", "no"),
        default="yes",
        help="yes 要求命中；no 在观察窗口内出现命中即失败。",
    )
    parser.add_argument(
        "--negative-observation-seconds",
        type=float,
        default=4.0,
        help="--expect-detection=no 时，WAKE_END 后继续观察的时长。",
    )
    parser.add_argument(
        "--reset-before-run",
        action="store_true",
        help="Hard-reset the board after opening the serial port, then wait for its ready sequence.",
    )
    parser.add_argument("--serial-log", type=Path)
    args = parser.parse_args()
    if args.baud <= 0 or args.timeout <= 0 or args.negative_observation_seconds <= 0:
        parser.error("baud、timeout 和 negative-observation-seconds 必须为正数")
    if args.input_audio is not None and not args.input_audio.is_file():
        parser.error(f"input-audio 不存在或不是文件: {args.input_audio}")
    return args


def wait_for(log: SerialLog, marker: str, cursor: int, timeout: float) -> int:
    next_cursor, _ = log.wait_for(marker, cursor, timeout)
    return next_cursor


def request_wake_begin(log: SerialLog, device: serial.Serial, timeout: float) -> int:
    """Open the local wake-input gate without requiring a one-shot boot log."""
    deadline = time.monotonic() + timeout
    cursor = log.mark()
    # A USB-Serial/JTAG attach can reset SparkBot. In that case writes made
    # before the serial task starts are buffered and later replayed together.
    # Observe its one-shot startup marker opportunistically before sending;
    # after a bounded wait, an already-running board simply proceeds to the
    # request/response handshake below.
    try:
        cursor, _ = log.wait_for("SERIAL_VOICE_TEST_READY=1", cursor, min(8.0, timeout))
    except TimeoutError:
        cursor = log.mark()
    while time.monotonic() < deadline:
        device.write(packet(WAKE_BEGIN))
        device.flush()
        try:
            cursor, line = log.wait_for("SERIAL_VOICE_WAKE_BEGIN=", cursor, min(1.0, deadline - time.monotonic()))
        except TimeoutError:
            # The serial task may still be starting after an explicit USB reset.
            # Retry the idempotent request until the task acknowledges it.
            cursor = log.mark()
            continue
        if "=ok" in line:
            return cursor
        # Code 4 means the detector is still leaving an active interaction.
        # Wait for its explicit standby transition before issuing one retry;
        # repeatedly writing while booting would only queue duplicate requests
        # on the USB endpoint.
        if "code=4" in line:
            try:
                cursor, _ = log.wait_for(
                    "SERIAL_VOICE_EVIDENCE event=standby_ready ",
                    cursor,
                    min(5.0, max(0.0, deadline - time.monotonic())),
                )
            except TimeoutError:
                cursor = log.mark()
        else:
            time.sleep(0.25)
    raise TimeoutError("SERIAL_VOICE_WAKE_BEGIN=ok")


def main() -> int:
    args = parse_args()
    if serial is None:
        print("pyserial is required", file=sys.stderr)
        return 2
    try:
        if args.input_audio is not None:
            audio = args.input_audio.read_bytes()
            input_description = str(args.input_audio)
        else:
            api_key = os.environ.get("DASHSCOPE_API_KEY")
            if not api_key:
                print("DASHSCOPE_API_KEY is required with --text", file=sys.stderr)
                return 2
            if dashscope is None or SpeechSynthesizer is None:
                print("dashscope is required with --text", file=sys.stderr)
                return 2
            dashscope.api_key = api_key
            audio = synthesize(args.text, args.tts_model, args.voice)
            input_description = args.text
        frames = to_pcm_frames(audio)
        followups = []
        for text in args.followup_text or []:
            if dashscope is None or SpeechSynthesizer is None:
                print("dashscope is required with --followup-text", file=sys.stderr)
                return 2
            followup_audio = synthesize(text, args.tts_model, args.voice)
            followups.append(PreparedTurn(input_text=text, tts_ms=0, frames=to_pcm_frames(followup_audio)))
    except (RuntimeError, OSError) as error:
        print(f"input_preparation_failed:{error}", file=sys.stderr)
        return 2
    if not frames:
        print("input_preparation_failed:empty_pcm", file=sys.stderr)
        return 2

    try:
        device = open_serial(args.port, args.baud)
    except serial.SerialException as error:
        print(f"cannot open serial port: {type(error).__name__}", file=sys.stderr)
        return 2

    log = SerialLog(device)
    log.start()
    cursor = 0
    try:
        if args.reset_before_run:
            reset_usb_serial_jtag(device)
        # `SERIAL_VOICE_TEST_READY=1` is emitted once per firmware boot and can
        # precede a later serial attach. The request/response handshake is the
        # authoritative readiness check for both fresh and already-running
        # boards; it also verifies that local wake detection is in standby.
        cursor = request_wake_begin(log, device, args.timeout)

        for frame in frames:
            device.write(packet(PCM, frame))
            device.flush()
            time.sleep(0.02)
        # MultiNet can decide on the final PCM chunk before the host-side
        # WAKE_END acknowledgement is emitted. Keep the pre-END cursor for
        # detection lookup; starting at the END marker would misclassify an
        # otherwise valid early detection (and miss early false positives).
        detection_cursor = cursor
        device.write(packet(WAKE_END))
        device.flush()
        cursor = wait_for(log, "SERIAL_VOICE_WAKE_END=ok", cursor, 5)
        expected_word = args.expected_word or args.text
        if args.expect_detection == "no":
            try:
                log.wait_for("WAKE_DETECTED word=", detection_cursor, args.negative_observation_seconds)
            except TimeoutError:
                print(
                    f"wake_injection_no_detection_success input={input_description} "
                    f"frames={len(frames)} observation_s={args.negative_observation_seconds:g}"
                )
                return 0
            raise TimeoutError("unexpected_wake_detection")
        cursor = wait_for(log, f"WAKE_DETECTED word={expected_word}", detection_cursor, args.timeout)
        cursor = wait_for(log, "SERIAL_VOICE_EVIDENCE event=wake_detected ", cursor, 5)
        # Both Linx-compatible wake paths are valid: a silent detect opens the
        # capture immediately, while a deliberate confirmation speech first
        # emits ack -> tts.stop and only then opens the capture. The harness
        # must wait for either path instead of timing out before sending the
        # follow-up utterance.
        cursor, wake_protocol = log.wait_for_any(
            (
                "SERIAL_VOICE_EVIDENCE event=local_wake_detect_requested ",
                "SERIAL_VOICE_EVIDENCE event=local_wake_ack_requested ",
            ),
            cursor,
            args.timeout,
        )
        if "local_wake_ack_requested" in wake_protocol:
            cursor = wait_for(log, "SERIAL_VOICE_EVIDENCE event=tts_stopped ", cursor, args.timeout)
        cursor = wait_for(log, "SERIAL_VOICE_EVIDENCE event=capture_started ", cursor, args.timeout)
        if not followups:
            print(f"wake_injection_success input={input_description} frames={len(frames)}")
            return 0
        for index, prepared in enumerate(followups, start=1):
            result = run_turn(
                device,
                log,
                index,
                prepared,
                response_timeout=args.timeout,
                first_turn=False,
                expect_terminal=False,
                guard_observation_seconds=8.5,
            )
            if result.error:
                raise TimeoutError(f"followup_{index}:{result.error}")
            print(
                f"wake_followup_success index={index} input={prepared.input_text} "
                f"asr={result.asr_text} reply={result.reply_text}"
            )
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
