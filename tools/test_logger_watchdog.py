"""Drive the storage-worker watchdog supervisor with a fake clock and task watchdog."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    logger = root / "firmware/common/logger"
    header = logger / "src/aq_logger_watchdog.h"
    # The decision logic stays pure so it builds and runs on the host.
    include = re.search(r'#include\s*[<"](esp_|freertos/|driver/|soc/)', header.read_text())
    assert include is None, f"watchdog header includes {include.group(0)}"
    with tempfile.TemporaryDirectory(prefix="aq-watchdog-") as directory:
        executable = Path(directory) / "watchdog-test"
        command = ["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-I", str(logger / "src"),
                   str(logger / "tests/test_watchdog.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("Worker watchdog: recovery clears the latch, repeated stalls, boot-loop guard, "
          "slow writes, hung sink and setup unwinding passed")


if __name__ == "__main__":
    main()
