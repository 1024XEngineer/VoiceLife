#!/ usr / bin / env python3
"""Generate the ESP-SparkBot assets partition image (official xiaozhi format).

直接移植 xiaozhi-esp32@37d1aee scripts/build_default_assets.py 的
pack_assets_simple 打包逻辑（12B 头 + 44B/项文件表 + "ZZ" magic 数据 +
checksum），打包 SparkBot 牛头 emoji GIF 与官方 common 14px 字体。
生成后回读校验（与 SparkBotEmojiAssets 解析器逻辑一致）并输出 SHA-256。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import tempfile
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


def _pack_wake_model(model_dir: Path) -> bytes:
    """按 ESP-SR 官方 pack_model.py 格式打包一个受控 WakeNet 模型目录。"""
    expected = {"_MODEL_INFO_", "wn9_index", "wn9_data"}
    files = sorted(path for path in model_dir.iterdir() if path.is_file())
    names = {path.name for path in files}
    if names != expected:
        raise AssetPackError(f"WakeNet 模型文件必须恰好是 {sorted(expected)}，实际 {sorted(names)}")
    model_name = model_dir.name.encode("ascii")
    if len(model_name) > MAX_NAME:
        raise AssetPackError("WakeNet 模型名称过长")
    header_size = 4 + MAX_NAME + 4 + len(files) * (MAX_NAME + 4 + 4)
    header = bytearray(struct.pack("<I", 1))
    header.extend(model_name.ljust(MAX_NAME, b"\0"))
    header.extend(struct.pack("<I", len(files)))
    data = bytearray()
    for path in files:
        payload = path.read_bytes()
        if not payload:
            raise AssetPackError(f"WakeNet 模型文件为空: {path.name}")
        name = path.name.encode("ascii")
        header.extend(name.ljust(MAX_NAME, b"\0"))
        header.extend(struct.pack("<II", header_size + len(data), len(payload)))
        data.extend(payload)
    if len(header) != header_size:
        raise AssetPackError("WakeNet 模型包头长度错误")
    return bytes(header + data)


def pack_assets(gif_dir: Path, common_font: Path, wake_model_dir: Path) -> tuple[int, bytes]:
    """按官方格式打包 GIF、common 字体和受控 WakeNet 模型为 assets 镜像。

    返回 (文件数, 完整镜像字节)。镜像布局：
      [0..4)   total_files  uint32 LE
      [4..8)   checksum     uint32 LE（sum(表+数据) & 0xFFFF）
      [8..12)  combined_len uint32 LE
      [12..)   文件表（44B/项）+ 数据（每文件前 "ZZ" magic）
    """
    files = sorted(p for p in gif_dir.iterdir() if p.suffix.lower() == ".gif")
    if not files:
        raise AssetPackError(f"{gif_dir} 中没有 .gif 文件")
    if not common_font.is_file() or common_font.stat().st_size == 0:
        raise AssetPackError(f"官方 common 字体不存在或为空: {common_font}")

    merged = bytearray()
    table: list[tuple[str, int, int, int, int]] = []
    for path in files:
        name = path.name
        data = path.read_bytes()
        if len(data) < 10 or data[:6] not in (b"GIF87a", b"GIF89a"):
            raise AssetPackError(f"{name} 不是合法 GIF")
        width, height = struct.unpack("<HH", data[6:10])
        if width != 96 or height != 96:
            raise AssetPackError(f"{name} 尺寸必须为 96x96，实际 {width}x{height}")
        table.append((name, len(merged), len(data), width, height))
        merged.extend(MAGIC)
        merged.extend(data)

    # 固定受控字体沿用官方 assets 容器，但不是图像，因此表内尺寸为 0。
    font_data = common_font.read_bytes()
    table.append((common_font.name, len(merged), len(font_data), 0, 0))
    merged.extend(MAGIC)
    merged.extend(font_data)

    model_data = _pack_wake_model(wake_model_dir)
    table.append(("srmodels.bin", len(merged), len(model_data), 0, 0))
    merged.extend(MAGIC)
    merged.extend(model_data)

    mmap_table = bytearray()
    for name, offset, size, width, height in table:
        fixed = name.encode("utf-8")[:MAX_NAME].ljust(MAX_NAME, b"\x00")
        if len(fixed) != MAX_NAME:
            raise AssetPackError(f"文件名超长: {name}")
        mmap_table.extend(fixed)
        mmap_table.extend(struct.pack("<IIHH", size, offset, width, height))

    combined = bytes(mmap_table) + bytes(merged)
    header = struct.pack(
        "<III",
        len(table),
        compute_checksum(combined),
        len(combined),
    )
    image = header + combined
    return len(table), image


def verify_image(image: bytes, expected_files: int, gif_dir: Path, common_font: Path, wake_model_dir: Path) -> None:
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
        if name == common_font.name:
            disk = common_font.read_bytes()
        elif name == "srmodels.bin":
            disk = _pack_wake_model(wake_model_dir)
        else:
            disk_path = gif_dir / name
            if not disk_path.is_file():
                raise AssetPackError(f"{name} 不在受控 GIF、字体或 WakeNet 模型集合中")
            disk = disk_path.read_bytes()
        if disk != combined[data_start + 2 : data_start + 2 + size]:
            raise AssetPackError(f"{name} 内容与磁盘不一致")
    if len(seen) != expected_files:
        raise AssetPackError("文件表条目与磁盘文件不一致")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gif-dir", required=True, type=Path, help="牛头 emoji GIF 目录")
    parser.add_argument("--common-font", required=True, type=Path, help="官方 Noto Sans common 14px CBIN")
    parser.add_argument("--wake-model-dir", required=True, type=Path, help="固定 ESP-SR WakeNet 模型目录")
    parser.add_argument("--output", required=True, type=Path, help="生成的 assets 镜像路径")
    parser.add_argument("--manifest", type=Path, help="可选的 manifest.json（用于记录资源包 SHA-256）")
    args = parser.parse_args()

    try:
        count, image = pack_assets(args.gif_dir, args.common_font, args.wake_model_dir)
        verify_image(image, count, args.gif_dir, args.common_font, args.wake_model_dir)
    except AssetPackError as error:
        print(f"FAIL {error}", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    sha = hashlib.sha256(image).hexdigest()
    print(f"PASS {count - 2} 个 GIF + common 14px 字体 + 官方 WakeNet -> {args.output} sha256={sha}")

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
