#!/usr/bin/env python3
"""Validate and package WakeNet customization corpus metadata.

The generated package is an input to Espressif's WakeNet customization
service. It is deliberately not a WakeNet model and cannot be flashed as one.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
import wave
from collections import Counter
from pathlib import Path, PurePosixPath
from typing import Any

TARGET_SAMPLE_RATE = 16_000
TARGET_CHANNELS = 1
TARGET_SAMPLE_WIDTH = 2
DEFAULT_REQUIRED_WAKE_WORDS = ("你好牛牛", "牛来")
DEFAULT_MINIMUM_SAMPLES = 20_000
SPLITS = ("train", "validation", "test")


class CorpusError(ValueError):
    """Raised when a corpus cannot satisfy the WakeNet input contract."""


def _safe_relative_path(value: object) -> Path:
    if not isinstance(value, str) or not value.strip():
        raise CorpusError("sample.file must be a non-empty string")
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or path.name != value.split("/")[-1]:
        raise CorpusError(f"sample.file must be a relative path without traversal: {value!r}")
    if path.suffix.lower() != ".wav":
        raise CorpusError(f"sample.file must be a WAV file: {value!r}")
    return Path(*path.parts)


def _read_wav_info(path: Path) -> dict[str, object]:
    try:
        with wave.open(str(path), "rb") as wav:
            info = {
                "sample_rate": wav.getframerate(),
                "channels": wav.getnchannels(),
                "sample_width": wav.getsampwidth(),
                "compression": wav.getcomptype(),
                "frames": wav.getnframes(),
                "duration_ms": round(wav.getnframes() * 1000 / wav.getframerate()) if wav.getframerate() else 0,
            }
    except (wave.Error, OSError) as error:
        raise CorpusError(f"invalid WAV file {path}: {error}") from error
    if (
        info["sample_rate"] != TARGET_SAMPLE_RATE
        or info["channels"] != TARGET_CHANNELS
        or info["sample_width"] != TARGET_SAMPLE_WIDTH
        or info["compression"] != "NONE"
    ):
        raise CorpusError(
            f"{path} must be 16 kHz/mono/16-bit PCM WAV, got "
            f"rate={info['sample_rate']} channels={info['channels']} width={info['sample_width']} "
            f"compression={info['compression']}"
        )
    if info["frames"] <= 0:
        raise CorpusError(f"{path} is empty")
    return info


def _speaker_id(sample: dict[str, Any], index: int) -> str:
    value = sample.get("speaker_id") or sample.get("voice")
    if not isinstance(value, str) or not value.strip():
        raise CorpusError(f"sample {index} needs speaker_id (voice is accepted as a fallback)")
    return value.strip()


def _speed_class(sample: dict[str, Any]) -> str:
    explicit = sample.get("speed_class")
    if explicit in ("slow", "normal", "fast"):
        return str(explicit)
    value = sample.get("speech_rate")
    if isinstance(value, (int, float)):
        if value <= 0.9:
            return "slow"
        if value >= 1.1:
            return "fast"
        return "normal"
    raise CorpusError("each sample needs speed_class or numeric speech_rate")


def _distance_m(sample: dict[str, Any]) -> float:
    value = sample.get("distance_m")
    if isinstance(value, (int, float)) and value > 0:
        return float(value)
    raise CorpusError("each sample needs positive distance_m metadata")


def _assign_splits(speaker_ids: list[str]) -> dict[str, str]:
    ordered = sorted(set(speaker_ids))
    if len(ordered) < 3:
        raise CorpusError("at least three distinct speakers are required for train/validation/test splits")
    assignment = {}
    for index, speaker in enumerate(ordered):
        assignment[speaker] = SPLITS[index % len(SPLITS)]
    return assignment


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def build_package(
    source_root: Path,
    source_manifest: Path,
    output_dir: Path,
    required_wake_words: tuple[str, ...] = DEFAULT_REQUIRED_WAKE_WORDS,
    minimum_samples: int = DEFAULT_MINIMUM_SAMPLES,
    copy_audio: bool = True,
) -> dict[str, object]:
    try:
        manifest = json.loads(source_manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise CorpusError(f"cannot read manifest {source_manifest}: {error}") from error
    samples = manifest.get("samples") if isinstance(manifest, dict) else None
    if not isinstance(samples, list) or not samples:
        raise CorpusError("manifest.samples must be a non-empty list")

    normalized: list[dict[str, object]] = []
    speakers: list[str] = []
    for index, raw in enumerate(samples):
        if not isinstance(raw, dict):
            raise CorpusError(f"sample {index} must be an object")
        relative = _safe_relative_path(raw.get("file"))
        source = (source_root / relative).resolve()
        root = source_root.resolve()
        if root not in source.parents:
            raise CorpusError(f"sample path escapes source root: {relative}")
        if not source.is_file():
            raise CorpusError(f"sample audio is missing: {source}")
        wav_info = _read_wav_info(source)
        text = raw.get("text")
        label = raw.get("label")
        if not isinstance(text, str) or not text.strip():
            raise CorpusError(f"sample {index} needs non-empty text")
        if label not in ("positive", "near_negative", "background"):
            raise CorpusError(f"sample {index} has unsupported label {label!r}")
        speaker = _speaker_id(raw, index)
        speakers.append(speaker)
        normalized.append(
            {
                "source_file": relative.as_posix(),
                "text": text,
                "label": label,
                "speaker_id": speaker,
                "distance_m": _distance_m(raw),
                "speed_class": _speed_class(raw),
                "speech_rate": raw.get("speech_rate"),
                "pitch_rate": raw.get("pitch_rate"),
                "sha256": _sha256(source),
                "wav": wav_info,
                "_source": source,
            }
        )

    present_targets = {str(item["text"]) for item in normalized if item["label"] == "positive"}
    missing = [word for word in required_wake_words if word not in present_targets]
    if missing:
        raise CorpusError(f"missing positive wake-word samples: {', '.join(missing)}")
    split_by_speaker = _assign_splits(speakers)

    if output_dir.exists() and not output_dir.is_dir():
        raise CorpusError(f"output path is not a directory: {output_dir}")
    output_dir.mkdir(parents=True, exist_ok=True)
    packaged: list[dict[str, object]] = []
    for item in normalized:
        source = item.pop("_source")
        assert isinstance(source, Path)
        speaker = str(item["speaker_id"])
        split = split_by_speaker[speaker]
        relative_output = Path("audio") / split / Path(str(item["source_file"]))
        destination = output_dir / relative_output
        if copy_audio:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            if _sha256(destination) != item["sha256"]:
                raise CorpusError(f"copied audio checksum mismatch: {destination}")
        item["split"] = split
        item["file"] = relative_output.as_posix() if copy_audio else str(source)
        packaged.append(item)

    label_counts = Counter(str(item["label"]) for item in packaged)
    word_counts = Counter(str(item["text"]) for item in packaged if item["label"] == "positive")
    distance_counts = Counter(str(item["distance_m"]) for item in packaged)
    speed_counts = Counter(str(item["speed_class"]) for item in packaged)
    split_counts = Counter(str(item["split"]) for item in packaged)
    warnings: list[str] = []
    if len(packaged) < minimum_samples:
        warnings.append(f"sample_count={len(packaged)} is below official minimum={minimum_samples}")
    if len(set(speakers)) < 500:
        warnings.append(f"speaker_count={len(set(speakers))} is below official recommendation=500")
    child_count = sum(1 for item in normalized if str(item["speaker_id"]).lower().startswith("child"))
    if child_count < 100:
        warnings.append(f"child_sample_count={child_count} is below official recommendation=100")
    for distance in ("1.0", "3.0"):
        if distance not in distance_counts:
            warnings.append(f"distance_m={distance} has no samples")
    for speed in ("slow", "normal", "fast"):
        if speed not in speed_counts:
            warnings.append(f"speed_class={speed} has no samples")

    result = {
        "schema_version": 1,
        "purpose": "espressif-wakenet-customization-input",
        "model_contract": {
            "sample_rate": TARGET_SAMPLE_RATE,
            "channels": TARGET_CHANNELS,
            "bits_per_sample": TARGET_SAMPLE_WIDTH * 8,
            "encoding": "signed_pcm",
            "format": "wav",
            "official_minimum_samples": DEFAULT_MINIMUM_SAMPLES,
            "official_recommended_speakers": 500,
            "official_recommended_children": 100,
        },
        "required_wake_words": list(required_wake_words),
        "source_manifest": str(source_manifest),
        "sample_count": len(packaged),
        "speaker_count": len(set(speakers)),
        "counts": {
            "labels": dict(sorted(label_counts.items())),
            "positive_wake_words": dict(sorted(word_counts.items())),
            "distance_m": dict(sorted(distance_counts.items())),
            "speed_class": dict(sorted(speed_counts.items())),
            "splits": dict(sorted(split_counts.items())),
        },
        "warnings": warnings,
        "samples": packaged,
    }
    (output_dir / "manifest.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--source-manifest", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--required-wake-word", action="append", dest="required_wake_words")
    parser.add_argument("--minimum-samples", type=int, default=DEFAULT_MINIMUM_SAMPLES)
    parser.add_argument("--no-copy-audio", action="store_true")
    args = parser.parse_args()
    if args.minimum_samples < 0:
        parser.error("minimum-samples must not be negative")
    return args


def main() -> int:
    args = parse_args()
    required = tuple(args.required_wake_words or DEFAULT_REQUIRED_WAKE_WORDS)
    try:
        result = build_package(
            args.source_root,
            args.source_manifest,
            args.output_dir,
            required_wake_words=required,
            minimum_samples=args.minimum_samples,
            copy_audio=not args.no_copy_audio,
        )
    except CorpusError as error:
        print(f"corpus_invalid:{error}", file=sys.stderr)
        return 2
    print(
        f"corpus_ready samples={result['sample_count']} speakers={result['speaker_count']} "
        f"warnings={len(result['warnings'])} manifest={args.output_dir / 'manifest.json'}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
