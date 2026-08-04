"""SQLite 实板探针备份清单校验与恢复编排。"""

from __future__ import annotations

import json
import tempfile
from pathlib import Path

try:
    import scripts.sqlite_board_probe_io as probe_io
    from scripts.sqlite_board_probe_protocol import (
        PARTITION_TABLE_OFFSET,
        PARTITION_TABLE_SIZE,
        STABLE_BAUD,
        Partition,
        ProbeError,
        parse_partition_table,
        sha256,
        validate_layout,
    )
except ModuleNotFoundError:
    import sqlite_board_probe_io as probe_io
    from sqlite_board_probe_protocol import (
        PARTITION_TABLE_OFFSET,
        PARTITION_TABLE_SIZE,
        STABLE_BAUD,
        Partition,
        ProbeError,
        parse_partition_table,
        sha256,
        validate_layout,
    )


def is_erased(path: Path) -> bool:
    """判断镜像文件是否仍然处于全 FF 擦除状态。"""

    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            if chunk != b"\xff" * len(chunk):
                return False
    return True


def manifest_file(directory: Path, file_name: object, field: str) -> Path:
    """校验备份清单中的文件名为安全的相对路径。"""

    if not isinstance(file_name, str) or not file_name or Path(file_name).name != file_name:
        raise ProbeError(f"invalid backup file for {field}")
    path = directory / file_name
    if path.is_symlink():
        raise ProbeError(f"backup file for {field} must not be a symlink")
    if not path.is_file():
        raise ProbeError(f"missing backup file for {field}: {path}")
    return path


def load_manifest(directory: Path) -> dict:
    """读取并完整校验备份清单及其本地镜像。"""

    manifest_path = directory / "manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ProbeError(f"cannot read backup manifest: {error}") from error
    if manifest.get("schema_version") != 1 or manifest.get("baud") != STABLE_BAUD:
        raise ProbeError("unsupported backup manifest")
    if manifest.get("chip") != "esp32s3":
        raise ProbeError("backup manifest is not for esp32s3")
    artifacts = manifest.get("artifacts")
    required_artifacts = ("data", "probe_slot", "otadata")
    if not isinstance(artifacts, dict):
        raise ProbeError("backup manifest has no artifacts")
    artifact_partitions: dict[str, Partition] = {}
    for name in required_artifacts:
        artifact = artifacts.get(name)
        if not isinstance(artifact, dict):
            raise ProbeError(f"backup manifest has no {name} artifact")
        try:
            partition = Partition(**artifact["partition"])
            expected_digest = artifact["sha256"]
        except (KeyError, TypeError) as error:
            raise ProbeError(f"invalid {name} artifact metadata") from error
        path = manifest_file(directory, artifact.get("file"), name)
        if path.stat().st_size != partition.size:
            raise ProbeError(f"backup size mismatch for {name}: {path}")
        if not isinstance(expected_digest, str) or sha256(path) != expected_digest:
            raise ProbeError(f"local backup digest mismatch: {path}")
        if name == "probe_slot":
            erased = artifact.get("erased")
            if not isinstance(erased, bool) or erased != is_erased(path):
                raise ProbeError("probe_slot erased flag does not match its backup image")
        artifact_partitions[name] = partition
    table = manifest.get("partition_table")
    if not isinstance(table, dict):
        raise ProbeError("backup manifest has no partition table")
    if table.get("offset") != PARTITION_TABLE_OFFSET or table.get("size") != PARTITION_TABLE_SIZE:
        raise ProbeError("backup partition table location is invalid")
    table_path = manifest_file(directory, table.get("file"), "partition_table")
    if table_path.stat().st_size != PARTITION_TABLE_SIZE:
        raise ProbeError("local partition table size mismatch")
    if not isinstance(table.get("sha256"), str) or sha256(table_path) != table["sha256"]:
        raise ProbeError("local partition table digest mismatch")
    backed_up_partitions = parse_partition_table(table_path.read_bytes())
    if artifact_partitions["otadata"].label != "otadata":
        raise ProbeError("otadata artifact has an invalid partition label")
    layout = validate_layout(
        backed_up_partitions,
        artifact_partitions["data"].label,
        artifact_partitions["probe_slot"].label,
        artifact_partitions["data"].size,
    )
    for name in required_artifacts:
        if artifact_partitions[name] != layout[name]:
            raise ProbeError(f"partition metadata mismatch for {name}")
    return manifest


def restore(args) -> None:
    """恢复数据、原 OTA 槽和 OTA 元数据，并在每次写入后芯片回读校验。"""

    if not args.yes:
        raise ProbeError("restore requires --yes")
    directory = args.directory.resolve()
    manifest = load_manifest(directory)
    table_info = manifest["partition_table"]

    with tempfile.TemporaryDirectory(prefix="voicelife-restore-") as temporary:
        current_table = Path(temporary) / "partition-table.bin"
        probe_io.read_flash(
            args.port,
            args.baud,
            table_info["offset"],
            table_info["size"],
            current_table,
            after="no-reset",
        )
        if sha256(current_table) != table_info["sha256"]:
            raise ProbeError("board partition table changed since backup; refusing restore")

        data = manifest["artifacts"]["data"]
        data_partition = Partition(**data["partition"])
        data_image = directory / data["file"]
        slot_artifact = manifest["artifacts"]["probe_slot"]
        slot = Partition(**slot_artifact["partition"])
        slot_image = directory / slot_artifact["file"]
        if slot_artifact["erased"]:
            slot_operation = ["erase-region", hex(slot.offset), hex(slot.size)]
        else:
            slot_operation = ["write-flash", hex(slot.offset), str(slot_image)]
        probe_io.esptool(
            args.port,
            args.baud,
            ["write-flash", hex(data_partition.offset), str(data_image)],
            before="no-reset",
            after="no-reset",
        )
        probe_io.verify_flash(
            args.port,
            args.baud,
            data_partition.offset,
            data_image,
            before="no-reset",
            after="no-reset",
        )

        probe_io.esptool(
            args.port,
            args.baud,
            slot_operation,
            before="no-reset",
            after="no-reset",
        )
        probe_io.verify_flash(
            args.port,
            args.baud,
            slot.offset,
            slot_image,
            before="no-reset",
            after="no-reset",
        )

        otadata = manifest["artifacts"]["otadata"]
        otadata_partition = Partition(**otadata["partition"])
        otadata_image = directory / otadata["file"]
        probe_io.esptool(
            args.port,
            args.baud,
            ["write-flash", hex(otadata_partition.offset), str(otadata_image)],
            before="no-reset",
            after="no-reset",
        )
        probe_io.verify_flash(
            args.port,
            args.baud,
            otadata_partition.offset,
            otadata_image,
            before="no-reset",
            after="hard-reset",
        )
    print("PASS data, original probe slot, and OTA metadata restored with full verification")
