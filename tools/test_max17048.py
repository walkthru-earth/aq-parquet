"""Build and run the fake-bus MAX17048 driver tests; no device is accessed."""

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
    with tempfile.TemporaryDirectory(prefix="aq-max17048-test-") as directory:
        executable = Path(directory) / "max17048-test"
        command = ["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   str(root / "tools/test_max17048.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("MAX17048 decode bounds, register order, extended registers, "
          "RI clear, IO failures and unset IO passed")


if __name__ == "__main__":
    main()
