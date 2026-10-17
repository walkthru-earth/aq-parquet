"""Drive the storage-worker watchdog supervisor with a fake clock and task watchdog."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from host_cxx import CXX_STD


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
    # begin() seeds the heartbeat before the worker exists, so a worker that
    # never stamps still ages out.
    seed = source.find("worker_heartbeat_us = esp_timer_get_time();\n  if (xTaskCreate(storage_worker")
    assert seed >= 0, "begin() must seed the heartbeat right before xTaskCreate"
    with tempfile.TemporaryDirectory(prefix="aq-watchdog-") as directory:
        executable = Path(directory) / "watchdog-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                   "-I", str(logger / "src"),
                   str(logger / "tests/test_watchdog.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("Worker watchdog: apply order, IDF feed rule, recovery clears the latch, repeated "
          "stalls, boot-loop guard, slow writes, hung sink, unstamped worker and setup "
          "unwinding passed")


if __name__ == "__main__":
    main()
