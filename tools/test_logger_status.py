"""Exercise the real shared STATUS formatter at numeric capacity boundaries."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    common = root / "firmware/common"
    with tempfile.TemporaryDirectory(prefix="aq-logger-status-") as directory:
        executable = Path(directory) / "status-test"
        command = ["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror"]
        for library in ["src", "runtime/src", "connectivity/src", "logger/src", "power/src"]:
            command += ["-I", str(common / library)]
        command += [str(common / "logger/tests/test_status.cpp"),
                    str(common / "logger/src/aq_logger_status.cpp"),
                    "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True, check=True)
    snapshots = [json.loads(line) for line in result.stdout.splitlines()]
    expected_keys = {"up_s", "int_s", "buf", "fin", "drop", "err", "miss", "fail",
                     "codec", "utc", "gen", "clk", "rtc", "sd", "sd_state", "sd_kib",
                     "sd_used_kib", "heap", "part", "qf", "qb", "open", "open_rg",
                     "dc", "dr", "utc_ms", "clk_age_s"}
    assert len(snapshots) == 3
    assert {row["codec"] for row in snapshots} == {"UNCOMPRESSED", "LZ4_RAW"}
    for row in snapshots[:2]:
        assert row.keys() == expected_keys
        assert row["miss"] == -(2**63)
        assert row["qb"] == 2**64 - 1
        assert row["gen"] == -(2**31) and row["clk"] == row["rtc"] == 255
        assert row["fail"] == row["utc"] == row["sd"] == 1
        assert row["sd_state"] == 3  # 0 ready, 1 degraded, 2 full, 3 retry
        assert row["utc_ms"] == row["clk_age_s"] == 2**63 - 1
        assert row["dc"] == 2**16 - 1 and row["dr"] == 255
        for key in expected_keys - {"miss", "qb", "gen", "clk", "rtc", "fail", "utc", "sd", "sd_state", "codec", "dc", "dr", "utc_ms", "clk_age_s"}:
            assert row[key] == 2**32 - 1
    assert snapshots[2]["utc"] == 0
    assert snapshots[2]["utc_ms"] is None and snapshots[2]["clk_age_s"] is None
    assert snapshots[2]["dc"] == 0 and snapshots[2]["dr"] is None
    assert snapshots[2].keys() == expected_keys
    lengths = [len(line) for line in result.stdout.splitlines()]
    print(f"Actual STATUS formatter worst numeric payloads: {lengths} bytes; all keys retained within 480")


if __name__ == "__main__":
    main()
