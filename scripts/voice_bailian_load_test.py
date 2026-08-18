#!/usr/bin/env python3
"""Run a bounded, sanitized TTS-to-STT load test against DashScope."""

from __future__ import annotations

import argparse
import base64
import concurrent.futures
import json
import mimetypes
import os
import statistics
import sys
import time
from collections import Counter
from dataclasses import asdict, dataclass
from typing import Any
from urllib import error as urlerror
from urllib import request

try:
    import dashscope
    from dashscope.audio.tts_v2 import SpeechSynthesizer
except ImportError:
    dashscope = None
    SpeechSynthesizer = None

DEFAULT_TEXT = "这是本地语音模型连通性测试。"
ASR_ENDPOINT = "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation"


@dataclass
class Result:
    tts_ms: float = 0.0
    tts_first_package_ms: float | None = None
    stt_ms: float = 0.0
    end_to_end_ms: float = 0.0
    audio_bytes: int = 0
    transcript_matches: bool = False
    error: str | None = None


def percentile(values: list[float], percentage: int) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, (len(ordered) * percentage + 99) // 100 - 1))
    return round(ordered[index], 2)


def summarize(results: list[Result], model: str, asr_model: str, concurrency: int) -> dict[str, Any]:
    successful = [result for result in results if result.error is None]

    def metrics(name: str, values: list[float]) -> dict[str, float | None]:
        return {
            "metric": name,
            "count": len(values),
            "p50_ms": percentile(values, 50),
            "p95_ms": percentile(values, 95),
            "p99_ms": percentile(values, 99),
            "mean_ms": round(statistics.mean(values), 2) if values else None,
        }

    return {
        "tts_model": model,
        "stt_model": asr_model,
        "requested": len(results),
        "concurrency": concurrency,
        "successful": len(successful),
        "failed": len(results) - len(successful),
        "transcript_matches": sum(result.transcript_matches for result in successful),
        "audio_bytes": {
            "total": sum(result.audio_bytes for result in successful),
            "mean": round(statistics.mean([result.audio_bytes for result in successful]), 2) if successful else None,
        },
        "latency": [
            metrics("tts", [result.tts_ms for result in successful]),
            metrics(
                "tts_first_package",
                [result.tts_first_package_ms for result in successful if result.tts_first_package_ms is not None],
            ),
            metrics("stt", [result.stt_ms for result in successful]),
            metrics("end_to_end", [result.end_to_end_ms for result in successful]),
        ],
        "errors": dict(sorted(Counter(result.error for result in results if result.error is not None).items())),
    }


def normalize(text: str) -> str:
    return "".join(character for character in text if character.isalnum())


def synthesize(model: str, voice: str, text: str) -> tuple[bytes, float, float | None]:
    started = time.monotonic()
    synthesizer = SpeechSynthesizer(model=model, voice=voice)
    audio = synthesizer.call(text)
    elapsed_ms = (time.monotonic() - started) * 1000
    if not isinstance(audio, (bytes, bytearray)) or not audio:
        raise RuntimeError("empty_tts_audio")
    first_package_delay = synthesizer.get_first_package_delay()
    return bytes(audio), elapsed_ms, float(first_package_delay) if first_package_delay is not None else None


def transcribe(api_key: str, model: str, audio: bytes, mime_type: str, timeout_seconds: int) -> tuple[str, float]:
    payload = {
        "model": model,
        "input": {
            "messages": [
                {
                    "role": "user",
                    "content": [{"audio": f"data:{mime_type};base64," + base64.b64encode(audio).decode("ascii")}],
                }
            ]
        },
        "parameters": {"asr_options": {"enable_itn": False}},
    }
    started = time.monotonic()
    http_request = request.Request(
        ASR_ENDPOINT,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"},
        method="POST",
    )
    with request.urlopen(http_request, timeout=timeout_seconds) as response:
        body = json.loads(response.read().decode("utf-8"))
    elapsed_ms = (time.monotonic() - started) * 1000
    choices = body.get("output", {}).get("choices", [])
    content = choices[0].get("message", {}).get("content", []) if choices else []
    transcript = next((item.get("text") for item in content if item.get("text")), "")
    if not transcript:
        raise RuntimeError("empty_stt_transcript")
    return transcript, elapsed_ms


def classify_error(error: Exception) -> str:
    """Return a stable, non-sensitive failure category for aggregate reports."""
    if isinstance(error, urlerror.HTTPError):
        return f"http:{error.code}"
    if isinstance(error, urlerror.URLError):
        return "network"
    if isinstance(error, TimeoutError):
        return "timeout"
    for attribute in ("status_code", "http_code"):
        status_code = getattr(error, attribute, None)
        if isinstance(status_code, int) and 100 <= status_code <= 599:
            return f"{type(error).__name__}:http:{status_code}"
    return type(error).__name__


def run_one(api_key: str, args: argparse.Namespace, stt_audio: bytes | None, stt_mime_type: str | None) -> Result:
    stage = "tts"
    try:
        started = time.monotonic()
        if args.mode == "stt":
            if stt_audio is None or stt_mime_type is None:
                raise RuntimeError("missing_stt_audio")
            audio, tts_ms, first_package_ms = stt_audio, 0.0, None
        else:
            audio, tts_ms, first_package_ms = synthesize(args.tts_model, args.voice, args.text)
        if args.mode == "tts":
            return Result(
                tts_ms=round(tts_ms, 2),
                tts_first_package_ms=first_package_ms,
                end_to_end_ms=round((time.monotonic() - started) * 1000, 2),
                audio_bytes=len(audio),
            )
        stage = "stt"
        transcript, stt_ms = transcribe(
            api_key, args.stt_model, audio, stt_mime_type or "audio/mpeg", args.timeout_seconds
        )
        return Result(
            tts_ms=round(tts_ms, 2),
            tts_first_package_ms=round(first_package_ms, 2) if first_package_ms is not None else None,
            stt_ms=round(stt_ms, 2),
            end_to_end_ms=round((time.monotonic() - started) * 1000, 2),
            audio_bytes=len(audio),
            transcript_matches=normalize(transcript) == normalize(args.text),
        )
    except Exception as error:  # Network/service errors are intentionally sanitized.
        return Result(error=f"{stage}:{classify_error(error)}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--requests", type=int, default=10, help="Total TTS-to-STT turns; must be positive.")
    parser.add_argument("--concurrency", type=int, default=2, help="Concurrent turns; must be positive.")
    parser.add_argument("--timeout-seconds", type=int, default=90, help="Per-STT HTTP timeout.")
    parser.add_argument("--tts-model", default="cosyvoice-v3-flash")
    parser.add_argument("--stt-model", default="qwen3-asr-flash")
    parser.add_argument("--voice", default="longanhuan_v3")
    parser.add_argument("--text", default=DEFAULT_TEXT)
    parser.add_argument(
        "--mode",
        choices=("tts-to-stt", "tts", "stt"),
        default="tts-to-stt",
        help="Run the end-to-end path, only TTS, or only STT with --stt-audio-path.",
    )
    parser.add_argument("--stt-audio-path", help="Local WAV/MP3/etc. used by --mode stt; never written to reports.")
    parser.add_argument("--result-json", help="Optional sanitized result path; never contains credentials or audio.")
    parser.add_argument(
        "--preflight",
        action="store_true",
        help="Print local readiness without making a network request or exposing credentials.",
    )
    args = parser.parse_args()
    if args.requests <= 0 or args.concurrency <= 0 or args.timeout_seconds <= 0 or not args.text.strip():
        parser.error("requests、concurrency、timeout-seconds 和 text 必须有效")
    if args.mode == "stt" and not args.stt_audio_path:
        parser.error("--mode stt 需要 --stt-audio-path")
    return args


def main() -> int:
    args = parse_args()
    api_key = os.environ.get("DASHSCOPE_API_KEY", "")
    needs_tts_dependency = args.mode != "stt"
    ready = {
        "api_key_configured": bool(api_key),
        "dashscope_installed": dashscope is not None or not needs_tts_dependency,
    }
    if args.preflight:
        print(json.dumps(ready, ensure_ascii=False))
        return 0 if all(ready.values()) else 2
    if not api_key:
        print("DASHSCOPE_API_KEY is required", file=sys.stderr)
        return 2
    if needs_tts_dependency and (dashscope is None or SpeechSynthesizer is None):
        print(
            "dashscope package is required; install the locked test dependency before running this probe",
            file=sys.stderr,
        )
        return 2
    if needs_tts_dependency:
        dashscope.api_key = api_key
    stt_audio: bytes | None = None
    stt_mime_type: str | None = None
    if args.mode == "stt":
        try:
            with open(args.stt_audio_path, "rb") as source:
                stt_audio = source.read()
        except OSError as error:
            print(f"cannot read --stt-audio-path: {error}", file=sys.stderr)
            return 2
        if not stt_audio:
            print("--stt-audio-path must not be empty", file=sys.stderr)
            return 2
        stt_mime_type = mimetypes.guess_type(args.stt_audio_path)[0] or "audio/wav"
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as executor:
        results = list(executor.map(lambda _: run_one(api_key, args, stt_audio, stt_mime_type), range(args.requests)))
    report = summarize(results, args.tts_model, args.stt_model, args.concurrency)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    if args.result_json:
        with open(args.result_json, "w", encoding="utf-8") as output:
            json.dump(
                {"report": report, "samples": [asdict(result) for result in results]},
                output,
                ensure_ascii=False,
                indent=2,
            )
    return 0 if report["failed"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
