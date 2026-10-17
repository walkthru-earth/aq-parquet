"""Check shared UTC rotation decisions with sanitizer-enabled host fixtures."""
from pathlib import Path
import subprocess
import tempfile
from host_cxx import CXX_STD


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    common = root / "firmware/common"
    with tempfile.TemporaryDirectory(prefix="aq-logger-rotation-") as directory:
        executable = Path(directory) / "rotation-test"
        subprocess.run([
            "clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I", str(common / "src"), "-I", str(common / "logger/src"),
            str(common / "logger/tests/test_rotation.cpp"), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
    print("UTC and unsynced monotonic windows, clock transitions and row-group bounds passed")


if __name__ == "__main__":
    main()
