from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import check_code_size  # noqa: E402


class CodeSizeTest(unittest.TestCase):
    def test_new_file_over_limit_is_blocking(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "new.cc"
            path.write_text("int line;\n" * 3, encoding="utf-8")

            errors, warnings = check_code_size.check_sizes([("A", path)], 2, 2)

        self.assertEqual(len(errors), 1)
        self.assertEqual(warnings, [])

    def test_existing_large_file_is_warning_only(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "existing.cc"
            path.write_text("int line;\n" * 3, encoding="utf-8")

            errors, warnings = check_code_size.check_sizes([("M", path)], 2, 2)

        self.assertEqual(errors, [])
        self.assertEqual(len(warnings), 1)


if __name__ == "__main__":
    unittest.main()
