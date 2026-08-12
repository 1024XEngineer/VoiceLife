#!/ usr / bin / env python3
"""Generate the ESP-SparkBot assets partition image (official xiaozhi format).

直接移植 xiaozhi-esp32@37d1aee scripts/build_default_assets.py 的
pack_assets_simple 打包逻辑（12B 头 + 44B/项文件表 + "ZZ" magic 数据 +
checksum），只打包 SparkBot 牛头 emoji GIF；字体等资源后续阶段接入。
生成后回读校验（与 SparkBotEmojiAssets 解析器逻辑一致）并输出 SHA-256。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

HEADER_BYTES = 12
TABLE_ENTRY_BYTES = 44  # name(32) + size(4) + offset(4) + width(2) + height(2)
MAGIC = b"\x5a\x5a"  # "ZZ"
MAX_NAME = 32


class AssetPackError(ValueError):
    """资产打包或回读校验失败。"""


def compute_checksum(data: bytes) -> int:
    return sum(data) & 0xFFFF


def pack_assets(gif_dir: Path, out_path: Path) -> tuple[int, bytes]:
    """按官方格式打包 GIF 目录为 assets 镜像。

    返回 (文件数, 完整镜像字节)。镜像布局：
      [0..4)   total_files  uint32 LE
      [4..8)   checksum     uint32 LE（sum(表+数据) & 0xFFFF）
      [8..12)  combined_len uint32 LE
      [12..)   文件表（44B/项）+ 数据（每文件前 "ZZ" magic）
    """
    files = sorted(p for p in gif_dir.iterdir() if p.suffix.lower() == ".gif")
    if not files:
        raise AssetPackError(f"{gif_dir} 中没有 .gif 文件")

    merged = bytearray()
    table: list[tuple[str, int, int]] = []
    for path in files:
        name = path.name
        data = path.read_bytes()
        table.append((name, len(merged), len(data)))
        merged.extend(MAGIC)
        merged.extend(data)

    mmap_table = bytearray()
    for name, offset, size in table:
        fixed = name.encode("utf-8")[:MAX_NAME].ljust(MAX_NAME, b"\x00")
        if len(fixed) != MAX_NAME:
            raise AssetPackError(f"文件名超长: {name}")
        mmap_table.extend(fixed)
        mmap_table.extend(struct.pack("<IIHH", size, offset, 0, 0))

    combined = bytes(mmap_table) + bytes(merged)
    header = struct.pack(
        "<III",
        len(table),
        compute_checksum(combined),
        len(combined),
    )
    image = header + combined
    return len(table), image


def verify_image(image: bytes, expected_files: int, gif_dir: Path) -> None:
    """回读校验镜像（与 SparkBotEmojiAssets 解析一致）。"""
    if len(image) < HEADER_BYTES:
        raise AssetPackError("镜像过短")
    total_files, checksum, combined_len = struct.unpack("<III", image[:HEADER_BYTES])
    if total_files != expected_files:
        raise AssetPackError(f"文件数不匹配: {total_files} != {expected_files}")
    if combined_len > len(image) - HEADER_BYTES:
        raise AssetPackError("combined_len 超出镜像范围")
    combined = image[HEADER_BYTES : HEADER_BYTES + combined_len]
    if compute_checksum(combined) != checksum:
        raise AssetPackError("校验和不匹配")
    table_bytes = total_files * TABLE_ENTRY_BYTES
    if len(combined) < table_bytes:
        raise AssetPackError("文件表越界")
    seen: set[str] = set()
    for i in range(total_files):
        entry = combined[i * TABLE_ENTRY_BYTES : (i + 1) * TABLE_ENTRY_BYTES]
        name = entry[:MAX_NAME].split(b"\x00", 1)[0].decode("utf-8", errors="replace")
        size, offset = struct.unpack("<II", entry[MAX_NAME : MAX_NAME + 8])
        if name in seen:
            raise AssetPackError(f"重复文件名: {name}")
        seen.add(name)
        data_start = table_bytes + offset
        if data_start + 2 + size > len(combined):
            raise AssetPackError(f"{name} 数据越界")
        if combined[data_start : data_start + 2] != MAGIC:
            raise AssetPackError(f"{name} 缺少 ZZ magic")
        disk = (gif_dir / name).read_bytes()
        if disk != combined[data_start + 2 : data_start + 2 + size]:
            raise AssetPackError(f"{name} 内容与磁盘不一致")
    if len(seen) != expected_files:
        raise AssetPackError("文件表条目与磁盘文件不一致")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gif-dir", required=True, type=Path, help="牛头 emoji GIF 目录")
    parser.add_argument("--output", required=True, type=Path, help="生成的 assets 镜像路径")
    parser.add_argument("--manifest", type=Path, help="可选的 manifest.json（用于记录资源包 SHA-256）")
    args = parser.parse_args()

    try:
        count, image = pack_assets(args.gif_dir, args.output)
        verify_image(image, count, args.gif_dir)
    except AssetPackError as error:
        print(f"FAIL {error}", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    sha = hashlib.sha256(image).hexdigest()
    print(f"PASS {count} 个 GIF -> {args.output} sha256={sha}")

    if args.manifest is not None:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
        budget = manifest.setdefault("budget", {})
        budget["assets_image_sha256"] = sha
        budget["assets_image_bytes"] = len(image)
        args.manifest.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"manifest budget 已记录: assets_image_sha256={sha}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
