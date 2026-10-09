"""Check allowlisted physical archive reset, partials and symlink rejection."""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

import parquet_device


def check_reset_command() -> None:
    args = SimpleNamespace(text="parquet erase-archive CONFIRM", timeout=1)
    for line, success in [
        ("PARQUET ERASE_ARCHIVE ok=1 files=200 directories=12 reboot_required=1", True),
        ("PARQUET ERASE_ARCHIVE ok=0 files=1 directories=0 reboot_required=1", False),
        ("PARQUET ERASE_ARCHIVE ok=0 reason=no-storage", False),
        ("I (4429) wifi_init: tcp mss: 1440PARQUET ERASE_ARCHIVE ok=1 files=130 directories=63 reboot_required=1", True),
        ("I (4429) wifi_init: tcp mss: 1440PARQUET ERASE_ARCHIVE ok=0 files=1 directories=0 reboot_required=1", False),
    ]:
        with patch.object(parquet_device, "send_command") as send, \
                patch.object(parquet_device, "lines_until", return_value=iter([line])), \
                patch("builtins.print"):
            try:
                assert parquet_device.command(None, args) == 0 and success
            except RuntimeError:
                assert not success
            send.assert_called_once_with(None, args.text)
    with patch.object(parquet_device, "send_command") as send:
        try:
            parquet_device.command(None, SimpleNamespace(text="parquet erase-archive", timeout=1))
            raise AssertionError("missing confirmation accepted")
        except ValueError:
            send.assert_not_called()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    check_reset_command()
    root = Path(__file__).resolve().parent.parent
    logger = root / "firmware/common/logger"
    with tempfile.TemporaryDirectory(prefix="aq-logger-archive-reset-") as directory:
        executable = Path(directory) / "archive-reset-test"
        command = ["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-I", str(logger / "src"),
                   str(logger / "tests/test_archive_reset.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("Archive reset checks passed: exact roots, partials, legacy, symlinks and depth bounds")


if __name__ == "__main__":
    main()
