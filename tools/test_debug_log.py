"""Verify ring-only radio diagnostics and complete serial forwarding."""
from pathlib import Path
import subprocess
import tempfile
from host_cxx import CXX_STD


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    runtime = root / "firmware/common/runtime/src"
    with tempfile.TemporaryDirectory(prefix="aq-debug-log-test-") as directory:
        executable = Path(directory) / "debug-log-test"
        subprocess.run([
            "clang++", CXX_STD, "-Wall", "-Wextra", "-Werror", "-pthread",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I", str(root / "tools/host_fakes/config"), "-I", str(runtime),
            str(root / "tools/test_debug_log.cpp"), str(runtime / "debug_log.cpp"),
            str(root / "tools/host_fakes/config/os_console.cpp"),
            "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
    print("Debug log: console isolation, 64 KiB persistent ring, sliced reads, warm reboot, cache writeback and corrupt header passed")


if __name__ == "__main__":
    main()
