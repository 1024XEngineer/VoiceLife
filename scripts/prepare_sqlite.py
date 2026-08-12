#!/usr/bin/env python3
"""准备项目统一使用的、固定版本 SQLite amalgamation。"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "third_party" / "sqlite3"
SQLITE_VERSION = "3.53.4"
SQLITE_ARCHIVE = "sqlite-amalgamation-3530400.zip"
SQLITE_URL = f"https://www.sqlite.org/2026/{SQLITE_ARCHIVE}"
SQLITE_ARCHIVE_SHA256 = "1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d"
SQLITE_C_SHA256 = "3fefe3dd640247a3239b95587418127d0a0c24d2620130b4bc3fea9ddf89142c"
SQLITE_H_SHA256 = "919e7f2e8ed1d8f56ac17b412b8971c76aa5d1a879752cc6058f75e7d5910e1d"


def sha256(path: Path) -> str:
    """@brief 计算文件的 SHA-256 摘要。

    @param path 要读取的文件路径。
    @return 小写十六进制摘要字符串。
    """
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fetch(destination: Path) -> None:
    """@brief 下载并校验固定版本的 SQLite 压缩包。

    @param destination 下载压缩包的临时路径。
    @return 无；摘要不匹配时抛出异常。
    """
    with urllib.request.urlopen(SQLITE_URL, timeout=60) as response:
        destination.write_bytes(response.read())
    actual = sha256(destination)
    if actual != SQLITE_ARCHIVE_SHA256:
        raise RuntimeError(f"SQLite 压缩包摘要不匹配：{actual}")


def extract(archive: Path) -> None:
    """@brief 将压缩包中的 amalgamation 文件解压到统一第三方目录。

    @param archive 已校验的 SQLite 压缩包路径。
    @return 无。
    """
    prefix = "sqlite-amalgamation-3530400"
    with zipfile.ZipFile(archive) as source:
        for name in ("sqlite3.c", "sqlite3.h"):
            member = f"{prefix}/{name}"
            (COMPONENT / name).write_bytes(source.read(member))


def apply_esp_idf_patch() -> None:
    """@brief 应用 ESP-IDF unix-none VFS 所需的兼容性补丁。

    @return 无；补丁无法应用时由 subprocess 抛出异常。
    """
    patch = COMPONENT / "sqlite3-esp-idf.patch"
    subprocess.run(
        ["patch", "--batch", "--forward", "-p0"],
        cwd=COMPONENT,
        input=patch.read_bytes(),
        check=True,
    )


def install_component_manifest() -> None:
    """@brief 安装 ESP-IDF 组件清单模板生成的 CMakeLists。

    @return 无。
    """
    shutil.copy2(COMPONENT / "CMakeLists.txt.template", COMPONENT / "CMakeLists.txt")


def verify_output() -> None:
    """@brief 校验补丁后的源码摘要，防止来源或补丁漂移。

    @return 无；任一摘要不匹配时抛出异常。
    """
    expected = {"sqlite3.c": SQLITE_C_SHA256, "sqlite3.h": SQLITE_H_SHA256}
    for name, digest in expected.items():
        actual = sha256(COMPONENT / name)
        if actual != digest:
            raise RuntimeError(f"{name} 摘要不匹配：{actual}")


def main() -> int:
    """@brief 下载、补丁、生成并校验项目统一 SQLite 组件。

    @return 成功时返回 0；失败异常交给命令行调用方处理。
    """
    COMPONENT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="voicelife-sqlite-") as directory:
        archive = Path(directory) / SQLITE_ARCHIVE
        fetch(archive)
        extract(archive)
    apply_esp_idf_patch()
    install_component_manifest()
    verify_output()
    print(f"PASS SQLite {SQLITE_VERSION} 已准备到 {COMPONENT}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        print(f"SQLite 准备失败：{error}", file=sys.stderr)
        raise SystemExit(1) from error
