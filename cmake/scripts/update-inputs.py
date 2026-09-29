#!/usr/bin/env python3
"""Refresh the source snapshot manifests after adding/removing tracked inputs.

Run from any directory. This changes only cmake/*-inputs.txt; it does not change
target source lists. Review and update targets/Sources.cmake separately.
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
for part in ("src", "depends"):
    files = subprocess.check_output(
        ["git", "-C", str(root), "ls-files", "--", part], text=True
    ).splitlines()
    if part == "src":
        files = [
            name for name in files
            if Path(name).suffix not in (".am", ".in", ".mk")
            and "/autom4te.cache/" not in name
            and not name.endswith("/configure")
        ]
    (root / "cmake" / f"{part}-inputs.txt").write_text("\n".join(files) + "\n")
