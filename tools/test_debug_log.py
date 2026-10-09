"""Verify ring-only radio diagnostics and complete serial forwarding."""
from pathlib import Path
import subprocess
import tempfile


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    runtime = root / "firmware/common/runtime/src"
    with tempfile.TemporaryDirectory(prefix="aq-debug-log-test-") as directory:
        executable = Path(directory) / "debug-log-test"
        subprocess.run([
            "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I", str(root / "tools/host_fakes/config"), "-I", str(runtime),
            str(root / "tools/test_debug_log.cpp"), str(runtime / "debug_log.cpp"),
            str(root / "tools/host_fakes/config/aq_console.cpp"),
            "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
    print("Debug log: blocked console isolation, full ring retention and serial forwarding passed")


if __name__ == "__main__":
    main()
