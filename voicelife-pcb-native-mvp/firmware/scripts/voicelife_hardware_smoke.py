#!/usr/bin/env python3
"""Run a non-destructive VoiceLife reminder smoke test on an ESP32-S3.

The test only replaces the VoiceLife SPIFFS partition.  It reads that
partition first and restores the exact bytes in a finally block; it never
erases the chip or touches application/NVS partitions.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

PARTITION_OFFSET = 0xE00000
PARTITION_SIZE = 0x200000
DEFAULT_PORT = "/dev/cu.usbmodem5A840116301"
DEFAULT_BACKUP = Path(__file__).resolve().parents[2] / "backup" / "esp32s3-original-flash-20260729.bin"
EXPECTED_BACKUP_SHA256 = "4e3ea1bd77873dc2b300f7b14adf0c3b5b93ceb15a8febe15d1c19464b76385d"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def tool_path(name: str, fallback: str | None = None) -> str:
    configured = os.environ.get(name)
    if configured:
        return configured
    if fallback and Path(fallback).exists():
        return fallback
    found = shutil.which(name.lower()) or shutil.which(name)
    if found:
        return found
    raise RuntimeError(f"找不到工具 {name}，请通过环境变量 {name} 指定路径")


def run_esptool(esptool: str, port: str, baud: int, *arguments: str) -> None:
    command = [
        sys.executable,
        esptool,
        "--chip",
        "esp32s3",
        "--port",
        port,
        "--baud",
        str(baud),
        *arguments,
    ]
    subprocess.run(command, check=True)


def utc_after(seconds: int) -> str:
    value = dt.datetime.now(dt.timezone.utc) + dt.timedelta(seconds=seconds)
    return value.replace(microsecond=0).isoformat().replace("+00:00", "Z")


def make_state(root: Path, delay: int) -> tuple[str, str]:
    starts_at = utc_after(delay)
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d%H%M%S")
    event_id = f"event-hardware-smoke-{stamp}"
    reminder_id = f"reminder-hardware-smoke-{stamp}"
    state = {
        "schemaVersion": 1,
        "events": [
            {
                "id": event_id,
                "title": "VoiceLife MVP 真机验证",
                "startsAt": starts_at,
                "endsAt": "",
                "kind": "point",
                "timeZone": "Asia/Shanghai",
                "location": "",
                "notes": "",
                "recurrenceFrequency": "",
                "recurrenceWeekday": 0,
                "recurrenceMonthDay": 0,
                "reminderOffsetMinutes": 0,
                "weakReminderEnabled": False,
                "weakReminderMinutes": 15,
                "paused": False,
                "terminated": False,
                "skippedOccurrences": [],
                "createdAt": int(time.time()),
                "updatedAt": int(time.time()),
            }
        ],
        "reminders": [
            {
                "id": reminder_id,
                "eventId": event_id,
                "originalStartAt": starts_at,
                "triggerAt": starts_at,
                "weak": False,
                "status": "scheduled",
                "snoozeCount": 0,
                "deliveredAt": 0,
                "closedAt": 0,
            }
        ],
        "notes": [],
        "receipts": [],
        "undoOperations": [],
    }
    root.mkdir(parents=True, exist_ok=True)
    (root / "state.json").write_text(json.dumps(state, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return event_id, starts_at


def capture_serial(port: str, log_path: Path, seconds: int) -> str:
    try:
        import serial  # type: ignore
    except ImportError as error:
        raise RuntimeError("需要 pyserial：python -m pip install pyserial") from error

    deadline = time.monotonic() + seconds
    chunks: list[str] = []
    with serial.Serial(port, 115200, timeout=0.5) as device, log_path.open("wb") as log:
        while time.monotonic() < deadline:
            data = device.read(device.in_waiting or 1)
            if not data:
                continue
            log.write(data)
            log.flush()
            chunks.append(data.decode("utf-8", errors="replace"))
    return "".join(chunks)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--delay", type=int, default=45, help="seconds until the fixture reminder is due")
    parser.add_argument("--serial-seconds", type=int, default=100)
    parser.add_argument("--baud", type=int, default=460800,
                        help="esptool serial baud rate (default: 460800)")
    parser.add_argument("--evidence-dir", type=Path)
    parser.add_argument("--no-restore", action="store_true", help="leave the fixture installed for manual inspection")
    args = parser.parse_args()
    if args.delay < 20:
        parser.error("--delay 至少 20 秒，给设备留下联网和启动时间")

    evidence = args.evidence_dir or Path(__file__).resolve().parents[2] / "test-evidence" / (
        dt.datetime.now().strftime("%Y%m%d-%H%M%S-hardware-smoke")
    )
    evidence.mkdir(parents=True, exist_ok=True)
    backup = DEFAULT_BACKUP
    if not backup.exists() or sha256(backup) != EXPECTED_BACKUP_SHA256:
        raise RuntimeError(f"原始整片备份缺失或哈希不匹配：{backup}")

    esptool = tool_path(
        "ESPTOOL",
        "/Users/mac/Library/Python/3.9/bin/esptool.py",
    )
    idf_path = Path(os.environ.get("IDF_PATH", "/Users/mac/esp-idf-v6.0.2"))
    spiffsgen = Path(os.environ.get("SPIFFSGEN", str(idf_path / "components/spiffs/spiffsgen.py")))
    if not spiffsgen.exists():
        raise RuntimeError(f"找不到 spiffsgen.py：{spiffsgen}")

    before = evidence / "voicelife-before.bin"
    after = evidence / "voicelife-after.bin"
    fixture = evidence / "voicelife-fixture.bin"
    serial_log = evidence / "serial.log"
    manifest_path = evidence / "manifest.json"
    event_id = ""
    starts_at = ""
    fixture_sha256: str | None = None
    output: str = ""
    result = "not_run"
    restore_error: str | None = None

    with tempfile.TemporaryDirectory(prefix="voicelife-fixture-") as temp_dir:
        root = Path(temp_dir)
        event_id, starts_at = make_state(root, args.delay)
        subprocess.run(
            [
                sys.executable,
                str(spiffsgen),
                str(PARTITION_SIZE),
                str(root),
                str(fixture),
                "--page-size",
                "256",
                "--block-size",
                "4096",
                "--obj-name-len",
                "32",
                "--meta-len",
                "4",
            ],
            check=True,
        )
        fixture_sha256 = sha256(fixture)
        try:
            run_esptool(esptool, args.port, args.baud, "read_flash", hex(PARTITION_OFFSET), hex(PARTITION_SIZE), str(before))
            run_esptool(esptool, args.port, args.baud, "write_flash", "--flash_size", "keep", hex(PARTITION_OFFSET), str(fixture))
            output = capture_serial(args.port, serial_log, args.serial_seconds)
            run_esptool(esptool, args.port, args.baud, "read_flash", hex(PARTITION_OFFSET), hex(PARTITION_SIZE), str(after))
            required = [
                "VoiceLifeStorage: Loaded",
                "Delivering",
                "VoiceLife reminder TTS started",
                "VoiceLife reminder received TTS audio packet",
                "VoiceLife reminder TTS stopped",
                "Marked",
            ]
            missing = [pattern for pattern in required if pattern not in output]
            after_text = subprocess.run(["strings", "-a", str(after)], check=True, capture_output=True, text=True).stdout
            if '"status":"pushed"' not in after_text or '"deliveredAt":0' in after_text:
                missing.append("readback:pushed-state")
            result = "pass" if not missing else "fail:" + ",".join(missing)
        finally:
            if not args.no_restore:
                try:
                    run_esptool(esptool, args.port, args.baud, "write_flash", "--flash_size", "keep", hex(PARTITION_OFFSET), str(before))
                except (OSError, subprocess.CalledProcessError) as error:
                    restore_error = str(error)

    manifest = {
        "result": result if restore_error is None else f"{result};restore_failed",
        "port": args.port,
        "baud": args.baud,
        "partitionOffset": hex(PARTITION_OFFSET),
        "partitionSize": hex(PARTITION_SIZE),
        "eventId": event_id,
        "startsAt": starts_at,
        "fixtureSha256": fixture_sha256,
        "beforeSha256": sha256(before) if before.exists() else None,
        "afterSha256": sha256(after) if after.exists() else None,
        "originalBackupSha256": sha256(backup),
        "restoreRequested": not args.no_restore,
        "restoreError": restore_error,
        "requiredLogPatterns": [
            "VoiceLifeStorage: Loaded",
            "Delivering",
            "VoiceLife reminder TTS started",
            "VoiceLife reminder received TTS audio packet",
            "VoiceLife reminder TTS stopped",
            "Marked",
        ],
    }
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    return 0 if result == "pass" and restore_error is None else 1


if __name__ == "__main__":
    raise SystemExit(main())
