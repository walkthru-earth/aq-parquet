"""Build and run the core dump report-once tests on the host."""

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
    with tempfile.TemporaryDirectory(prefix="aq-coredump-report-") as directory:
        executable = Path(directory) / "coredump-report-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                   str(root / "tools/test_coredump_report.cpp"),
                   str(root / "firmware/common/runtime/src/coredump_report.cpp"),
                   "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("core dump report once, record layout, store read and write failures, "
          "corrupt and unreadable dumps and one token names passed")


if __name__ == "__main__":
    main()
