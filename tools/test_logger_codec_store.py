"""Store, load, missing key and corrupt value cases for the persisted Parquet codec."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch
from host_cxx import CXX_STD


def strip_comments(source: str) -> str:
    """Drop // and /* */ comments but keep string and char literals intact."""
    out, i, n = [], 0, len(source)
    while i < n:
        if source.startswith("//", i):
            i = source.find("\n", i)
            i = n if i < 0 else i
        elif source.startswith("/*", i):
            end = source.find("*/", i + 2)
            i = n if end < 0 else end + 2
            out.append(" ")
        elif source[i] in "\"'":
            quote, j = source[i], i + 1
            while j < n and source[j] != quote:
                j += 2 if source[j] == "\\" else 1
            out.append(source[i:j + 1])
            i = j + 1
        else:
            out.append(source[i])
            i += 1
    return "".join(out)


def check_wiring(source: str) -> None:
    """begin() boots through apply_boot and the command saves through select.

    The helpers themselves run in the C++ test below. Here the live code must
    call them, outside comments, and nothing else may assign selected_codec.
    """
    code = " ".join(strip_comments(source).split())
    for call in ("const auto boot_codec = codec_store::apply_boot(selected_codec);",
                 "const auto saved = codec_store::select(selected_codec, requested);"):
        assert code.count(call) == 1, f"os_logger.cpp must call `{call}` exactly once"
    writes = re.findall(r"\bselected_codec\s*(?:=(?!=)|\.store\s*\()", code)
    assert not writes, "selected_codec is only written by apply_boot and select"
    assert "codec=UNCOMPRESSED" not in code, "a boot line still hard codes the codec"


def check_host_command() -> None:
    """parquet_device prints the codec reply even when the NVS save fails."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import parquet_device

    def run(lines: list[str]) -> tuple[int | str, list[str]]:
        printed: list[str] = []
        with patch.object(parquet_device, "send_command"), \
                patch.object(parquet_device, "lines_until", return_value=iter(lines)), \
                patch("builtins.print", side_effect=lambda line, **_: printed.append(line)):
            try:
                return parquet_device.command(None, SimpleNamespace(text="parquet codec none", timeout=1)), printed
            except RuntimeError as error:
                return str(error), printed

    config = "PARQUET CONFIG codec=UNCOMPRESSED persistent=true"
    assert run([config]) == (0, [config])
    store = "PARQUET ERROR operation=codec-store reason=commit"
    refused = "PARQUET CONFIG codec=UNCOMPRESSED persistent=false"
    assert run([store, refused]) == (store, [store, refused]), "the reply must print before the save error"
    storage = "PARQUET ERROR operation=codec reason=storage-full"
    assert run([storage, config]) == (storage, [storage]), "other errors still stop at once"
    lost = f"{store} (no PARQUET CONFIG reply before deadline)"
    assert run([store]) == (lost, [store]), "a save error without a reply still raises the save error"
    try:
        run([])
    except TimeoutError:
        pass
    else:
        raise AssertionError("no reply at all must time out")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    logger = root / "firmware/common/logger"
    check_wiring((logger / "src/os_logger.cpp").read_text())
    check_host_command()
    with tempfile.TemporaryDirectory(prefix="aq-codec-store-") as directory:
        executable = Path(directory) / "codec-store-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                   "-I", str(root / "tools/logger_codec_host_fakes"),
                   "-I", str(root / "firmware/common/src"),
                   "-I", str(logger / "src"),
                   str(logger / "tests/test_codec_store.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("Codec store: wire values, missing key default, store and reload, corrupt value and "
          "wrong type fallback, open/read/write/commit failures, boot and command wiring, host codec reply passed")


if __name__ == "__main__":
    main()
