#!/usr/bin/env python3
"""Inspect, back up, exercise, and restore the SQLite board probe safely."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from dataclasses import asdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
__all__ = [
    "EXPECTED_PHASES",
    "EXPECTED_RESET_POINTS",
    "PARTITION_ENTRY",
    "PARTITION_MAGIC",
    "PARTITION_TABLE_OFFSET",
    "PARTITION_TABLE_SIZE",
    "STABLE_BAUD",
    "Partition",
    "ProbeError",
    "ResetSequence",
    "esptool",
    "monitor",
    "parse_partition_table",
    "partition_by_label",
    "read_flash",
    "read_layout",
    "run",
    "sha256",
    "stable_baud",
    "validate_layout",
    "verify_flash",
]
try:
    from scripts.sqlite_board_probe_protocol import (
        EXPECTED_PHASES,
        EXPECTED_RESET_POINTS,
        PARTITION_ENTRY,
        PARTITION_MAGIC,
        PARTITION_TABLE_OFFSET,
        PARTITION_TABLE_SIZE,
        STABLE_BAUD,
        Partition,
        ProbeError,
        ResetSequence,
        parse_partition_table,
        partition_by_label,
        sha256,
        stable_baud,
        validate_layout,
    )
except ModuleNotFoundError:
    from sqlite_board_probe_protocol import (
        EXPECTED_PHASES,
        EXPECTED_RESET_POINTS,
        PARTITION_ENTRY,
        PARTITION_MAGIC,
        PARTITION_TABLE_OFFSET,
        PARTITION_TABLE_SIZE,
        STABLE_BAUD,
        Partition,
        ProbeError,
        ResetSequence,
        parse_partition_table,
        partition_by_label,
        sha256,
        stable_baud,
        validate_layout,
    )

try:
    from scripts import sqlite_board_probe_io as probe_io
    from scripts.sqlite_board_probe_io import (
        read_flash,
        run,
        verify_flash,
    )
except ModuleNotFoundError:
    import sqlite_board_probe_io as probe_io
    from sqlite_board_probe_io import (
        read_flash,
        run,
        verify_flash,
    )

try:
    from scripts.sqlite_board_probe_monitor import monitor
except ModuleNotFoundError:
    from sqlite_board_probe_monitor import monitor

try:
    from scripts.sqlite_board_probe_restore import is_erased, load_manifest, restore
except ModuleNotFoundError:
    from sqlite_board_probe_restore import is_erased, load_manifest, restore


def esptool(*args, **kwargs) -> None:
    """调用 Flash 适配器，并保留入口层的命令可测试性。"""

    original_run = probe_io.run
    probe_io.run = run
    try:
        probe_io.esptool(*args, **kwargs)
    finally:
        probe_io.run = original_run


def read_layout(
    port: str,
    baud: int,
    destination: Path,
    *,
    before: str = "default-reset",
    after: str = "hard-reset",
) -> list[Partition]:
    """读取分区表，并让入口层可替换底层读操作进行契约测试。"""

    read_flash(
        port,
        baud,
        PARTITION_TABLE_OFFSET,
        PARTITION_TABLE_SIZE,
        destination,
        before=before,
        after=after,
    )
    return parse_partition_table(destination.read_bytes())


def inspect(args: argparse.Namespace) -> None:
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    partitions = read_layout(args.port, args.baud, output)
    validate_layout(partitions, args.data_label, args.probe_slot, args.expected_data_size)
    print(
        json.dumps(
            [asdict(partition) for partition in partitions],
            ensure_ascii=False,
            indent=2,
        )
    )


def backup(args: argparse.Namespace) -> None:
    directory = args.directory.resolve()
    if directory.exists() and any(directory.iterdir()):
        raise ProbeError(f"backup directory is not empty: {directory}")
    directory.mkdir(parents=True, exist_ok=True)

    table_path = directory / "partition-table.bin"
    partitions = read_layout(args.port, args.baud, table_path, after="no-reset")
    layout = validate_layout(partitions, args.data_label, args.probe_slot, args.expected_data_size)

    artifacts: dict[str, dict] = {}
    artifact_names = ("data", "probe_slot", "otadata")
    extra_labels = tuple(dict.fromkeys(getattr(args, "extra_label", [])))
    for label in extra_labels:
        if label in {"data", "probe_slot", "otadata"}:
            raise ProbeError(f"extra backup label duplicates required artifact: {label}")
        layout[label] = partition_by_label(partitions, label)
        artifact_names += (label,)
    for index, name in enumerate(artifact_names):
        partition = layout[name]
        path = directory / f"{partition.label}.bin"
        read_flash(
            args.port,
            args.baud,
            partition.offset,
            partition.size,
            path,
            before="no-reset",
            after="no-reset",
        )
        verify_flash(
            args.port,
            args.baud,
            partition.offset,
            path,
            before="no-reset",
            after="hard-reset" if index == len(artifact_names) - 1 else "no-reset",
        )
        artifacts[name] = {
            "file": path.name,
            "sha256": sha256(path),
            "partition": asdict(partition),
        }
        if name == "probe_slot":
            artifacts[name]["erased"] = is_erased(path)

    manifest = {
        "schema_version": 1,
        "chip": "esp32s3",
        "baud": args.baud,
        "partition_table": {
            "file": table_path.name,
            "sha256": sha256(table_path),
            "offset": PARTITION_TABLE_OFFSET,
            "size": PARTITION_TABLE_SIZE,
        },
        "artifacts": artifacts,
    }
    manifest_path = directory / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"PASS verified backup: {manifest_path}")


def write_probe(args: argparse.Namespace) -> None:
    if not args.yes or args.confirm_inactive_slot != args.probe_slot:
        raise ProbeError("probe write requires --yes and an exact --confirm-inactive-slot")
    directory = args.backup_directory.resolve()
    manifest = load_manifest(directory)
    slot_artifact = manifest["artifacts"]["probe_slot"]
    slot = Partition(**slot_artifact["partition"])
    if slot.label != args.probe_slot:
        raise ProbeError("probe slot does not match backup manifest")
    image = args.binary.resolve()
    if not image.is_file() or image.stat().st_size > slot.size:
        raise ProbeError("probe image is missing or too large for the selected OTA slot")

    idf_path_value = os.environ.get("IDF_PATH")
    if not idf_path_value:
        raise ProbeError("IDF_PATH is not set; load the ESP-IDF environment before writing Flash")
    idf_path = Path(idf_path_value)
    otatool = idf_path / "components" / "app_update" / "otatool.py"
    if not otatool.is_file():
        raise ProbeError("IDF_PATH does not point to an ESP-IDF installation containing otatool.py")

    table_info = manifest["partition_table"]
    with tempfile.TemporaryDirectory(prefix="voicelife-write-check-") as temporary:
        current_table = Path(temporary) / "partition-table.bin"
        read_flash(
            args.port,
            args.baud,
            table_info["offset"],
            table_info["size"],
            current_table,
        )
        if sha256(current_table) != table_info["sha256"]:
            raise ProbeError("board partition table changed since backup; refusing probe write")

    esptool(
        args.port,
        args.baud,
        ["write-flash", hex(slot.offset), str(image)],
        after="no-reset",
    )
    verify_flash(args.port, args.baud, slot.offset, image, before="no-reset", after="hard-reset")

    run(
        [
            sys.executable,
            str(otatool),
            "--port",
            args.port,
            "--baud",
            str(args.baud),
            "switch_ota_partition",
            "--name",
            slot.label,
        ]
    )
    print(f"PASS probe image verified and boot switched to {slot.label}")


def add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=stable_baud, default=STABLE_BAUD)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    inspect_parser = subparsers.add_parser("inspect", help="读取并校验板上分区表")
    add_common(inspect_parser)
    inspect_parser.add_argument("--output", type=Path, required=True)
    inspect_parser.add_argument("--data-label", default="voicelife")
    inspect_parser.add_argument("--probe-slot", default="ota_1")
    inspect_parser.add_argument("--expected-data-size", type=lambda value: int(value, 0), default=0x200000)
    inspect_parser.set_defaults(handler=inspect)

    backup_parser = subparsers.add_parser("backup", help="备份并用芯片摘要复核数据和 OTA 元数据")
    add_common(backup_parser)
    backup_parser.add_argument("--directory", type=Path, required=True)
    backup_parser.add_argument("--data-label", default="voicelife")
    backup_parser.add_argument("--probe-slot", default="ota_1")
    backup_parser.add_argument("--expected-data-size", type=lambda value: int(value, 0), default=0x200000)
    backup_parser.add_argument(
        "--extra-label",
        action="append",
        default=[],
        help="额外只读备份的分区标签，可重复；不会参与 restore 写回",
    )
    backup_parser.set_defaults(handler=backup)

    write_parser = subparsers.add_parser("write-probe", help="将探针写入已人工确认的非活动 OTA 槽")
    add_common(write_parser)
    write_parser.add_argument("--backup-directory", type=Path, required=True)
    write_parser.add_argument("--probe-slot", default="ota_1")
    write_parser.add_argument("--confirm-inactive-slot", required=True)
    write_parser.add_argument("--binary", type=Path, required=True)
    write_parser.add_argument("--yes", action="store_true")
    write_parser.set_defaults(handler=write_probe)

    monitor_parser = subparsers.add_parser("monitor", help="监视探针并在两个故障点注入 EN 复位")
    add_common(monitor_parser)
    monitor_parser.add_argument("--timeout", type=int, default=240)
    monitor_parser.set_defaults(handler=monitor)

    restore_parser = subparsers.add_parser("restore", help="校验后恢复数据、原 OTA 槽和 OTA 元数据")
    add_common(restore_parser)
    restore_parser.add_argument("--directory", type=Path, required=True)
    restore_parser.add_argument("--yes", action="store_true")
    restore_parser.set_defaults(handler=restore)

    args = parser.parse_args()
    try:
        args.handler(args)
    except (ProbeError, OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"ERROR {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
