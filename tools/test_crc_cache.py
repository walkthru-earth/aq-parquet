"""Host checks for the archive OPEN CRC cache (hit, miss, LRU, invalidation).

Synthetic keys do not establish SD timestamps, finalize timing or OPEN latency.
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
    with tempfile.TemporaryDirectory(prefix="aq-crc-cache-test-") as directory:
        executable = Path(directory) / "crc-cache-test"
        command = [
            "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-I", str(root / "firmware/common/logger/src"),
            str(root / "tools/test_crc_cache.cpp"), "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("CRC cache hit, miss, invalidation and LRU checks passed")


if __name__ == "__main__":
    main()
