"""Store, load, missing key and corrupt value cases for the persisted Parquet codec."""
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
    logger = root / "firmware/common/logger"
    # aq_logger.cpp must boot from the store and persist every codec command.
    source = (logger / "src/aq_logger.cpp").read_text()
    for call in ("codec_store::resolve(codec_store::load())", "codec_store::store(selected_codec)"):
        assert call in source, f"aq_logger.cpp no longer calls {call}"
    assert "codec=UNCOMPRESSED" not in source, "a boot line still hard codes the codec"
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
          "wrong type fallback, open/read/write/commit failures passed")


if __name__ == "__main__":
    main()
