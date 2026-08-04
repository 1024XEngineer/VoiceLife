"""SQLite 实板探针使用的分区布局与复位协议。"""

from __future__ import annotations

import argparse
import hashlib
import struct
from dataclasses import dataclass
from pathlib import Path

PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_SIZE = 0x1000
PARTITION_MAGIC = 0x50AA
PARTITION_MD5_MAGIC = 0xEBEB
PARTITION_ENTRY = struct.Struct("<HBBII16sI")
STABLE_BAUD = 115200
EXPECTED_RESET_POINTS = ("OPEN_TRANSACTION", "AFTER_COMMIT")
EXPECTED_PHASES = (0, 1, 2)


class ProbeError(RuntimeError):
    """表示实板探针协议或安全前置条件不满足。"""


def stable_baud(value: str) -> int:
    """只接受已在该板 USB 串口上验证稳定的波特率。"""

    baud = int(value, 0)
    if baud != STABLE_BAUD:
        raise argparse.ArgumentTypeError("该板 USB 串口只允许使用已验证的 115200")
    return baud


def sha256(path: Path) -> str:
    """计算备份文件摘要，用于恢复前后的一致性校验。"""

    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


@dataclass(frozen=True)
class Partition:
    """描述一条 ESP 分区表记录。"""

    label: str
    type: int
    subtype: int
    offset: int
    size: int
    flags: int


class ResetSequence:
    """校验固件阶段与主机 EN 复位点的严格顺序。"""

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


def parse_partition_table(content: bytes) -> list[Partition]:
    """解析并校验 ESP 分区表记录。"""

    partitions: list[Partition] = []
    terminated_at: int | None = None
    for position in range(0, len(content) - PARTITION_ENTRY.size + 1, PARTITION_ENTRY.size):
        entry = content[position : position + PARTITION_ENTRY.size]
        magic = struct.unpack_from("<H", entry)[0]
        if magic == 0xFFFF:
            terminated_at = position
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
    if terminated_at is not None and content[terminated_at:] != b"\xff" * (len(content) - terminated_at):
        raise ProbeError("partition table has non-FF bytes after its terminator")
    if not partitions:
        raise ProbeError("partition table contains no entries")
    labels = [partition.label for partition in partitions]
    if len(labels) != len(set(labels)):
        raise ProbeError("partition labels are not unique")
    return partitions


def partition_by_label(partitions: list[Partition], label: str) -> Partition:
    """按标签查找唯一分区。"""

    matches = [partition for partition in partitions if partition.label == label]
    if len(matches) != 1:
        raise ProbeError(f"expected exactly one partition named {label}")
    return matches[0]


def validate_layout(
    partitions: list[Partition], data_label: str, ota_slot: str, expected_data_size: int
) -> dict[str, Partition]:
    """校验本次探针允许访问的数据分区、OTA 槽和 OTA 元数据。"""

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
