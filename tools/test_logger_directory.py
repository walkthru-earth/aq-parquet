"""Check directory creation and truthful archive traversal under SD failures."""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile
from host_cxx import CXX_STD


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    logger = root / "firmware/common/logger"
    with tempfile.TemporaryDirectory(prefix="aq-logger-directory-") as directory:
        executable = Path(directory) / "directory-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                   "-I", str(logger / "src"),
                   str(logger / "tests/test_directory.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("Logger directory checks passed: FatFs root, mkdir races, archive traversal, resumable bounded walk steps, quarantine provenance, same-boot quarantine, iterative quarantine walk depth report, per entry skip and read error stop, and crosslink guard")


if __name__ == "__main__":
    main()
