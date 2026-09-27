"""Host checks for shared numeric rows, UTC helpers and archive wire primitives.

Synthetic tests do not establish board radio, sensor, SD or power-loss behavior.
"""

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
    with tempfile.TemporaryDirectory(prefix="aq-common-test-") as directory:
        executable = Path(directory) / "common-test"
        command = [
            "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-I", str(root / "firmware/common/src"),
            str(root / "tools/test_common.cpp"), "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("Shared sample, UTC, wire, CRC and archive checks passed")


if __name__ == "__main__":
    main()
