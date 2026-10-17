"""Check the SD space governor states, hysteresis, backoff and no-delete rule."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from host_cxx import CXX_STD


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    common = root / "firmware/common"
    header = common / "logger/src/storage_governor.h"
    # The governor only reports state. It must never reach a file API that
    # removes, renames or truncates owner data.
    forbidden = re.compile(r"\b(remove|unlink|rename|rmdir|truncate|ftruncate|f_unlink|f_rename|fopen)\s*\(")
    match = forbidden.search(header.read_text())
    assert match is None, f"storage governor calls {match.group(0)}"
    with tempfile.TemporaryDirectory(prefix="aq-storage-governor-") as directory:
        executable = Path(directory) / "storage-governor-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                   "-I", str(common / "logger/src"),
                   str(common / "logger/tests/test_storage_governor.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("SD governor thresholds, hysteresis, backoff and recovery passed with no delete path")


if __name__ == "__main__":
    main()
