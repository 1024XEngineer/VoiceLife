from __future__ import annotations

import json
import sys
import tempfile
import unittest
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))
import prepare_wakenet_corpus as corpus  # noqa: E402


def write_wav(path: Path, *, rate: int = 16_000, channels: int = 1, width: int = 2) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as stream:
        stream.setframerate(rate)
        stream.setnchannels(channels)
        stream.setsampwidth(width)
        stream.writeframes(b"\x01\x00" * 160)


def write_manifest(path: Path, samples: list[dict[str, object]]) -> None:
    path.write_text(json.dumps({"samples": samples}, ensure_ascii=False), encoding="utf-8")


def sample(file: str, text: str, speaker: str, *, label: str = "positive") -> dict[str, object]:
    return {
        "file": file,
        "text": text,
        "label": label,
        "speaker_id": speaker,
        "distance_m": 1.0,
        "speech_rate": 1.0,
        "pitch_rate": 1.0,
    }


class PrepareWakeNetCorpusTest(unittest.TestCase):
    def test_build_package_validates_contract_and_splits_by_speaker(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            tmp_path = Path(temporary_directory)
            source = tmp_path / "source"
            rows = [
                sample("a.wav", "你好牛牛", "speaker-a"),
                sample("b.wav", "牛来", "speaker-b"),
                sample("c.wav", "你好牛牛", "speaker-c"),
                sample("d.wav", "你好妞妞", "speaker-a", label="near_negative"),
            ]
            for row in rows:
                write_wav(source / str(row["file"]))
            manifest = source / "input.json"
            write_manifest(manifest, rows)

            result = corpus.build_package(source, manifest, tmp_path / "out", minimum_samples=0)

            self.assertEqual(result["sample_count"], 4)
            self.assertEqual(result["speaker_count"], 3)
            self.assertEqual(result["counts"]["splits"], {"test": 1, "train": 2, "validation": 1})
            packaged = json.loads((tmp_path / "out" / "manifest.json").read_text(encoding="utf-8"))
            self.assertTrue(all(item["file"].startswith("audio/") for item in packaged["samples"]))
            self.assertTrue(all(len(item["sha256"]) == 64 for item in packaged["samples"]))
            self.assertFalse(set(packaged["samples"][0]["speaker_id"]) - set("speaker-abc"))

    def test_missing_required_wake_word_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            tmp_path = Path(temporary_directory)
            source = tmp_path / "source"
            rows = [
                sample("a.wav", "你好牛牛", "speaker-a"),
                sample("b.wav", "你好牛牛", "speaker-b"),
                sample("c.wav", "你好牛牛", "speaker-c"),
            ]
            for row in rows:
                write_wav(source / str(row["file"]))
            manifest = source / "input.json"
            write_manifest(manifest, rows)

            with self.assertRaisesRegex(corpus.CorpusError, "牛来"):
                corpus.build_package(source, manifest, tmp_path / "out", minimum_samples=0)

    def test_invalid_wav_and_path_traversal_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            tmp_path = Path(temporary_directory)
            source = tmp_path / "source"
            rows = [sample("../escape.wav", "你好牛牛", "speaker-a")]
            manifest = source / "input.json"
            source.mkdir()
            write_manifest(manifest, rows)
            with self.assertRaisesRegex(corpus.CorpusError, "traversal"):
                corpus.build_package(source, manifest, tmp_path / "out", minimum_samples=0)

            rows = [
                sample("a.wav", "你好牛牛", "speaker-a"),
                sample("b.wav", "牛来", "speaker-b"),
                sample("c.wav", "牛来", "speaker-c"),
            ]
            write_manifest(manifest, rows)
            write_wav(source / "a.wav", rate=8_000)
            write_wav(source / "b.wav")
            write_wav(source / "c.wav")
            with self.assertRaisesRegex(corpus.CorpusError, "16 kHz"):
                corpus.build_package(source, manifest, tmp_path / "out", minimum_samples=0)
