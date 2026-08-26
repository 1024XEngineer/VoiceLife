#!/usr/bin/env python3
"""Drive a real SparkBot/Linx multi-turn voice test through the test-only USB PCM harness."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

try:
    import dashscope
    import serial
    from dashscope.audio.tts_v2 import SpeechSynthesizer
except ImportError:
    dashscope = None
    serial = None
    SpeechSynthesizer = None


MAGIC, VERSION, BEGIN, PCM, END = b"VLVT", 1, 1, 2, 3
PCM_FRAME_BYTES = 640
DEFAULT_TURNS = ["你好牛牛，请介绍一下你自己。", "把刚才的回答再简短一点。", "请用一句话总结我们刚才的对话。"]


@dataclass
class TurnResult:
    index: int
    input_text: str
    tts_ms: int = 0
    pcm_frames: int = 0
    pcm_frames_sent: int = 0
    input_endpoint_truncated: bool = False
    asr_text: str = ""
    asr_segments: list[str] = field(default_factory=list)
    asr_matches_input: bool = False
    reply_text: str = ""
    tts_sentences: list[str] = field(default_factory=list)
    tts_first_audio_seen: bool = False
    tts_stopped_seen: bool = False
    terminal_guard_armed: bool = False
    terminal_guard_clean: bool = True
    log_start: int = 0
    log_end: int = 0
    completed: bool = False
    error: str | None = None


@dataclass
class PreparedTurn:
    input_text: str
    tts_ms: int
    frames: list[bytes]


class SerialLog:
    def __init__(self, device: serial.Serial) -> None:
        self._device = device
        self._items: list[tuple[float, str]] = []
        self._condition = threading.Condition()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._read_loop, daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2)

    def mark(self) -> int:
        with self._condition:
            return len(self._items)

    def wait_for(self, marker: str, after: int, timeout: float) -> tuple[int, str]:
        deadline = time.monotonic() + timeout
        with self._condition:
            while True:
                for index in range(after, len(self._items)):
                    if marker in self._items[index][1]:
                        return index + 1, self._items[index][1]
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(marker)
                self._condition.wait(timeout=remaining)

    def wait_for_any(self, markers: tuple[str, ...], after: int, timeout: float) -> tuple[int, str]:
        """Wait for the first event in a protocol alternative set."""
        deadline = time.monotonic() + timeout
        with self._condition:
            while True:
                for index in range(after, len(self._items)):
                    line = self._items[index][1]
                    if any(marker in line for marker in markers):
                        return index + 1, line
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(" or ".join(markers))
                self._condition.wait(timeout=remaining)

    def lines_since(self, after: int) -> list[str]:
        with self._condition:
            return [line for _, line in self._items[after:]]

    def contains_since(self, marker: str, after: int) -> bool:
        with self._condition:
            return any(marker in line for _, line in self._items[after:])

    def all_lines(self) -> list[str]:
        return self.lines_since(0)

    def _read_loop(self) -> None:
        while not self._stop.is_set():
            try:
                raw = self._device.readline()
            except serial.SerialException as error:
                self._append(f"SERIAL_HOST_ERROR {type(error).__name__}")
                return
            if raw:
                self._append(raw.decode("utf-8", "replace").rstrip())

    def _append(self, line: str) -> None:
        print(line, flush=True)
        with self._condition:
            self._items.append((time.monotonic(), line))
            self._condition.notify_all()


def packet(kind: int, payload: bytes = b"") -> bytes:
    if len(payload) > 0xFFFF:
        raise ValueError("serial payload is too large")
    return MAGIC + bytes((VERSION, kind)) + len(payload).to_bytes(2, "little") + payload


def open_serial(port: str, baud: int) -> serial.Serial:
    """Open USB UART without toggling SparkBot reset lines by default."""
    device = serial.Serial()
    device.port = port
    device.baudrate = baud
    device.timeout = 0.2
    device.write_timeout = 5
    # SparkBot maps RTS to EN. Keep both modem-control lines inactive while
    # pyserial opens the USB-JTAG endpoint; enabling flow control here can
    # suppress the later reset pulse or reset the board during open().
    device.dsrdtr = False
    device.rtscts = False
    device.dtr = False
    device.rts = False
    device.open()
    return device


def reset_usb_serial_jtag(device: serial.Serial) -> None:
    """Reset SparkBot's application through the USB-Serial/JTAG EN line."""
    # SparkBot exposes EN on RTS and has no boot-button automation. Keep DTR
    # deasserted so the pulse cannot select the ROM downloader.
    device.rts = False
    device.dtr = False
    device.rts = True
    time.sleep(0.15)
    device.rts = False
    time.sleep(0.2)


def synthesize(text: str, model: str, voice: str) -> bytes:
    synthesizer = SpeechSynthesizer(model=model, voice=voice)
    audio = synthesizer.call(text)
    if not isinstance(audio, (bytes, bytearray)) or not audio:
        raise RuntimeError("empty_tts_audio")
    return bytes(audio)


def synthesize_macos_say(text: str, voice: str) -> bytes:
    """Generate local AIFF only for board-harness fallback input.

    This path is intentionally opt-in. It lets the serial state-machine test
    remain reproducible when the external TTS provider is unavailable; it is
    not reported as a DashScope model result.
    """
    path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(prefix="voicelife-serial-input-", suffix=".aiff", delete=False) as output:
            path = Path(output.name)
        subprocess.run(["say", "-v", voice, "-o", str(path), text], check=True, capture_output=True)
        audio = path.read_bytes()
        if not audio:
            raise RuntimeError("empty_local_tts_audio")
        return audio
    except FileNotFoundError as error:
        raise RuntimeError("macos_say_unavailable") from error
    except subprocess.CalledProcessError as error:
        raise RuntimeError("macos_say_failed") from error
    finally:
        if path is not None:
            path.unlink(missing_ok=True)


def to_pcm_frames(audio: bytes) -> list[bytes]:
    result = subprocess.run(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-i",
            "pipe:0",
            "-f",
            "s16le",
            "-acodec",
            "pcm_s16le",
            "-ac",
            "1",
            "-ar",
            "16000",
            "pipe:1",
        ],
        input=audio,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0 or not result.stdout:
        raise RuntimeError("ffmpeg_pcm_decode_failed")
    # The explicit serial end packet is the endpoint for this harness. Adding
    # trailing silence makes local VAD stop early, then incorrectly turns the
    # remaining test frames into late-frame rejection noise.
    pcm = result.stdout
    remainder = len(pcm) % PCM_FRAME_BYTES
    if remainder:
        pcm += bytes(PCM_FRAME_BYTES - remainder)
    return [pcm[offset : offset + PCM_FRAME_BYTES] for offset in range(0, len(pcm), PCM_FRAME_BYTES)]


def wait_evidence(log: SerialLog, event: str, cursor: int, timeout: float) -> tuple[int, str]:
    return log.wait_for(f"SERIAL_VOICE_EVIDENCE event={event} ", cursor, timeout)


def collect_until(
    log: SerialLog,
    markers: tuple[str, ...],
    cursor: int,
    timeout: float,
) -> tuple[int, list[str], str]:
    """Collect all matching evidence until the terminal marker is observed."""
    deadline = time.monotonic() + timeout
    lines: list[str] = []
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(" or ".join(markers))
        cursor, line = log.wait_for_any(markers, cursor, remaining)
        lines.append(line)
        if markers[-1] in line:
            return cursor, lines, line


def evidence_text(line: str) -> str:
    marker = " detail="
    return line.split(marker, 1)[1] if marker in line else ""


def normalize_transcript(text: str) -> str:
    """Ignore display-only punctuation and whitespace, never semantic words."""
    return "".join(character for character in text if character.isalnum())


def parse_audio_stats(line: str) -> dict[str, int]:
    return {key: int(value) for key, value in re.findall(r"(\w+)=([0-9]+)", line)}


def prepare_turns(texts: list[str], input_tts: str, model: str, voice: str, say_voice: str) -> list[PreparedTurn]:
    """Generate every host utterance before opening the first device capture.

    The firmware reopens capture immediately after each non-terminal reply. Cloud
    TTS must therefore not run between turns, or its variable latency can let
    local VAD end the newly opened capture before the next PCM frame arrives.
    """
    prepared: list[PreparedTurn] = []
    for index, text in enumerate(texts, start=1):
        started = time.monotonic()
        audio = synthesize(text, model, voice) if input_tts == "dashscope" else synthesize_macos_say(text, say_voice)
        frames = to_pcm_frames(audio)
        tts_ms = round((time.monotonic() - started) * 1000)
        if not frames:
            raise RuntimeError(f"turn_{index}_has_no_pcm_frames")
        prepared.append(PreparedTurn(input_text=text, tts_ms=tts_ms, frames=frames))
    return prepared


def run_turn(
    device: serial.Serial,
    log: SerialLog,
    index: int,
    prepared: PreparedTurn,
    response_timeout: float,
    first_turn: bool,
    expect_terminal: bool,
    guard_observation_seconds: float,
) -> TurnResult:
    result = TurnResult(
        index=index,
        input_text=prepared.input_text,
        tts_ms=prepared.tts_ms,
        pcm_frames=len(prepared.frames),
    )
    try:
        cursor = log.mark()
        result.log_start = cursor
        # The firmware closes the test-input gate at every capture endpoint,
        # including automatic follow-up capture after TTS. Re-open it once per
        # turn; Runtime treats BEGIN as an injection-window refresh while it
        # is already Listening, so no duplicate PressDown is generated.
        device.write(packet(BEGIN))
        device.flush()
        turn_origin = cursor
        begin_cursor, _ = log.wait_for("SERIAL_VOICE_TURN_BEGIN=ok", turn_origin, 5)
        if first_turn:
            capture_cursor, _ = log.wait_for("SERIAL_VOICE_CAPTURE_READY", turn_origin, 12)
            cursor = max(begin_cursor, capture_cursor)
        else:
            cursor = begin_cursor
        turn_cursor = cursor
        for frame in prepared.frames:
            # A device-side endpoint closes capture asynchronously. Stop the
            # test source immediately so a malformed utterance is reported as
            # input truncation rather than inflated with late PCM rejections.
            if log.contains_since("SERIAL_VOICE_CAPTURE_CLOSED", turn_cursor):
                result.input_endpoint_truncated = True
                break
            device.write(packet(PCM, frame))
            device.flush()
            result.pcm_frames_sent += 1
            time.sleep(0.02)
        device.write(packet(END))
        device.flush()
        # The serial task may wait for pooled PCM payloads while a long Linx
        # utterance drains. Scale the endpoint window with the injected frame
        # count, but cap it so a genuinely stuck turn still fails promptly.
        turn_end_timeout = min(60.0, max(15.0, 10.0 + len(prepared.frames) * 0.1))
        # Auto mode may let Linx's server VAD finish the turn before the USB
        # fixture's explicit END packet reaches the state machine. In that
        # ordering STT is the authoritative endpoint and is already followed
        # by the same TTS state flow; waiting for a later local stop would
        # skip the valid ASR line and manufacture a timeout.
        cursor, endpoint_marker = log.wait_for_any(
            (
                "SERIAL_VOICE_TURN_END",
                "SERIAL_VOICE_EVIDENCE event=capture_stopped ",
                "SERIAL_VOICE_EVIDENCE event=stt_text_received ",
            ),
            turn_cursor,
            turn_end_timeout,
        )
        asr_lines: list[str] = []
        if "event=stt_text_received" in endpoint_marker:
            asr_lines.append(endpoint_marker)
        if "SERIAL_VOICE_TURN_END" in endpoint_marker:
            if "=ok" not in endpoint_marker and not result.input_endpoint_truncated:
                raise RuntimeError(f"turn_end_failed:{endpoint_marker}")
            cursor, endpoint_followup = log.wait_for_any(
                (
                    "SERIAL_VOICE_EVIDENCE event=capture_stopped ",
                    "SERIAL_VOICE_EVIDENCE event=stt_text_received ",
                ),
                cursor,
                response_timeout,
            )
            if "event=stt_text_received" in endpoint_followup:
                asr_lines.append(endpoint_followup)
                capture_stopped_seen = False
            else:
                capture_stopped_seen = True
        elif asr_lines:
            capture_stopped_seen = False
        else:
            # Local VAD is a valid endpoint even when the USB fixture's END
            # packet is consumed after the capture callback has already run.
            capture_stopped_seen = True
        # Local VAD can stop capture before the explicit host end packet. The
        # packet still terminates injection, but the real state transition is
        # valid from any point after this turn began.
        if not asr_lines:
            if not capture_stopped_seen:
                cursor, _ = wait_evidence(log, "capture_stopped", cursor, 12)
            cursor, asr_line = wait_evidence(log, "stt_text_received", cursor, response_timeout)
            asr_lines.append(asr_line)

        # Server VAD can split one injected utterance into multiple final STT
        # events. Preserve every segment and compare their concatenation with
        # the source utterance instead of silently keeping only the first one.
        result.asr_segments = [evidence_text(line) for line in asr_lines]
        result.asr_text = "".join(result.asr_segments)
        result.asr_matches_input = normalize_transcript(result.asr_text) == normalize_transcript(result.input_text)
        cursor, _ = wait_evidence(log, "tts_started", cursor, response_timeout)
        # Linx may announce sentence text before the first PCM packet, and a
        # long answer may contain many sentence_start events. Collect the
        # complete stream through tts_stopped so the report proves full TTS.
        cursor, tts_lines, _ = collect_until(
            log,
            (
                "SERIAL_VOICE_EVIDENCE event=tts_first_audio ",
                "SERIAL_VOICE_EVIDENCE event=tts_sentence_started ",
                "SERIAL_VOICE_EVIDENCE event=tts_stopped ",
            ),
            cursor,
            response_timeout,
        )
        result.tts_first_audio_seen = any("event=tts_first_audio" in line for line in tts_lines)
        result.tts_stopped_seen = any("event=tts_stopped" in line for line in tts_lines)
        result.tts_sentences = [evidence_text(line) for line in tts_lines if "event=tts_sentence_started" in line]
        result.reply_text = result.tts_sentences[0] if result.tts_sentences else ""
        if not result.tts_first_audio_seen or not result.tts_stopped_seen:
            raise RuntimeError("tts_stream_incomplete")
        if expect_terminal:
            cursor, _ = log.wait_for("WAKE_GUARD_ARMED ms=8000 reason=terminal_tts", cursor, 12)
            result.terminal_guard_armed = True
            # Record the entire acoustic tail after the guard is armed. The
            # fixed farewell text contains the local command word, so only
            # events after this point may prove a false re-wake.
            time.sleep(guard_observation_seconds)
            unexpected_markers = (
                "WAKE_DETECTED",
                "SERIAL_VOICE_EVIDENCE event=wake_detected",
                "SERIAL_VOICE_CAPTURE_READY",
                "detail=收到！",
            )
            guard_lines = log.lines_since(cursor)
            hits = [marker for marker in unexpected_markers if any(marker in line for line in guard_lines)]
            result.terminal_guard_clean = not hits
            if hits:
                raise RuntimeError(f"terminal_wake_guard_failed:{','.join(hits)}")
            result.completed = True
            result.log_end = log.mark()
            return result
        # VoiceSession reopens listening for the next conversational turn.
        # Wait for that existing capture instead of sending a second PressDown.
        log.wait_for("SERIAL_VOICE_CAPTURE_READY", cursor, 12)
        result.completed = True
    except (RuntimeError, TimeoutError, serial.SerialException) as error:
        result.error = str(error)
    result.log_end = log.mark()
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/cu.usbmodem14201")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--reset-before-run",
        action="store_true",
        help="Hard-reset the test device after opening the serial port, then wait for its full ready sequence.",
    )
    parser.add_argument("--text", action="append", help="One input utterance; repeat for a multi-turn conversation.")
    parser.add_argument("--tts-model", default="cosyvoice-v3-flash")
    parser.add_argument("--voice", default="longanhuan_v3")
    parser.add_argument(
        "--input-tts",
        choices=("dashscope", "macos-say"),
        default="dashscope",
        help="Source for injected audio. macos-say is a local fallback when external TTS is unavailable.",
    )
    parser.add_argument("--say-voice", default="Ting-Ting", help="macOS voice used with --input-tts macos-say.")
    parser.add_argument("--response-timeout", type=float, default=45)
    parser.add_argument(
        "--expect-terminal",
        action="store_true",
        help="Treat the final utterance as terminal and verify the 8-second local-wake guard.",
    )
    parser.add_argument(
        "--guard-observation-seconds",
        type=float,
        default=8.5,
        help="Raw-serial observation window after terminal TTS; default covers the 8-second firmware guard.",
    )
    parser.add_argument(
        "--require-display-scroll",
        action="store_true",
        help="Require at least one subtitle wider than the safe one-line viewport to enter horizontal scrolling.",
    )
    parser.add_argument(
        "--allow-asr-mismatch",
        action="store_true",
        help="Record but do not fail on STT text mismatches; disabled by default for fidelity stress tests.",
    )
    parser.add_argument("--serial-log", type=Path, help="Optional local raw serial log; never commit it.")
    parser.add_argument(
        "--result-json", type=Path, help="Optional local result summary; never contains credentials or audio."
    )
    args = parser.parse_args()
    if args.baud <= 0 or args.response_timeout <= 0 or args.guard_observation_seconds < 8:
        parser.error("baud、response-timeout 和 guard-observation-seconds 必须有效，且观察窗口不得小于 8 秒")
    return args


def main() -> int:
    args = parse_args()
    if args.input_tts == "dashscope" and not os.environ.get("DASHSCOPE_API_KEY"):
        print("DASHSCOPE_API_KEY is required", file=sys.stderr)
        return 2
    if serial is None:
        print("pyserial is required", file=sys.stderr)
        return 2
    if args.input_tts == "dashscope" and (dashscope is None or SpeechSynthesizer is None):
        print("dashscope is required for --input-tts dashscope", file=sys.stderr)
        return 2
    if args.input_tts == "dashscope":
        dashscope.api_key = os.environ["DASHSCOPE_API_KEY"]
    texts = args.text or DEFAULT_TURNS
    try:
        # Prepare every utterance before the serial endpoint is opened. This is
        # part of the harness contract, not a production latency measurement.
        prepared_turns = prepare_turns(texts, args.input_tts, args.tts_model, args.voice, args.say_voice)
    except (RuntimeError, subprocess.SubprocessError, TimeoutError) as error:
        print(f"input_preparation_failed:{error}", file=sys.stderr)
        return 2
    try:
        device = open_serial(args.port, args.baud)
    except serial.SerialException as error:
        print(f"cannot open serial port: {type(error).__name__}", file=sys.stderr)
        return 2
    log = SerialLog(device)
    log.start()
    results: list[TurnResult] = []
    try:
        if args.reset_before_run:
            reset_usb_serial_jtag(device)
        log.wait_for("SERIAL_VOICE_TEST_READY=1", 0, 20)
        # READY means the serial endpoint and I2S port exist, not that the
        # asynchronous local wake-model bootstrap has returned the controller
        # to standby. Starting before standby_ready is intentionally rejected
        # by the state machine and must never be treated as a valid first turn.
        log.wait_for("SERIAL_VOICE_EVIDENCE event=standby_ready ", 0, 20)
        for index, prepared in enumerate(prepared_turns, start=1):
            result = run_turn(
                device,
                log,
                index,
                prepared,
                args.response_timeout,
                first_turn=index == 1,
                expect_terminal=args.expect_terminal and index == len(texts),
                guard_observation_seconds=args.guard_observation_seconds,
            )
            results.append(result)
            if result.error:
                break
    except TimeoutError as error:
        results.append(TurnResult(index=0, input_text="", error=f"harness_ready:{error}"))
    finally:
        time.sleep(1)
        log.stop()
        device.close()
    raw_lines = log.all_lines()
    if args.serial_log:
        args.serial_log.write_text("\n".join(raw_lines) + "\n", encoding="utf-8")
    last_audio_stats = next((line for line in reversed(raw_lines) if "AUDIO_STATS" in line), "")
    audio_stats = parse_audio_stats(last_audio_stats)
    required_zero = (
        "in_drop",
        "out_reject",
        "short_write",
        "in_i2s_err",
        "out_i2s_err",
    )
    required_present = ("test_in_frames", "out_frames", *required_zero)
    interaction_stats = [parse_audio_stats(line) for line in raw_lines if "INTERACTION_QUEUE_STATS" in line]
    interaction_keys = ("control_dropped", "best_effort_dropped", "board_dropped")
    serial_pcm_rejections = [line for line in raw_lines if "SERIAL_VOICE_PCM=reject" in line]
    required_active_phases = (3, 4, 5, 6)
    rendered_text_lines = [line for line in raw_lines if "SPARKBOT_TEXT_RENDER" in line]
    content_render_lines = [line for line in rendered_text_lines if "content_visible=1" in line]
    display_text_trace_complete = (
        bool(rendered_text_lines)
        and all(
            "generation=" in line
            and "revision=" in line
            and "content_height=" in line
            and "viewport_height=" in line
            and "overflow_width=" in line
            and "manual_line_breaks=" in line
            and " status=" in line
            and " content=" in line
            for line in rendered_text_lines
        )
        and all("viewport_height=120" in line and "content_height=0" not in line for line in content_render_lines)
    )
    display_scroll_observed = any(re.search(r"overflow_width=[1-9][0-9]*", line) for line in content_render_lines)

    def turn_phases_complete(marker: str) -> bool:
        return len(results) == len(texts) and all(
            all(
                any(f"{marker}={phase} " in line for line in raw_lines[result.log_start : result.log_end])
                for phase in required_active_phases
            )
            for result in results
        )

    acceptance = {
        "audio_stats_present": all(key in audio_stats for key in required_present),
        "audio_flow_observed": audio_stats.get("test_in_frames", 0) > 0 and audio_stats.get("out_frames", 0) > 0,
        "zero_loss": all(audio_stats.get(key, -1) == 0 for key in required_zero),
        "zero_serial_pcm_rejections": not serial_pcm_rejections,
        "asr_text_matches_input": args.allow_asr_mismatch
        or len(results) == len(texts)
        and all(result.asr_matches_input for result in results),
        "input_not_endpoint_truncated": not any(result.input_endpoint_truncated for result in results),
        "interaction_queue_clean": bool(interaction_stats)
        and all(all(stats.get(key, -1) == 0 for key in interaction_keys) for stats in interaction_stats),
        "no_interaction_rejection": not any("INTERACTION_REJECTED" in line for line in raw_lines),
        "no_provider_error": not any("SERIAL_VOICE_EVIDENCE event=provider_error" in line for line in raw_lines),
        "state_flow_complete": turn_phases_complete("state"),
        "display_flow_complete": turn_phases_complete("phase")
        and not any("SPARKBOT_DISPLAY_RENDER_FAILED" in line for line in raw_lines),
        "display_text_trace_complete": display_text_trace_complete,
        "display_scroll_observed": not args.require_display_scroll or display_scroll_observed,
        "terminal_guard_clean": not args.expect_terminal
        or (bool(results) and results[-1].terminal_guard_armed and results[-1].terminal_guard_clean),
    }
    report = {
        "requested_turns": len(texts),
        "completed_turns": sum(result.completed for result in results),
        "asr_exact_matches": sum(result.asr_matches_input for result in results),
        "results": [result.__dict__ for result in results],
        "last_audio_stats": last_audio_stats,
        "audio_stats": audio_stats,
        "serial_pcm_rejections": serial_pcm_rejections,
        "display": {
            "rendered_text_snapshots": len(rendered_text_lines),
            "content_snapshots": len(content_render_lines),
            "scroll_observed": display_scroll_observed,
        },
        "acceptance": acceptance,
    }
    print(json.dumps(report, ensure_ascii=False, indent=2))
    if args.result_json:
        args.result_json.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return 0 if report["completed_turns"] == len(texts) and all(acceptance.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
