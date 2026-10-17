"""Check FrameRing prefixes, wrap, partial consume, drop and the 24 KiB LAN bound.

Host checks of the transmit ring only; no socket or radio behavior is established.
Run: pixi run frame-ring-test
"""
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
    connectivity = root / "firmware/common/connectivity/src"
    with tempfile.TemporaryDirectory(prefix="aq-frame-ring-test-") as directory:
        executable = Path(directory) / "frame-ring-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                   "-I", str(connectivity),
                   str(root / "tools/test_frame_ring.cpp"),
                   str(connectivity / "frame_ring.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("Frame ring: prefix, Ok/Full/Closed/Rejected, wrap, partial consume, drop, "
          "24 KiB bound and differential model passed")


if __name__ == "__main__":
    main()
