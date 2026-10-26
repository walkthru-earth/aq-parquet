"""Drive the storage-worker watchdog supervisor with a fake clock and task watchdog."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from host_cxx import CXX_STD


def check_start_order(source: str) -> None:
    """PARQUET BEGIN prints before any worker line, and the worker is always released.

    The worker waits for a start notification before its first stamp, and
    begin() gives it right after the BEGIN line with no return in between.
    """
    worker = source.find("void storage_worker(void *) {")
    assert worker >= 0, "storage_worker not found"
    body = source[worker + len("void storage_worker(void *) {"):source.find("\n}\n", worker)]
    # The wait must be the first statement, so no call of any kind, a logging
    # helper included, can run before it. clang-format may wrap the lines.
    code = " ".join(re.sub(r"//[^\n]*", "", body).split())
    first = "const bool released = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kWorkerStartWaitMs)) != 0;"
    assert code.startswith(first), "the start wait must be the first statement of storage_worker"
    # A timeout starts the worker anyway. Its only step before the first
    # stamp is the worker-start-wait line, which says the order is lost.
    rest = code.removeprefix(first).lstrip()
    timeout = re.match(r'if \(!released\) \{ \+\+errors; oslog\.printf\("PARQUET ERROR '
                       r'operation=worker-start-wait [^;]*\); \} ', rest)
    assert timeout, "a start wait timeout must log worker-start-wait before anything else"
    assert rest[timeout.end():].startswith("stamp_heartbeat();"), "the first stamp must follow the start wait"
    begin = source.find("bool begin(const Config &configuration")
    assert begin >= 0, "begin() not found"
    body = source[begin:source.find("\n}\n", begin)]
    create = body.find("xTaskCreate(storage_worker")
    banner = body.find('"PARQUET BEGIN ')
    give = body.find("xTaskNotifyGive(worker_task);")
    assert 0 <= create < banner < give, "begin() must give the start signal after it prints BEGIN"
    # The create failure returns before the task exists. After it, nothing
    # may return before the signal, or the worker would wait out its bound.
    after_create = body[body.find("\n", body.find("return false;", create)):give]
    assert "return" not in after_create, "begin() returns between xTaskCreate and the start signal"
    tail = body[give:].removeprefix("xTaskNotifyGive(worker_task);").strip()
    assert tail.startswith("return true;"), \
        "the start signal must be the last step before begin() returns"
    wait = re.search(r"constexpr std::uint32_t kWorkerStartWaitMs = (\d+);", source)
    assert wait and int(wait.group(1)) < 15000, "the start wait must stay below the 15 s minimum stall timeout"


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
    check_start_order(source)
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
          "stalls, boot-loop guard, slow writes, hung sink, unstamped worker, setup "
          "unwinding and BEGIN before the worker start passed")


if __name__ == "__main__":
    main()
