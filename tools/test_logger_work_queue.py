"""Test archive-worker queue/wakeup interleavings with deterministic host fakes."""
from pathlib import Path
import subprocess
import tempfile
from host_cxx import CXX_STD


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="aq-worker-test-") as directory:
        executable = Path(directory) / "worker-test"
        subprocess.run([
            "clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I", str(root / "tools/logger_work_host_fakes"),
            "-I", str(root / "firmware/common/logger/src"),
            str(root / "tools/test_logger_work_queue.cpp"), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
    print("Archive worker: raced/pre-start wakeups, coalescing, sample priority, saturation, stall checks and the watchdog boot-loop guard passed")


if __name__ == "__main__":
    main()
