"""Board neutral footer, cadence and BLE name checks (#90).

Compiles firmware/common/logger/tests/test_footer.cpp with the shared writer.
The C++ part pins the default `opensensor` footer JSON (10000, 90, unknown,
#88), its escaping and its fail closed errors, and the OS- BLE name and mDNS
host. It then writes a file for a non air quality fixture board (no PMS, 30 s
period, SL- prefix). PyArrow and DuckDB read it here and its one footer key
validates against the pinned stac-opensensor footer.json.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

import duckdb
import pyarrow.parquet as pq

sys.path.insert(0, str(Path(__file__).resolve().parent))
import footer_contract  # noqa: E402
from host_cxx import CXX_STD  # noqa: E402

# kDictSha and kDescSha of test_footer.cpp.
DICT_SHA = "0" * 63 + "1"
DESC_SHA = "0" * 63 + "2"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    common = root / "firmware/common"
    with tempfile.TemporaryDirectory(prefix="aq-logger-footer-") as directory:
        executable = Path(directory) / "footer-test"
        output = Path(directory) / "soil.parquet"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror"]
        for library in ["src", "runtime/src", "connectivity/src", "logger/src",
                        "power/src", "location/src"]:
            command += ["-I", str(common / library)]
        command += [str(common / "logger/tests/test_footer.cpp"),
                    str(common / "src/parquet_writer.cpp"),
                    "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        result = subprocess.run([str(executable), str(output)],
                                capture_output=True, text=True, check=True)
        assert result.stdout.split() == ["SL-89ab", "sl-89ab"], result.stdout

        parquet = pq.ParquetFile(output)
        footer = footer_contract.check_file(output, dict_sha=DICT_SHA, desc=DESC_SHA)
        acquisition, file = footer["acquisition"], footer["file"]
        assert acquisition["sample_interval_ms"] == 30000
        assert file == {"rotation_interval_s": 3600, "row_groups": 2, "row_group_rows_max": 90,
                        "compression": "UNCOMPRESSED", "purpose": "telemetry"}, file
        assert footer["station"]["deployment_id"] == "fixture-plot-7"
        assert acquisition["calibration_id"] == "soil-cal-2026-10"
        assert footer["firmware"]["schema"] == "soil-fixture-v1"
        assert footer["station"]["board"] == "fixture soil board"

        groups = [parquet.metadata.row_group(i).num_rows
                  for i in range(parquet.metadata.num_row_groups)]
        assert groups == [90, 30], groups
        assert max(groups) <= file["row_group_rows_max"]
        names = parquet.schema_arrow.names
        assert not any(name.startswith("pm") for name in names), names
        assert names[-2:] == ["soil_moisture_pct", "soil_temperature_c"]

        table = parquet.read()
        mono = table.column("monotonic_us").to_pylist()
        steps = {b - a for a, b in zip(mono, mono[1:])}
        assert steps == {acquisition["sample_interval_ms"] * 1000}, steps
        assert len(mono) == 120

        connection = duckdb.connect()
        rows, step, nulls = connection.execute(
            "SELECT count(*), max(monotonic_us) - min(monotonic_us), "
            "count(*) - count(soil_moisture_pct) FROM read_parquet(?)",
            [str(output)]).fetchone()
        assert rows == 120 and step == 119 * 30_000_000 and nulls == 24
        keys, interval = connection.execute(
            "SELECT list(decode(key)), any_value(decode(value)::JSON->>'$.acquisition.sample_interval_ms') "
            "FROM parquet_kv_metadata(?)", [str(output)]).fetchone()
        assert keys == ["opensensor"] and interval == "30000", (keys, interval)
    print("Default opensensor footer JSON pinned, OS- BLE name and os- mDNS host. "
          "SL- soil fixture at 30 s wrote 90 + 30 rows with matching "
          "sample_interval_ms in one valid opensensor key, read by PyArrow and DuckDB")


if __name__ == "__main__":
    main()
