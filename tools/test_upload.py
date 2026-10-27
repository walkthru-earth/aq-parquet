"""Build and run the device uploader tests (#81) on the host."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile
from host_cxx import CXX_STD

SOURCES = (
    "os_upload_sigv4.cpp",
    "os_upload_sts.cpp",
    "os_upload_policy.cpp",
    "os_upload_ledger.cpp",
    "os_upload_session.cpp",
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    upload = root / "firmware/common/upload/src"
    with tempfile.TemporaryDirectory(prefix="os-upload-") as directory:
        executable = Path(directory) / "upload-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                   "-I", str(root / "firmware/common/src"),
                   str(root / "tools/test_upload.cpp"),
                   *(str(upload / source) for source in SOURCES),
                   "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("upload SigV4 vectors, STS body and parser, policy, ledger and "
          "session passed")


if __name__ == "__main__":
    main()
