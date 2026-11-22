# SPDX-License-Identifier: GPL-2.0-or-later
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import sync_edgetx_reference as sync


class ReferenceCaptureTest(unittest.TestCase):
    def test_extract_ignores_macros_comments_and_alternative_braces(self):
        body = """inline int convert(int x)
{
  const char* brace = "}";
  // }
#if defined(FEATURE)
  if (x) {
#else
  if (!x) {
#endif
    return x;
  }
}
"""
        source = "#define convert_16(x) convert(x)\n" + body
        extracted, line = sync.extract(source, "convert")
        self.assertEqual(extracted, body)
        self.assertEqual(line, 2)

    def test_duplicate_definition_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "found 2"):
            sync.extract("int value() {\n}\nint value() {\n}\n", "value")

    def fixture(self, root):
        body = b"int value()\n{\n  return 1;\n}\n"
        (root / "mixer.inc").write_bytes(body)
        (root / "source.cpp").write_bytes(body)
        manifest = {
            "revision": "test-baseline",
            "snapshot_sha256": sync.digest(body),
            "functions": [{"name": "value", "sha256": sync.digest(body)}],
            "watched_files": {"source.cpp": sync.digest(body)},
        }
        (root / "manifest.json").write_text(json.dumps(manifest))

    def test_modified_snapshot_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            (root / "mixer.inc").write_text("int value()\n{\n  return 2;\n}\n")
            with patch.object(sync, "DESTINATION", root):
                with self.assertRaisesRegex(ValueError, "snapshot changed"):
                    sync.check(None)

    def test_modified_upstream_source_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            with patch.object(sync, "DESTINATION", root):
                with contextlib.redirect_stdout(io.StringIO()):
                    sync.check(root)
                (root / "source.cpp").write_text("changed upstream\n")
                with self.assertRaisesRegex(ValueError, "source.cpp"):
                    sync.check(root)


if __name__ == "__main__":
    unittest.main()
