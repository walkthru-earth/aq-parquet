"""Check mounted-root creation, existing-file rejection and mkdir races."""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    logger = root / "firmware/common/logger"
    with tempfile.TemporaryDirectory(prefix="aq-logger-directory-") as directory:
        executable = Path(directory) / "directory-test"
        command = ["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-I", str(logger / "src"),
                   str(logger / "tests/test_directory.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("Logger directory checks passed: FatFs root, file rejection, denied access and mkdir races")


if __name__ == "__main__":
    main()
