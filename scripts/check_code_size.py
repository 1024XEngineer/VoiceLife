#!/usr/bin/env python3
"""Check source-file size for new and modified files in a pull request."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

SOURCE_ROOTS = ("components/", "main/", "services/", "scripts/", "tests/")
SOURCE_SUFFIXES = {".cc", ".h", ".py", ".sh", ".mjs", ".ts"}
IGNORED_PARTS = {"build", "build-host", "dist", "generated", "managed_components", "node_modules"}
PRODUCTION_ROOTS = ("components/", "main/", "services/", "scripts/")
DEFAULT_BASELINE = Path(__file__).resolve().parents[1] / "config" / "code-size-baseline.json"


def changed_sources(base: str) -> list[tuple[str, Path]]:
    output = subprocess.check_output(
        ["git", "diff", "--diff-filter=AM", "--name-status", f"{base}...HEAD"],
        text=True,
    )
    result: list[tuple[str, Path]] = []
    for line in output.splitlines():
        status, _, raw_path = line.partition("\t")
        path = Path(raw_path)
        if status not in {"A", "M"} or not path.exists():
            continue
        if not raw_path.startswith(SOURCE_ROOTS) or path.suffix not in SOURCE_SUFFIXES:
            continue
        if any(part in IGNORED_PARTS for part in path.parts):
            continue
        result.append((status, path))
    return result


def check_sizes(
    files: list[tuple[str, Path]],
    new_file_limit: int,
    existing_file_warning: int,
    production_baseline: dict[str, int],
) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    warnings: list[str] = []
    for status, path in files:
        lines = len(path.read_text(encoding="utf-8").splitlines())
        if str(path) in production_baseline and lines > production_baseline[str(path)]:
            errors.append(f"{path}: 受控生产文件从 {production_baseline[str(path)]} 行增长至 {lines} 行")
        elif str(path) in production_baseline:
            continue
        if status == "A" and lines > new_file_limit:
            errors.append(f"{path}: 新增源码文件 {lines} 行，超过 {new_file_limit} 行上限")
        elif status == "M" and str(path).startswith(PRODUCTION_ROOTS) and lines > new_file_limit:
            errors.append(f"{path}: 新增超线生产文件 {lines} 行，先拆分或登记受控基线")
        elif status == "M" and lines > existing_file_warning:
            warnings.append(f"{path}: 现有源码文件 {lines} 行，超过 {existing_file_warning} 行，建议拆分")
    return errors, warnings


def load_production_baseline(path: Path) -> dict[str, int]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"无法读取源码规模基线 {path}: {error}") from error
    files = document.get("productionFiles")
    if document.get("version") != 1 or not isinstance(files, dict):
        raise ValueError(f"源码规模基线 {path} 格式错误")
    if any(not isinstance(name, str) or not isinstance(lines, int) or lines <= 0 for name, lines in files.items()):
        raise ValueError(f"源码规模基线 {path} 必须包含正整数行数")
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, help="用于比较的 Git 基线，例如 origin/main 或 base SHA")
    parser.add_argument("--new-file-limit", type=int, default=500)
    parser.add_argument("--existing-file-warning", type=int, default=800)
    parser.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    args = parser.parse_args()

    try:
        files = changed_sources(args.base)
    except subprocess.CalledProcessError as error:
        print(f"无法读取 Git 基线 {args.base}: {error}", file=sys.stderr)
        return 2

    try:
        production_baseline = load_production_baseline(args.baseline)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2

    errors, warnings = check_sizes(files, args.new_file_limit, args.existing_file_warning, production_baseline)

    for warning in warnings:
        print(f"::warning file={warning.split(':', 1)[0]}::{warning.split(':', 1)[1].strip()}")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        print("新增大文件应拆为职责单一的模块；确有必要时先补 Design/ADR。", file=sys.stderr)
        return 1
    print(
        f"PASS 已检查 {len(files)} 个变更源码文件；新增文件上限 {args.new_file_limit} 行；"
        f"历史生产基线 {len(production_baseline)} 个文件"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
