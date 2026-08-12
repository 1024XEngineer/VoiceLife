#!/usr/bin/env python3
"""兼容入口：调用仓库根目录的 SQLite 准备脚本。"""

from __future__ import annotations

import runpy
from pathlib import Path


def main() -> int:
    """@brief 转发到统一的 SQLite amalgamation 准备脚本。

    @return 根脚本的返回码；根脚本异常会原样向上传播。
    """
    root_script = Path(__file__).resolve().parents[3] / "scripts" / "prepare_sqlite.py"
    namespace = runpy.run_path(str(root_script), run_name="voicelife_prepare_sqlite")
    return int(namespace["main"]())


if __name__ == "__main__":
    raise SystemExit(main())
