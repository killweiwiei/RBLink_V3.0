"""Run idf.py after restoring the configured ESP-IDF tool paths.

The managed command environment may sanitize PATH while starting Python.  The
ESP-IDF CMake subprocesses still need Ninja and the RISC-V compiler, so this
small wrapper restores the explicitly validated directories inside Python
before idf.py imports any build modules.
"""

from __future__ import annotations

import os
import runpy
import sys


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit("usage: idf_runner.py <idf.py> [arguments...]")
    idf_command = sys.argv[1]
    prepend = os.environ.pop("RBLINK_IDF_PATH_PREPEND", "")
    if prepend:
        os.environ["PATH"] = prepend + os.pathsep + os.environ.get("PATH", "")
    sys.path.insert(0, os.path.dirname(os.path.abspath(idf_command)))
    sys.argv = sys.argv[1:]
    runpy.run_path(idf_command, run_name="__main__")


if __name__ == "__main__":
    main()
