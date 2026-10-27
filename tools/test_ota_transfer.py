"""Build and run the LAN OTA transfer rule tests on the host."""

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
    with tempfile.TemporaryDirectory(prefix="os-ota-transfer-") as directory:
        executable = Path(directory) / "ota-transfer-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                   "-I", str(root / "firmware/common/src"),
                   str(root / "tools/test_ota_transfer.cpp"),
                   str(root / "firmware/common/src/ota_transfer.cpp"),
                   "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("OTA begin size and slot cap, contiguous chunks, header chip and "
          "project match, SHA-256 before activate, abort on mismatch, "
          "ownership and STATE frame passed")


if __name__ == "__main__":
    main()
