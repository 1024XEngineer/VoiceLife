#!/usr/bin/env python3
"""Validate public E2E artifacts without printing their contents."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from e2e_evidence import EvidenceValidationError, scan_sensitive, validate_evidence


def validate_directory(root: Path) -> tuple[int, int]:
    """Return (file_count, evidence_count) after validating an artifact directory."""
    if not root.is_dir():
        raise ValueError("artifact directory is missing")
    files = sorted(path for path in root.rglob("*") if path.is_file())
    if not files:
        raise ValueError("artifact directory is empty")
    evidence_count = 0
    for path in files:
        if path.suffix != ".json":
            raise ValueError("public artifacts may only contain JSON")
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise ValueError("artifact JSON is unreadable") from error
        if scan_sensitive(document):
            raise ValueError("artifact contains sensitive data")
        if path.name.startswith("evidence-"):
            if not isinstance(document, dict):
                raise ValueError("E2E evidence must be a JSON object")
            try:
                validate_evidence(document)
            except EvidenceValidationError as error:
                raise ValueError("E2E evidence does not match the public schema") from error
            evidence_count += 1
    if evidence_count == 0:
        raise ValueError("artifact directory has no E2E evidence")
    return len(files), evidence_count


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact_dir", type=Path)
    args = parser.parse_args(argv)
    try:
        file_count, evidence_count = validate_directory(args.artifact_dir)
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 1
    print(f"validated {evidence_count} E2E evidence file(s) in {file_count} public artifact file(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
