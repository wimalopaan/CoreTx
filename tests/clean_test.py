# SPDX-License-Identifier: GPL-2.0-or-later
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


CMAKE = sys.argv.pop(1)
SCRIPT = Path(__file__).resolve().parents[1] / "cmake" / "clean.cmake"


class CleanupTest(unittest.TestCase):
    def fixture(self, root):
        (root / "cmake").mkdir()
        shutil.copyfile(SCRIPT, root / "cmake" / "clean.cmake")
        (root / "coretx" / "core").mkdir(parents=True)
        for name in ("CMakeLists.txt", "CMakePresets.json", "README.backup.adoc",
                     "coretx/core/core.hpp", "CoreTX.creator"):
            (root / name).write_text("preserve\n")

    def clean(self, root):
        return subprocess.run(
            [CMAKE, "-P", str(root / "cmake" / "clean.cmake")],
            capture_output=True, text=True, check=False,
        )

    def test_cleanup_preserves_sources_and_is_repeatable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            for name in ("build", "build-sanitize", "build-i6x", "build-i6s",
                         "build-docs", "deps", "tools/__pycache__", "tests/__pycache__"):
                path = root / name
                path.mkdir(parents=True)
                (path / "artifact").write_text("generated")
            for name in ("README.html", "README.backup.html", "COMPATIBILITY.html",
                         "PERIPHERALS.html", "scratch.tmp", "editor.swp", "source~",
                         "CoreTX.creator.user", "CoreTX.creator.user.1",
                         "tools/module.pyc", "tests/module.pyo"):
                (root / name).write_text("generated")
            result = self.clean(root)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                sorted(str(path.relative_to(root)) for path in root.rglob("*") if path.is_file()),
                ["CMakeLists.txt", "CMakePresets.json", "CoreTX.creator",
                 "README.backup.adoc", "cmake/clean.cmake", "coretx/core/core.hpp"],
            )
            result = self.clean(root)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_symlink_is_rejected_before_any_deletion(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "source"
            root.mkdir()
            self.fixture(root)
            (root / "build").mkdir()
            outside = Path(directory) / "outside"
            outside.mkdir()
            (outside / "keep").write_text("preserve")
            (root / "deps").symlink_to(outside, target_is_directory=True)
            result = self.clean(root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Refusing to remove symlink", result.stderr)
            self.assertTrue((root / "build").is_dir())
            self.assertEqual((outside / "keep").read_text(), "preserve")

    def test_invalid_source_tree_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            (root / "CMakePresets.json").unlink()
            (root / "build").mkdir()
            result = self.clean(root)
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue((root / "build").is_dir())


if __name__ == "__main__":
    unittest.main()
