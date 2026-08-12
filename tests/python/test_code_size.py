from __future__ import annotations

import json
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

            errors, warnings = check_code_size.check_sizes([("A", path)], 2, 2, {})

        self.assertEqual(len(errors), 1)
        self.assertEqual(warnings, [])

    def test_existing_large_file_is_warning_only(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "existing.cc"
            path.write_text("int line;\n" * 3, encoding="utf-8")

            errors, warnings = check_code_size.check_sizes([("M", path)], 2, 2, {})

        self.assertEqual(errors, [])
        self.assertEqual(len(warnings), 1)

    def test_existing_baselined_production_file_cannot_grow(self) -> None:
        with tempfile.TemporaryDirectory(dir=ROOT) as directory:
            path = Path(directory) / "existing.cc"
            path.write_text("int line;\n" * 3, encoding="utf-8")
            relative_path = str(path.relative_to(ROOT))

            errors, warnings = check_code_size.check_sizes([("M", Path(relative_path))], 2, 10, {relative_path: 2})

        self.assertEqual(len(errors), 1)
        self.assertEqual(warnings, [])

    def test_load_production_baseline_rejects_invalid_line_count(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "baseline.json"
            path.write_text(json.dumps({"version": 1, "productionFiles": {"main/app.cc": 0}}), encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "必须包含正整数行数"):
                check_code_size.load_production_baseline(path)


if __name__ == "__main__":
    unittest.main()
