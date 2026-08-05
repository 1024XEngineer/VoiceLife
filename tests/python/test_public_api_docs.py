from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import check_public_api_docs  # noqa: E402


class PublicApiDocsTest(unittest.TestCase):
    def check_fixture(self, content: str) -> list[str]:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.h"
            path.write_text(content, encoding="utf-8")
            return check_public_api_docs.check_header(path)

    def test_requires_chinese_doxygen_for_public_function(self) -> None:
        errors = self.check_fixture(
            """
            /// 示例类型。
            class Example {
             public:
                /** @brief Reports whether the value is valid. @return Validation state. */
                bool valid() const;
            };
            """
        )

        self.assertTrue(any("必须包含中文说明" in error for error in errors))

    def test_accepts_chinese_doxygen_for_public_function(self) -> None:
        errors = self.check_fixture(
            """
            /// 示例类型。
            class Example {
             public:
                /** @brief 判断值是否有效。 @return 有效时返回 true。 */
                bool valid() const;
            };
            """
        )

        self.assertEqual(errors, [])


if __name__ == "__main__":
    unittest.main()
