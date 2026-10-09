"""Exercise the shared native network time service without sockets or hardware."""
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
    with tempfile.TemporaryDirectory(prefix="aq-network-time-") as directory:
        executable = Path(directory) / "network-time-test"
        command = [
            "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-I", str(root / "tools/wifi_host_fakes"),
            "-I", str(root / "firmware/common/connectivity/src"),
            "-I", str(root / "firmware/common/src"),
            str(root / "tools/test_network_time.cpp"),
            str(root / "firmware/common/connectivity/src/network_time.cpp"),
            "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
