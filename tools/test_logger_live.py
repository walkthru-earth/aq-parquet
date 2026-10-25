"""Exercise the real shared LIVE formatter: declared keys, order, nulls, capacity."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import tempfile
from host_cxx import CXX_STD


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    common = root / "firmware/common"
    with tempfile.TemporaryDirectory(prefix="aq-logger-live-") as directory:
        executable = Path(directory) / "live-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror"]
        for library in ["src", "runtime/src", "connectivity/src", "logger/src", "power/src"]:
            command += ["-I", str(common / library)]
        command += [str(common / "logger/tests/test_live.cpp"),
                    str(common / "logger/src/aq_logger_live.cpp"),
                    "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True, check=True)
    documents = [json.loads(line, object_pairs_hook=list) for line in result.stdout.splitlines()]
    assert len(documents) == 3
    assert [key for key, _ in documents[0]] == ["seq", "pm25", "t", "pm1"]
    assert [key for key, _ in documents[1]] == ["seq", "pm25", "t", "pm1", "utc", "rh"]
    assert dict(documents[1])["utc"] == 2**63 - 1
    assert documents[2] == []
    print("Shared LIVE formatter: declared keys only, field-table order, nulls and NaN omitted, exact capacity passed, overflow clears to {} and logs once")


if __name__ == "__main__":
    main()
