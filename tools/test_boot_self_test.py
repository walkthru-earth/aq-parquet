"""Build and run the boot self test decision tests on the host."""

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
    with tempfile.TemporaryDirectory(prefix="os-boot-self-test-") as directory:
        executable = Path(directory) / "boot-self-test-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                   "-I", str(root / "firmware/common/src"),
                   str(root / "tools/test_boot_self_test.cpp"),
                   str(root / "firmware/common/src/boot_self_test.cpp"),
                   "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("self test pass, fail and deadline verdicts, mark valid plan "
          "and SELFTEST line passed")


if __name__ == "__main__":
    main()
