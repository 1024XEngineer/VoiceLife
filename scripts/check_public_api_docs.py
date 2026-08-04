#!/usr/bin/env python3
"""Ensure public C++ API declarations have concise Doxygen documentation."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import NamedTuple

TYPE_DECLARATION = re.compile(r"^(?:template\s*<.*>\s*)?(?:class|struct|enum\s+class)\s+\w+")
FUNCTION_DECLARATION = re.compile(
    r"^(?:(?:\[\[nodiscard\]\]\s*)?(?:(?:virtual|static|explicit)\s+)?)"
    r"(?:(?:[\w:<>]+(?:\s+[\w:<>]+)*\s*[*&]?\s+)?~?\w+)\s*\("
)
ACCESS_SPECIFIER = re.compile(r"^(public|private|protected):$")
CJK = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff]")


class DocComment(NamedTuple):
    """A Doxygen documentation comment associated with a declaration."""

    style: str
    text: str


def preceding_doc_comment(lines: list[str], line_number: int) -> DocComment | None:
    """Return the Doxygen comment immediately preceding a declaration, if any."""
    index = line_number - 1
    while index >= 0 and not lines[index].strip():
        index -= 1
    while index >= 0 and lines[index].lstrip().startswith("template"):
        index -= 1
    if index < 0:
        return None

    stripped = lines[index].lstrip()
    if stripped.startswith("///"):
        comment_lines: list[str] = []
        while index >= 0 and lines[index].lstrip().startswith("///"):
            comment_lines.append(lines[index])
            index -= 1
        return DocComment("line", "\n".join(reversed(comment_lines)))
    if stripped.startswith("/**") and stripped.endswith("*/"):
        return DocComment("block", lines[index])
    if not stripped.endswith("*/"):
        return None

    comment_lines = [lines[index]]
    index -= 1
    while index >= 0:
        comment_lines.append(lines[index])
        if lines[index].lstrip().startswith("/**"):
            return DocComment("block", "\n".join(reversed(comment_lines)))
        index -= 1
    return None


def declaration_parameters(lines: list[str], line_number: int) -> list[str]:
    """Extract parameter names from a declaration beginning at `line_number`."""
    declaration = " ".join(line.strip() for line in lines[line_number : line_number + 4])
    opening = declaration.find("(")
    closing = declaration.find(")", opening)
    if opening == -1 or closing == -1:
        return []
    parameters = declaration[opening + 1 : closing].strip()
    if not parameters or parameters == "void":
        return []
    return [match.group(1) for match in re.finditer(r"([A-Za-z_]\w*)\s*(?:=[^,)]*)?(?:,|$)", parameters)]


def returns_value(line: str) -> bool:
    """Return whether a single-line function declaration has a non-void return type."""
    function = re.search(r"(~?\w+)\s*\(", line)
    if not function or function.group(1).startswith("~"):
        return False
    prefix = line[: function.start()].replace("[[nodiscard]]", "").strip()
    prefix = re.sub(r"^(virtual|static|explicit)\b\s*", "", prefix).strip()
    return bool(prefix) and prefix != "void"


def check_header(path: Path) -> list[str]:
    lines = path.read_text(encoding="utf-8").splitlines()
    errors: list[str] = []
    access = "public"

    for number, line in enumerate(lines):
        stripped = line.strip()
        access_match = ACCESS_SPECIFIER.fullmatch(stripped)
        if access_match:
            access = access_match.group(1)
            continue
        if not stripped or stripped.startswith(("//", "/*", "*", "#")):
            continue

        is_type = TYPE_DECLARATION.match(stripped) is not None
        is_public_function = (
            access == "public" and not stripped.startswith(":") and FUNCTION_DECLARATION.match(stripped) is not None
        )
        doc = preceding_doc_comment(lines, number)
        if (is_type or is_public_function) and doc is None:
            kind = "类型" if is_type else "公开函数"
            errors.append(f"{path}:{number + 1}: {kind} 缺少紧邻的 Doxygen 注释（/// 或 /**）")
            continue
        if (is_type or is_public_function) and not CJK.search(doc.text):
            errors.append(f"{path}:{number + 1}: 公共 API 注释必须包含中文说明")
        if not is_public_function:
            continue
        if doc.style != "block":
            errors.append(f"{path}:{number + 1}: 公开函数必须使用 /** ... */ 块注释")
            continue
        if "@brief" not in doc.text:
            errors.append(f"{path}:{number + 1}: 公开函数文档缺少 @brief")
        for parameter in declaration_parameters(lines, number):
            if not re.search(rf"@param\s+{re.escape(parameter)}\b", doc.text):
                errors.append(f"{path}:{number + 1}: 公开函数文档缺少 @param {parameter}")
        if returns_value(stripped) and "@return" not in doc.text:
            errors.append(f"{path}:{number + 1}: 有返回值的公开函数文档缺少 @return")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, nargs="?", default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()

    headers = sorted((args.root / "components").glob("**/include/**/*.h"))
    errors = [error for header in headers for error in check_header(header)]
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"PASS 已检查 {len(headers)} 个公共 C++ 头文件的 API 文档")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
