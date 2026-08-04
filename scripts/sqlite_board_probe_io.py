"""SQLite 实板探针的串口 Flash 读写适配。"""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys

try:
    from scripts.sqlite_board_probe_protocol import (
        PARTITION_TABLE_OFFSET,
        PARTITION_TABLE_SIZE,
        Partition,
        parse_partition_table,
    )
except ModuleNotFoundError:
    from sqlite_board_probe_protocol import (
        PARTITION_TABLE_OFFSET,
        PARTITION_TABLE_SIZE,
        Partition,
        parse_partition_table,
    )


def run(command: list[str]) -> None:
    """打印并执行一条可审计的板级命令。"""

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
    """用兼容 esptool 4/5 的命令名执行 Flash 操作。"""

    compatible_operation = [operation[0].replace("-", "_"), *operation[1:]]
    run(
        [
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
        ]
    )


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
    """读取一段 Flash 到本地备份文件。"""

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
    """用芯片回读摘要校验本地镜像。"""

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
    """读取并解析板上的分区表。"""

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
