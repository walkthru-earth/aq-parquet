"""Test real LAN transport routing with host TCP sockets and platform fakes."""

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
    with tempfile.TemporaryDirectory(prefix="aq-wifi-test-") as directory:
        executable = Path(directory) / "wifi-test"
        command = [
            "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-I", str(root / "tools/wifi_host_fakes"),
            "-I", str(root / "firmware/common/runtime/src"),
            "-I", str(root / "firmware/common/connectivity/src"),
            str(root / "tools/test_wifi_link.cpp"),
            str(root / "firmware/common/runtime/src/debug_log.cpp"),
            "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("LAN transport: native lifecycle/scans, authentication, routing, pushes, stale sessions and isolation passed")


if __name__ == "__main__":
    main()
