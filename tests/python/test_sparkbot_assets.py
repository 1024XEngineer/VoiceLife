from __future__ import annotations

import hashlib
import json
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ASSET_DIR = ROOT / "components" / "voicelife_display_esp" / "assets" / "esp-sparkbot"
MANIFEST = ASSET_DIR / "manifest.json"
GIF_DIR = ASSET_DIR / "mascot" / "gifs"

ALLOWED_ASSET_KEYS = {
    "asset_id",
    "file",
    "width",
    "height",
    "frame_count",
    "duration_ms",
    "size_bytes",
    "sha256",
}


class GifParseError(ValueError):
    """GIF 块结构损坏。"""


def parse_gif(data: bytes) -> tuple[int, int, int, list[int]]:
    """解析 GIF 元数据。

    返回 (width, height, frame_count, 各帧 duration 毫秒列表)。
    不依赖第三方库；块结构损坏时抛出 GifParseError。
    """
    if len(data) < 13:
        raise GifParseError("GIF 数据过短")
    if data[:6] not in (b"GIF87a", b"GIF89a"):
        raise GifParseError(f"非法 GIF 签名: {data[:6]!r}")
    width, height = struct.unpack("<HH", data[6:10])
    packed = data[10]
    pos = 13
    if packed & 0x80:  # 全局颜色表
        pos += 3 * (1 << ((packed & 0x07) + 1))
    frames = 0
    durations: list[int] = []
    while pos < len(data):
        marker = data[pos]
        if marker == 0x3B:  # trailer
            return width, height, frames, durations
        if marker == 0x21:  # 扩展块
            label = data[pos + 1]
            if label == 0xF9:  # 图形控制扩展：帧延迟
                if pos + 8 > len(data) or data[pos + 2] != 4 or data[pos + 7] != 0:
                    raise GifParseError("损坏的 GCE 块")
                # 延迟单位是厘秒（1/100s），转毫秒与 manifest/PIL 对齐。
                durations.append(struct.unpack("<H", data[pos + 4 : pos + 6])[0] * 10)
            p = pos + 2
            while p < len(data):
                n = data[p]
                p += 1
                if n == 0:
                    break
                p += n
            else:
                raise GifParseError("扩展子块未终止")
            pos = p
        elif marker == 0x2C:  # 图像描述符
            frames += 1
            if pos + 10 > len(data):
                raise GifParseError("图像描述符不完整")
            packed2 = data[pos + 9]
            p = pos + 10
            if packed2 & 0x80:  # 局部颜色表
                p += 3 * (1 << ((packed2 & 0x07) + 1))
            if p >= len(data):
                raise GifParseError("缺少 LZW 最小码字字节")
            p += 1  # LZW min code size
            while p < len(data):
                n = data[p]
                p += 1
                if n == 0:
                    break
                p += n
            else:
                raise GifParseError("图像数据子块未终止")
            pos = p
        else:
            raise GifParseError(f"未知块标记 0x{marker:02x} @ {pos}")
    raise GifParseError("GIF 缺少 trailer (0x3B)")


class SparkBotAssetManifestTest(unittest.TestCase):
    def setUp(self) -> None:
        self.manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
        self.assets = self.manifest["assets"]

    def test_manifest_schema(self) -> None:
        self.assertEqual(self.manifest["schema_version"], 1)
        self.assertEqual(self.manifest["board_id"], "esp-sparkbot")
        loading = self.manifest["loading"]
        self.assertEqual(loading["mode"], "embedded_manifest_only")
        self.assertFalse(loading["allow_network_url"])
        self.assertFalse(loading["allow_arbitrary_path"])
        self.assertFalse(loading["allow_arbitrary_bytes"])
        self.assertIn("license", self.manifest["source"])
        self.assertEqual(self.manifest["source"]["license"], "MIT")
        self.assertTrue(self.manifest["source"]["upstream_commit"])
        self.assertEqual(len(self.assets), 10)
        self.assertEqual(self.manifest["budget"]["total_bytes"], 142683)

    def test_asset_ids_unique_and_controlled(self) -> None:
        ids = [entry["asset_id"] for entry in self.assets]
        self.assertEqual(len(ids), len(set(ids)), "asset_id 必须唯一")
        for asset_id in ids:
            self.assertTrue(asset_id, "asset_id 不得为空")
            self.assertNotIn("/", asset_id)
            self.assertNotIn("\\", asset_id)
            self.assertNotIn("..", asset_id)

    def test_no_url_or_path_fields(self) -> None:
        for entry in self.assets:
            self.assertNotIn("url", entry)
            self.assertNotIn("path", entry)
            self.assertNotIn("remote", entry)
            self.assertEqual(set(entry.keys()), ALLOWED_ASSET_KEYS, "资源条目键集合必须与清单 schema 完全一致")

    def test_file_is_plain_name(self) -> None:
        for entry in self.assets:
            name = entry["file"]
            self.assertNotIn("/", name)
            self.assertNotIn("\\", name)
            self.assertNotIn("..", name)
            self.assertTrue(name.endswith(".gif"), "只允许 .gif 资源")

    def test_assets_match_disk(self) -> None:
        for entry in self.assets:
            path = GIF_DIR / entry["file"]
            self.assertTrue(path.is_file(), f"清单引用的文件必须存在: {entry['file']}")
            data = path.read_bytes()
            width, height, frames, durations = parse_gif(data)
            self.assertEqual((width, height), (entry["width"], entry["height"]), f"{entry['file']} 尺寸与清单不一致")
            self.assertEqual(frames, entry["frame_count"], f"{entry['file']} 帧数与清单不一致")
            self.assertEqual(sum(durations), entry["duration_ms"], f"{entry['file']} 动画总时长与清单不一致")
            self.assertEqual(len(data), entry["size_bytes"], f"{entry['file']} 大小与清单不一致")
            self.assertEqual(hashlib.sha256(data).hexdigest(), entry["sha256"], f"{entry['file']} SHA-256 与清单不一致")

    def test_sha256_is_strict_hex(self) -> None:
        for entry in self.assets:
            sha = entry["sha256"]
            self.assertEqual(len(sha), 64)
            self.assertTrue(all(c in "0123456789abcdef" for c in sha))

    def test_pil_can_open(self) -> None:
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("Pillow 不可用，跳过官方解码器可打开性检查")
        for entry in self.assets:
            with Image.open(GIF_DIR / entry["file"]) as im:
                self.assertEqual(im.size, (entry["width"], entry["height"]), f"{entry['file']} PIL 尺寸与清单不一致")
                self.assertEqual(
                    getattr(im, "n_frames", 1), entry["frame_count"], f"{entry['file']} PIL 帧数与清单不一致"
                )

    def test_assets_image_matches_official_format(self) -> None:
        """assets 镜像必须与小智 pack_assets_simple 字节级一致。"""
        import subprocess
        import tempfile

        def official_pack(gif_dir: Path) -> bytes:
            """xiaozhi-esp32@37d1aee scripts/build_default_assets.py pack_assets_simple 算法副本。"""
            merged = bytearray()
            infos: list[tuple[str, int, int]] = []
            for path in sorted(gif_dir.iterdir()):
                if path.suffix.lower() != ".gif":
                    continue
                data = path.read_bytes()
                infos.append((path.name, len(merged), len(data)))
                merged.extend(b"\x5a" * 2)
                merged.extend(data)
            table = bytearray()
            for name, offset, size in infos:
                fixed = name.encode("utf-8")[:32].ljust(32, b"\x00")
                table.extend(fixed)
                table.extend(struct.pack("<IIHH", size, offset, 0, 0))
            combined = bytes(table) + bytes(merged)
            header = struct.pack("<III", len(infos), sum(combined) & 0xFFFF, len(combined))
            return header + combined

        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "assets.bin"
            subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "scripts" / "build_sparkbot_assets.py"),
                    "--gif-dir",
                    str(GIF_DIR),
                    "--output",
                    str(out),
                ],
                check=True,
                capture_output=True,
            )
            ours = out.read_bytes()
        official = official_pack(GIF_DIR)
        self.assertEqual(ours, official, "assets 镜像必须与小智 pack_assets_simple 字节级一致")

    def test_gif_sentinel_and_truncation(self) -> None:
        """解码器必须在 trailer 0x3B 停止；截断/越界数据必须拒绝。"""
        for entry in self.assets:
            data = (GIF_DIR / entry["file"]).read_bytes()
            # 尾部附加非法哨兵（0x06），正常解码不受影响（解码停在 trailer）。
            width, height, frames, durations = parse_gif(data + b"\x06" * 32)
            self.assertEqual(frames, entry["frame_count"])
            self.assertEqual(sum(durations), entry["duration_ms"])
            # 截断数据（去掉 trailer 或尾部子块）必须报结构错误，不允许越界读取。
            for cut in (len(data) - 1, len(data) - 4, max(1, len(data) // 2)):
                with self.assertRaises(GifParseError, msg=f"{entry['file']} 截断 {cut} 字节必须拒绝"):
                    parse_gif(data[:cut])
            # 首帧尺寸与全部分帧一致（无半图尺寸漂移）。
            self.assertEqual((width, height), (96, 96), f"{entry['file']} 尺寸必须 96x96")


if __name__ == "__main__":
    unittest.main(verbosity=2)
