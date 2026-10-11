"""Check BLE notify credits, stall clock, advert window, interval mode and link limits.

Host checks of the pure link policy only; no radio, host stack or central behavior
is established.
Run: pixi run ble-policy-test
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
    connectivity = root / "firmware/common/connectivity/src"
    with tempfile.TemporaryDirectory(prefix="aq-ble-policy-test-") as directory:
        executable = Path(directory) / "ble-policy-test"
        command = ["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-I", str(connectivity),
                   str(root / "tools/test_ble_policy.cpp"),
                   str(connectivity / "ble_policy.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("BLE policy: credits Ok/Full/Closed, refused/failed, stale generation, "
          "15 s stall clock, cancel, advert window, interval mode and link "
          "limits passed")


if __name__ == "__main__":
    main()
