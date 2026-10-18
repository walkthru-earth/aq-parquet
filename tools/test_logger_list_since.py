"""Check the LIST_SINCE ring, plan and incremental walk on a real directory tree."""
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
    common = root / "firmware/common"
    with tempfile.TemporaryDirectory(prefix="aq-logger-list-since-") as directory:
        executable = Path(directory) / "list-since-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror"]
        for library in ["src", "runtime/src", "connectivity/src", "logger/src"]:
            command += ["-I", str(common / library)]
        command += [str(common / "logger/tests/test_list_since.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable), directory], check=True)
    print("LIST_SINCE checks passed: dates, names, cursor wire, boot ring, NVS failure fallback, plan, bounded incremental walk on a 5000+ file tree, new files across boots and clock moves")


if __name__ == "__main__":
    main()
