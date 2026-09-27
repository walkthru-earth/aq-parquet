"""Build and run the fake-bus LTR553 driver tests; no device is accessed."""

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
    with tempfile.TemporaryDirectory(prefix="aq-ltr553-test-") as directory:
        executable = Path(directory) / "ltr553-test"
        command = ["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   str(root / "tools/test_ltr553.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
