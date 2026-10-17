"""Exercise the logger power tracker, emergency close intent and POWER STATE lines."""
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
    with tempfile.TemporaryDirectory(prefix="aq-logger-power-") as directory:
        executable = Path(directory) / "power-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror"]
        for library in ["power/src", "logger/src"]:
            command += ["-I", str(common / library)]
        command += [str(common / "logger/tests/test_power.cpp"),
                    str(common / "power/src/aq_battery_health.cpp"),
                    "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("Logger power tracker: one finalize per discharge, Ok or external "
          "power re-arms, the report wake, emergency close outcomes, retry, "
          "preempt, close pass lines and POWER STATE lines passed")


if __name__ == "__main__":
    main()
