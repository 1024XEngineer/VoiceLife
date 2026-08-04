#!/usr/bin/env python3
"""Fetch the pinned SQLite amalgamation used by the board probe."""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
COMPONENT = ROOT / "components" / "sqlite3"
SQLITE_VERSION = "3.53.4"
SQLITE_ARCHIVE = "sqlite-amalgamation-3530400.zip"
SQLITE_URL = f"https://www.sqlite.org/2026/{SQLITE_ARCHIVE}"
SQLITE_ARCHIVE_SHA256 = "1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d"
SQLITE_C_SHA256 = "3fefe3dd640247a3239b95587418127d0a0c24d2620130b4bc3fea9ddf89142c"
SQLITE_H_SHA256 = "919e7f2e8ed1d8f56ac17b412b8971c76aa5d1a879752cc6058f75e7d5910e1d"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fetch(destination: Path) -> None:
    with urllib.request.urlopen(SQLITE_URL, timeout=60) as response:
        destination.write_bytes(response.read())
    actual = sha256(destination)
    if actual != SQLITE_ARCHIVE_SHA256:
        raise RuntimeError(f"SQLite archive digest mismatch: {actual}")


def extract(archive: Path) -> None:
    prefix = "sqlite-amalgamation-3530400"
    with zipfile.ZipFile(archive) as source:
        for name in ("sqlite3.c", "sqlite3.h"):
            member = f"{prefix}/{name}"
            (COMPONENT / name).write_bytes(source.read(member))


def apply_esp_idf_patch() -> None:
    patch = (COMPONENT / "sqlite3-esp-idf.patch").read_bytes()
    subprocess.run(
        ["patch", "--batch", "--forward", "-p0"],
        cwd=COMPONENT,
        input=patch,
        check=True,
    )


def verify_output() -> None:
    expected = {"sqlite3.c": SQLITE_C_SHA256, "sqlite3.h": SQLITE_H_SHA256}
    for name, digest in expected.items():
        actual = sha256(COMPONENT / name)
        if actual != digest:
            raise RuntimeError(f"{name} digest mismatch after patch: {actual}")


def main() -> int:
    COMPONENT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="voicelife-sqlite-") as directory:
        archive = Path(directory) / SQLITE_ARCHIVE
        fetch(archive)
        extract(archive)
    apply_esp_idf_patch()
    shutil.copy2(COMPONENT / "CMakeLists.txt.template", COMPONENT / "CMakeLists.txt")
    verify_output()
    print(f"PASS SQLite {SQLITE_VERSION} prepared in {COMPONENT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
