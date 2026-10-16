"""Build and run the pure battery health classifier tests on the host."""

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
    with tempfile.TemporaryDirectory(prefix="aq-battery-health-") as directory:
        executable = Path(directory) / "battery-health-test"
        command = ["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   str(root / "tools/test_battery_health.cpp"),
                   str(root / "firmware/common/power/src/aq_battery_health.cpp"),
                   "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("battery health codes, thresholds, debounce, hysteresis, settling, "
          "invalid samples and minutes to empty passed")


if __name__ == "__main__":
    main()
