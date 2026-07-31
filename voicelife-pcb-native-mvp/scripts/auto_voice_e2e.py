#!/usr/bin/env python3
"""Run the PCB VoiceLife voice-to-reminder smoke test without button input.

The board's USB CDC control lines are wired to the same reset/button circuit as
the physical BOOT key.  A short DTR pulse therefore starts an interactive turn
without changing the firmware.  The Mac's built-in speaker supplies the test
phrase and the original audio device is restored before the script exits.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path
import subprocess
import time

import serial  # type: ignore


DEFAULT_PORT = "/dev/cu.usbmodem5A840116301"
DEFAULT_PHRASE = "两分钟后提醒我喝水"
DEFAULT_VOICE = "Tingting"
DEFAULT_EVIDENCE = Path(__file__).resolve().parents[1] / "test-evidence"


def command_output(*args: str) -> str:
    return subprocess.check_output(args, text=True).strip()


def switch_audio(device: str, audio_type: str) -> None:
    subprocess.run(
        ["SwitchAudioSource", "-t", audio_type, "-s", device],
        check=True,
        stdout=subprocess.DEVNULL,
    )


def set_volume(value: int) -> None:
    subprocess.run(
        ["osascript", "-e", f"set volume output volume {value}"],
        check=True,
        stdout=subprocess.DEVNULL,
    )


def append_bytes(log, data: bytes, transcript: list[str]) -> None:
    if not data:
        return
    log.write(data)
    log.flush()
    transcript.append(data.decode("utf-8", errors="replace"))


def read_until(
    device,
    log,
    transcript: list[str],
    predicate,
    deadline: float,
) -> bool:
    while time.monotonic() < deadline:
        data = device.read(device.in_waiting or 1)
        append_bytes(log, data, transcript)
        if predicate("".join(transcript)):
            return True
    return predicate("".join(transcript))


def pulse_boot_button(device) -> None:
    # DTR=True pulls GPIO0 low on this USB reset circuit.  Keep EN high so this
    # is a button click rather than a reset.
    device.setRTS(False)
    device.setDTR(True)
    time.sleep(0.35)
    device.setDTR(False)


def hard_reset(device) -> None:
    device.setDTR(False)
    device.setRTS(True)
    time.sleep(0.12)
    device.setRTS(False)
    time.sleep(0.25)


def open_serial(port: str):
    device = serial.Serial()
    device.port = port
    device.baudrate = 115200
    device.timeout = 0.15
    device.dsrdtr = False
    device.rtscts = False
    # Configure both control lines before open(). Some USB-UART drivers apply
    # their defaults immediately and otherwise reset the ESP32 into ROM mode.
    device.dtr = False
    device.rts = False
    device.open()
    return device


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--phrase", default=DEFAULT_PHRASE)
    parser.add_argument("--voice", default=DEFAULT_VOICE)
    parser.add_argument("--rate", type=int, default=155)
    parser.add_argument("--listen-timeout", type=int, default=45)
    parser.add_argument("--overall-timeout", type=int, default=330)
    parser.add_argument("--evidence-dir", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S-auto-voice-e2e")
    evidence = args.evidence_dir or DEFAULT_EVIDENCE / stamp
    evidence.mkdir(parents=True, exist_ok=True)
    serial_log_path = evidence / "serial.log"
    manifest_path = evidence / "manifest.json"

    original_output = command_output("SwitchAudioSource", "-c", "-t", "output")
    original_system = command_output("SwitchAudioSource", "-c", "-t", "system")
    original_volume = int(command_output("osascript", "-e", "output volume of (get volume settings)"))
    transcript: list[str] = []
    say_process = None
    result = "fail"
    error = None
    device = None

    try:
        switch_audio("MacBook Pro扬声器", "output")
        switch_audio("MacBook Pro扬声器", "system")
        set_volume(78)

        device = open_serial(args.port)
        with serial_log_path.open("wb") as log:
            hard_reset(device)
            idle_seen = read_until(
                device,
                log,
                transcript,
                lambda text: "State: activating -> idle" in text or "State: starting -> idle" in text,
                time.monotonic() + args.listen_timeout,
            )
            # Activation can finish just before the USB console is reopened. A
            # short settle period avoids treating a missed boot log as failure.
            if not idle_seen:
                time.sleep(2)
            pulse_boot_button(device)
            listening_seen = read_until(
                device,
                log,
                transcript,
                lambda text: "State: connecting -> listening" in text,
                time.monotonic() + 30,
            )
            if not listening_seen:
                raise RuntimeError("USB BOOT pulse did not enter listening state")

            say_process = subprocess.Popen(
                ["say", "-v", args.voice, "-r", str(args.rate), args.phrase],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
            )

            interactive_deadline = time.monotonic() + 90
            closed_turn = False
            while time.monotonic() < interactive_deadline:
                data = device.read(device.in_waiting or 1)
                append_bytes(log, data, transcript)
                text = "".join(transcript)
                if not closed_turn and (
                    "State: speaking -> listening" in text
                    or "Application: << " in text
                    and "State: listening -> speaking" in text
                ):
                    time.sleep(1)
                    pulse_boot_button(device)
                    closed_turn = True
                if say_process.poll() is not None and "State: listening -> idle" in text:
                    closed_turn = True
                    break

            if say_process.poll() is None:
                say_process.terminate()
                say_process.wait(timeout=5)
            elif say_process.returncode:
                raise RuntimeError(f"say failed with exit code {say_process.returncode}")

            # Keep the same session open long enough for a two-minute reminder
            # to be delivered; if it is still listening, stop it once.
            if not closed_turn:
                pulse_boot_button(device)
            overall_deadline = time.monotonic() + max(0, args.overall_timeout - 90)
            while time.monotonic() < overall_deadline:
                data = device.read(device.in_waiting or 1)
                append_bytes(log, data, transcript)
                text = "".join(transcript)
                required = (
                    "Application: >> " in text,
                    "VoiceLife: Delivering" in text,
                    "VoiceLife reminder TTS started" in text,
                    "VoiceLife reminder received TTS audio packet" in text,
                    "VoiceLife reminder TTS stopped" in text,
                )
                if all(required):
                    result = "pass"
                    break

        if say_process is not None and say_process.poll() is None:
            say_process.terminate()
            say_process.wait(timeout=5)
    except Exception as exc:  # noqa: BLE001 - manifest must capture hardware failures
        error = str(exc)
    finally:
        if say_process is not None and say_process.poll() is None:
            say_process.terminate()
            say_process.wait(timeout=5)
        if device is not None:
            device.close()
        try:
            switch_audio(original_output, "output")
            switch_audio(original_system, "system")
            set_volume(original_volume)
        except Exception as restore_error:  # noqa: BLE001
            error = f"{error}; audio restore failed: {restore_error}" if error else str(restore_error)
            result = "fail"

    text = "".join(transcript)
    manifest = {
        "result": result,
        "port": args.port,
        "phrase": args.phrase,
        "voice": args.voice,
        "capturedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
        "originalOutput": original_output,
        "originalSystemOutput": original_system,
        "requiredLogPatterns": [
            "Application: >> ",
            "VoiceLife: Delivering",
            "VoiceLife reminder TTS started",
            "VoiceLife reminder received TTS audio packet",
            "VoiceLife reminder TTS stopped",
        ],
        "observedLogPatterns": {
            "stt": "Application: >> " in text,
            "delivery": "VoiceLife: Delivering" in text,
            "ttsStart": "VoiceLife reminder TTS started" in text,
            "ttsAudio": "VoiceLife reminder received TTS audio packet" in text,
            "ttsStop": "VoiceLife reminder TTS stopped" in text,
        },
        "error": error,
    }
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    return 0 if result == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
