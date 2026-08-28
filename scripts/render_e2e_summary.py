#!/usr/bin/env python3
"""Render a safe GitHub Actions job summary from E2E evidence."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from e2e_evidence import EvidenceValidationError, validate_evidence


def render(root: Path) -> str:
    rows: list[str] = []
    paths = sorted(root.rglob("evidence-*.json"))
    for path in paths:
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(document, dict):
                raise ValueError
            validate_evidence(document)
        except (OSError, UnicodeError, json.JSONDecodeError, ValueError, EvidenceValidationError) as error:
            raise ValueError("cannot render invalid E2E evidence") from error
        status = str(document["status"]).upper()
        category = document["failure_category"] or "none"
        phase = document["failed_phase"] or "none"
        diagnostics = document.get("diagnostics") or {}
        diagnostic_text = "none"
        if diagnostics:
            diagnostic_text = str(diagnostics["code"])
            if diagnostics["markers"]:
                diagnostic_text += ":" + ",".join(diagnostics["markers"])
        rows.append(
            f"| `{document['profile']}` | `{document['journey']}` | {status} | `{category}` | "
            f"`{phase}` | `{document['message_code']}` | `{diagnostic_text}` |"
        )
    if not rows:
        raise ValueError("no E2E evidence found")
    return (
        "## E2E result\n\n"
        "| Profile | Journey | Status | Failure category | Failed phase | Message | Diagnostics |\n"
        "| --- | --- | --- | --- | --- | --- | --- |\n" + "\n".join(rows) + "\n"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact_dir", type=Path)
    parser.add_argument("--output", type=Path, default=None)
    args = parser.parse_args(argv)
    try:
        summary = render(args.artifact_dir)
        if args.output is None:
            print(summary, end="")
        else:
            args.output.write_text(summary, encoding="utf-8")
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
