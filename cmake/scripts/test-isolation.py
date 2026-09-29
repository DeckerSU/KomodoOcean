#!/usr/bin/env python3
"""Black-box regression checks for source isolation; no dependency build needed."""
from pathlib import Path
import subprocess
import tempfile
import unittest


SNAPSHOT = Path(__file__).resolve().parents[1] / "Snapshot.cmake"


class SnapshotIsolation(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="komodo-cmake-isolation-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.repo = self.base / "repository"
        self.output = self.base / "build"
        (self.repo / "src").mkdir(parents=True)
        self.manifest = self.base / "inputs.txt"
        self.driver = self.base / "snapshot.cmake"
        self.driver.write_text(
            'cmake_minimum_required(VERSION 3.22)\n'
            f'set(KMD_REPOSITORY "{self.repo}")\n'
            f'include("{SNAPSHOT}")\n'
            f'kmd_snapshot("{self.manifest}" "{self.output}")\n'
        )

    def run_snapshot(self, success=True):
        result = subprocess.run(
            ["cmake", "-P", str(self.driver)], text=True, capture_output=True
        )
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)

    def test_stale_autotools_files_and_local_experiments_are_not_consumed(self):
        source = self.repo / "src/main.cpp"
        source.write_text("original\n")
        (self.repo / "src/bitcoin-config.h").write_text("old autotools config\n")
        (self.repo / "src/experiment.cpp").write_text("untracked experiment\n")
        self.manifest.write_text("src/main.cpp\n")
        self.run_snapshot()
        copied = self.output / "src/main.cpp"
        self.assertEqual(copied.read_text(), "original\n")
        self.assertFalse((self.output / "src/bitcoin-config.h").exists())
        self.assertFalse((self.output / "src/experiment.cpp").exists())
        old_timestamp = copied.stat().st_mtime_ns
        self.run_snapshot()
        self.assertEqual(copied.stat().st_mtime_ns, old_timestamp)
        source.write_text("edited\n")
        self.run_snapshot()
        self.assertEqual(copied.read_text(), "edited\n")
        generated = self.output / "src/bitcoin-config.h"
        generated.write_text("new CMake config\n")
        self.manifest.write_text("")
        self.run_snapshot()
        self.assertFalse(copied.exists())
        self.assertEqual(generated.read_text(), "new CMake config\n")
        self.assertEqual((self.repo / "src/bitcoin-config.h").read_text(), "old autotools config\n")
        self.assertEqual(source.read_text(), "edited\n")

    def test_manifest_cannot_escape_build_directory(self):
        outside = self.base / "outside"
        outside.write_text("keep\n")
        self.manifest.write_text("../outside\n")
        self.run_snapshot(success=False)
        self.assertEqual(outside.read_text(), "keep\n")


if __name__ == "__main__":
    unittest.main()
