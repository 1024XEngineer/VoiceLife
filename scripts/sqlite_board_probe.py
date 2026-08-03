#!/usr/bin/env python3
"""Inspect, back up, exercise, and restore the SQLite board probe safely."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_SIZE = 0x1000
PARTITION_MAGIC = 0x50AA
PARTITION_MD5_MAGIC = 0xEBEB
PARTITION_ENTRY = struct.Struct("<HBBII16sI")
STABLE_BAUD = 115200
EXPECTED_RESET_POINTS = ("OPEN_TRANSACTION", "AFTER_COMMIT")
EXPECTED_PHASES = (0, 1, 2)


class ProbeError(RuntimeError):
    pass


@dataclass(frozen=True)
class Partition:
    label: str
    type: int
    subtype: int
    offset: int
    size: int
    flags: int


class ResetSequence:
    def __init__(self) -> None:
        self.reset_points: list[str] = []
        self.phases: list[int] = []
        self.image: str | None = None

    def observe(self, line: str) -> str | None:
        if "PROBE_RESULT: FAIL" in line:
            raise ProbeError("board probe reported failure")
        if "ESP_ERROR_CHECK failed" in line or "abort() was called" in line:
            raise ProbeError("board probe firmware aborted")
        if "PROBE_PHASE:" in line:
            fields = {}
            for token in line.split("PROBE_PHASE:", 1)[1].strip().split():
                key, separator, value = token.partition("=")
                if separator:
                    fields[key] = value
            try:
                phase = int(fields["phase"])
                image = fields["image"]
            except (KeyError, ValueError) as error:
                raise ProbeError("malformed probe phase marker") from error

            expected_index = len(self.phases)
            if expected_index >= len(EXPECTED_PHASES) or phase != EXPECTED_PHASES[expected_index]:
                raise ProbeError(f"unexpected probe phase: {phase}")
            if phase != len(self.reset_points):
                raise ProbeError(f"probe phase {phase} was not preceded by the required reset")
            if self.image is None:
                self.image = image
            elif image != self.image:
                raise ProbeError("probe image changed between resets")
            self.phases.append(phase)
        if "HOST_RESET_POINT:" in line:
            point = line.split("HOST_RESET_POINT:", 1)[1].strip()
            expected_index = len(self.reset_points)
            if expected_index >= len(EXPECTED_RESET_POINTS) or point != EXPECTED_RESET_POINTS[expected_index]:
                raise ProbeError(f"unexpected reset point order: {point}")
            if self.phases != list(EXPECTED_PHASES[: expected_index + 1]):
                raise ProbeError(f"reset point {point} appeared outside its probe phase")
            self.reset_points.append(point)
            return "reset"
        if "PROBE_RESULT: PASS" in line:
            if tuple(self.reset_points) != EXPECTED_RESET_POINTS:
                raise ProbeError(f"probe passed without required resets: {self.reset_points}")
            if tuple(self.phases) != EXPECTED_PHASES:
                raise ProbeError(f"probe passed without required phases: {self.phases}")
            return "pass"
        return None


def stable_baud(value: str) -> int:
    baud = int(value, 0)
    if baud != STABLE_BAUD:
        raise argparse.ArgumentTypeError("该板 USB 串口只允许使用已验证的 115200")
    return baud


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_partition_table(content: bytes) -> list[Partition]:
    partitions: list[Partition] = []
    for position in range(0, len(content) - PARTITION_ENTRY.size + 1, PARTITION_ENTRY.size):
        entry = content[position : position + PARTITION_ENTRY.size]
        magic = struct.unpack_from("<H", entry)[0]
        if magic == 0xFFFF:
            break
        if magic == PARTITION_MD5_MAGIC:
            continue
        if magic != PARTITION_MAGIC:
            raise ProbeError(f"invalid partition entry magic 0x{magic:04x} at 0x{position:x}")
        _, type_value, subtype, offset, size, raw_label, flags = PARTITION_ENTRY.unpack(entry)
        try:
            label = raw_label.split(b"\0", 1)[0].decode("ascii")
        except UnicodeDecodeError as error:
            raise ProbeError(f"partition label at 0x{position:x} is not ASCII") from error
        partitions.append(Partition(label, type_value, subtype, offset, size, flags))
    if not partitions:
        raise ProbeError("partition table contains no entries")
    labels = [partition.label for partition in partitions]
    if len(labels) != len(set(labels)):
        raise ProbeError("partition labels are not unique")
    return partitions


def partition_by_label(partitions: list[Partition], label: str) -> Partition:
    matches = [partition for partition in partitions if partition.label == label]
    if len(matches) != 1:
        raise ProbeError(f"expected exactly one partition named {label}")
    return matches[0]


def run(command: list[str]) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, check=True)


def esptool(
    port: str,
    baud: int,
    operation: list[str],
    *,
    before: str = "default-reset",
    after: str = "hard-reset",
) -> None:
    # esptool 5 keeps these legacy spellings as aliases; esptool 4 only accepts
    # them, so use the common command surface for board recovery tooling.
    compatible_operation = [operation[0].replace("-", "_"), *operation[1:]]
    run([
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "--port",
        port,
        "--baud",
        str(baud),
        "--before",
        before.replace("-", "_"),
        "--after",
        after.replace("-", "_"),
        *compatible_operation,
    ])


def read_flash(
    port: str,
    baud: int,
    offset: int,
    size: int,
    destination: Path,
    *,
    before: str = "default-reset",
    after: str = "hard-reset",
) -> None:
    esptool(
        port,
        baud,
        ["read-flash", hex(offset), hex(size), str(destination)],
        before=before,
        after=after,
    )


def verify_flash(
    port: str,
    baud: int,
    offset: int,
    image: Path,
    *,
    before: str = "default-reset",
    after: str = "hard-reset",
) -> None:
    esptool(
        port,
        baud,
        ["verify-flash", hex(offset), str(image)],
        before=before,
        after=after,
    )


def read_layout(
    port: str,
    baud: int,
    destination: Path,
    *,
    before: str = "default-reset",
    after: str = "hard-reset",
) -> list[Partition]:
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


def validate_layout(partitions: list[Partition], data_label: str, ota_slot: str, expected_data_size: int) -> dict:
    data = partition_by_label(partitions, data_label)
    otadata = partition_by_label(partitions, "otadata")
    slot = partition_by_label(partitions, ota_slot)
    if data.type != 1 or data.size != expected_data_size:
        raise ProbeError(
            f"{data_label} must be a {expected_data_size}-byte data partition, got type={data.type} size={data.size}"
        )
    if otadata.type != 1 or otadata.subtype != 0 or otadata.size != 0x2000:
        raise ProbeError("otadata layout does not match the validated board")
    if slot.type != 0 or not 0x10 <= slot.subtype <= 0x1F:
        raise ProbeError(f"{ota_slot} is not an OTA application partition")
    return {"data": data, "otadata": otadata, "probe_slot": slot}


def inspect(args: argparse.Namespace) -> None:
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    partitions = read_layout(args.port, args.baud, output)
    validate_layout(partitions, args.data_label, args.probe_slot, args.expected_data_size)
    print(json.dumps([asdict(partition) for partition in partitions], ensure_ascii=False, indent=2))


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


def is_erased(path: Path) -> bool:
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            if chunk != b"\xff" * len(chunk):
                return False
    return True


def manifest_file(directory: Path, file_name: object, field: str) -> Path:
    if not isinstance(file_name, str) or not file_name or Path(file_name).name != file_name:
        raise ProbeError(f"invalid backup file for {field}")
    path = directory / file_name
    if not path.is_file():
        raise ProbeError(f"missing backup file for {field}: {path}")
    return path


def load_manifest(directory: Path) -> dict:
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

    idf_path_value = os.environ.get("IDF_PATH")
    if not idf_path_value:
        raise ProbeError("IDF_PATH is not set; load the ESP-IDF environment before writing Flash")
    idf_path = Path(idf_path_value)
    otatool = idf_path / "components" / "app_update" / "otatool.py"
    if not otatool.is_file():
        raise ProbeError("IDF_PATH does not point to an ESP-IDF installation containing otatool.py")

    esptool(
        args.port,
        args.baud,
        ["write-flash", hex(slot.offset), str(image)],
        after="no-reset",
    )
    verify_flash(args.port, args.baud, slot.offset, image, before="no-reset", after="hard-reset")

    run([
        sys.executable,
        str(otatool),
        "--port",
        args.port,
        "--baud",
        str(args.baud),
        "switch_ota_partition",
        "--name",
        slot.label,
    ])
    print(f"PASS probe image verified and boot switched to {slot.label}")


def reset_via_en(device) -> None:
    device.rts = True
    time.sleep(0.15)
    device.rts = False


def monitor(args: argparse.Namespace) -> None:
    try:
        import serial
    except ImportError as error:
        raise ProbeError("pyserial is required; use the ESP-IDF Python environment") from error

    device = serial.Serial(port=None, baudrate=args.baud, timeout=0.1)
    device.dtr = False
    device.rts = False
    device.port = args.port
    device.open()
    sequence = ResetSequence()
    deadline = time.monotonic() + args.timeout
    buffered = bytearray()
    try:
        while time.monotonic() < deadline:
            chunk = device.read(device.in_waiting or 1)
            if not chunk:
                continue
            buffered.extend(chunk)
            while b"\n" in buffered:
                raw_line, _, remainder = buffered.partition(b"\n")
                buffered = bytearray(remainder)
                line = raw_line.decode("utf-8", errors="replace").rstrip("\r")
                print(line, flush=True)
                action = sequence.observe(line)
                if action == "reset":
                    point = sequence.reset_points[-1]
                    print(f"HOST_ACTION: EN_RESET point={point}", flush=True)
                    reset_via_en(device)
                elif action == "pass":
                    print(f"HOST_RESULT: PASS resets={','.join(sequence.reset_points)}", flush=True)
                    return
    finally:
        device.close()
    raise ProbeError(f"probe timeout after reset points {sequence.reset_points}")


def restore(args: argparse.Namespace) -> None:
    if not args.yes:
        raise ProbeError("restore requires --yes")
    directory = args.directory.resolve()
    manifest = load_manifest(directory)
    table_info = manifest["partition_table"]

    with tempfile.TemporaryDirectory(prefix="voicelife-restore-") as temporary:
        current_table = Path(temporary) / "partition-table.bin"
        read_flash(
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
        esptool(
            args.port,
            args.baud,
            ["write-flash", hex(data_partition.offset), str(data_image)],
            before="no-reset",
            after="no-reset",
        )
        verify_flash(args.port, args.baud, data_partition.offset, data_image, before="no-reset", after="no-reset")

        esptool(
            args.port,
            args.baud,
            slot_operation,
            before="no-reset",
            after="no-reset",
        )
        verify_flash(args.port, args.baud, slot.offset, slot_image, before="no-reset", after="no-reset")

        otadata = manifest["artifacts"]["otadata"]
        otadata_partition = Partition(**otadata["partition"])
        otadata_image = directory / otadata["file"]
        esptool(
            args.port,
            args.baud,
            ["write-flash", hex(otadata_partition.offset), str(otadata_image)],
            before="no-reset",
            after="no-reset",
        )
        verify_flash(
            args.port,
            args.baud,
            otadata_partition.offset,
            otadata_image,
            before="no-reset",
            after="hard-reset",
        )
    print("PASS data, original probe slot, and OTA metadata restored with full verification")


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
