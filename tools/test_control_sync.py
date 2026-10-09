"""Check real worker-side shared control handlers with transport/platform fakes.

This establishes frame/security/action behavior, not real radio scheduling.
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
    common = root / "firmware/common/src"
    runtime = root / "firmware/common/runtime/src"
    connectivity = root / "firmware/common/connectivity/src"
    with tempfile.TemporaryDirectory(prefix="aq-control-test-") as directory:
        executable = Path(directory) / "control-test"
        command = [
            "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-I", str(root / "tools/host_fakes/config"),
            "-I", str(common), "-I", str(runtime), "-I", str(connectivity),
            str(root / "tools/test_control_sync.cpp"),
            str(runtime / "device_config.cpp"), str(runtime / "debug_log.cpp"),
            str(root / "tools/host_fakes/config/aq_console.cpp"),
            str(connectivity / "control_sync.cpp"), "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("Shared control: configuration secrecy/actions, BLE-only token and log limits passed")


if __name__ == "__main__":
    main()
