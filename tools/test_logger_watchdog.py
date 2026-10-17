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
    # The decision logic and the steps that apply it stay pure so they build
    # and run on the host, and aq_logger.cpp must go through them.
    for name in ("aq_logger_watchdog.h", "aq_logger_watchdog_apply.h"):
        header = logger / "src" / name
        include = re.search(r'#include\s*[<"](esp_|freertos/|driver/|soc/)', header.read_text())
        assert include is None, f"{name} includes {include.group(0)}"
    source = (logger / "src/aq_logger.cpp").read_text()
    for call in ("watchdog::tick(", "watchdog::prepare(", "watchdog::launch("):
        assert call in source, f"aq_logger.cpp no longer calls {call}"
    with tempfile.TemporaryDirectory(prefix="aq-watchdog-") as directory:
        executable = Path(directory) / "watchdog-test"
        command = ["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-I", str(logger / "src"),
                   str(logger / "tests/test_watchdog.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("Worker watchdog: apply order, IDF feed rule, recovery clears the latch, repeated "
          "stalls, boot-loop guard, slow writes, hung sink and setup "
          "unwinding passed")


if __name__ == "__main__":
    main()
