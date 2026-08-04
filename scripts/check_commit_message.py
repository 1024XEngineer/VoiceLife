#!/usr/bin/env python3
"""Validate VoiceLife's Gitmoji + Conventional Commit message format."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ALLOWED = {
    "✨": {"feat"},
    "🐛": {"fix"},
    "📝": {"docs"},
    "♻️": {"refactor"},
    "⚡️": {"perf"},
    "✅": {"test"},
    "👷": {"ci"},
    "🔧": {"build", "chore"},
    "🔨": {"build"},
    "🔒️": {"fix"},
    "🏗️": {"refactor"},
    "🔥": {"refactor", "chore"},
    "🚚": {"refactor", "chore"},
    "⬆️": {"build"},
    "🔖": {"chore"},
    "⏪️": {"revert"},
}
HEADER = re.compile(
    r"^(?P<emoji>\S+) (?P<type>feat|fix|docs|refactor|perf|test|build|ci|chore|revert)"
    r"(?:\((?P<scope>[a-z0-9][a-z0-9-]*)\))?(?P<breaking>!)?: (?P<subject>.+)$"
)
CJK = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff]")


def validate(message: str) -> list[str]:
    lines = message.rstrip("\n").splitlines()
    if not lines:
        return ["提交描述为空"]
    header = lines[0]
    errors: list[str] = []
    if len(header) > 72:
        errors.append(f"主题长度为 {len(header)}，不能超过 72 个字符")
    match = HEADER.fullmatch(header)
    if not match:
        errors.append("主题应为：<gitmoji> <type>(<scope>): <中文描述>")
        return errors
    emoji = match.group("emoji")
    commit_type = match.group("type")
    if emoji not in ALLOWED:
        errors.append(f"未允许的 Gitmoji：{emoji}")
    elif commit_type not in ALLOWED[emoji]:
        expected = "/".join(sorted(ALLOWED[emoji]))
        errors.append(f"{emoji} 应与 type={expected} 搭配，当前为 {commit_type}")
    if not CJK.search(match.group("subject")):
        errors.append("主题描述应以中文为主，技术名词可保留英文")
    if match.group("subject").endswith(("。", ".")):
        errors.append("主题结尾不加句号")
    if len(lines) > 1 and lines[1].strip():
        errors.append("主题与正文之间必须空一行")
    if match.group("breaking") and "BREAKING CHANGE:" not in message:
        errors.append("使用 ! 时，正文必须说明 BREAKING CHANGE:")
    return errors


def messages_from_range(revision_range: str) -> list[tuple[str, str]]:
    output = subprocess.check_output(
        ["git", "log", "--format=%H%x00%B%x00", "--no-merges", revision_range],
        text=True,
    )
    fields = output.split("\x00")
    messages: list[tuple[str, str]] = []
    for index in range(0, len(fields) - 1, 2):
        commit = fields[index].strip()
        message = fields[index + 1]
        if commit:
            messages.append((commit, message))
    return messages


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--file", type=Path, help="commit-msg hook 传入的消息文件")
    group.add_argument("--range", dest="revision_range", help="Git revision range，例如 main..HEAD")
    args = parser.parse_args()

    if args.file:
        candidates = [(str(args.file), args.file.read_text(encoding="utf-8"))]
    else:
        candidates = messages_from_range(args.revision_range)

    failed = False
    for identifier, message in candidates:
        errors = validate(message)
        if not errors:
            print(f"PASS {identifier[:12]}")
            continue
        failed = True
        print(f"FAIL {identifier[:12]}", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
