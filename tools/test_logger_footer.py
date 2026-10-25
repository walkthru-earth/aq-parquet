"""Board neutral footer, cadence and BLE name checks (#90).

Compiles firmware/common/logger/tests/test_footer.cpp with the shared writer.
The C++ part pins the default footer bytes (10000, 90, unknown) the phone
reads per file, and the OS- BLE name and mDNS host. It then writes a file for
a non air quality fixture board (no PMS, 30 s period, SL- prefix), which PyArrow and DuckDB read here.
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
import parquet_footer as footer  # noqa: E402
from host_cxx import CXX_STD  # noqa: E402

FIXED_KEYS = [
    "schema_version", "device_id", "station_id", "boot_id", "firmware",
    "dictionary_version", "dictionary_uri", "dictionary_sha256",
    "acquisition_config_id", "acquisition_config", "deployment_id",
    "calibration_id", "time_semantics", "rotation_interval_s", "row_groups",
    "row_group_rows_max", "compression", "purpose", "board",
    "sample_interval_ms", "clock", "durability",
]


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
        metadata = {k.decode(): v.decode()
                    for k, v in parquet.metadata.metadata.items()}
        raw = footer.read_footer(output)
        ordered = [entry[1].decode() for entry in raw[footer.FILE_KV]]
        assert ordered[:len(FIXED_KEYS)] == FIXED_KEYS, ordered
        assert ordered[len(FIXED_KEYS):] == ["hardware_revision"], ordered
        assert metadata["sample_interval_ms"] == "30000"
        assert metadata["row_group_rows_max"] == "90"
        assert metadata["rotation_interval_s"] == "3600"
        assert metadata["row_groups"] == "2"
        assert metadata["deployment_id"] == "fixture-plot-7"
        assert metadata["calibration_id"] == "soil-cal-2026-10"
        assert metadata["schema_version"] == "soil-fixture-v1"
        assert metadata["board"] == "fixture soil board"

        groups = [parquet.metadata.row_group(i).num_rows
                  for i in range(parquet.metadata.num_row_groups)]
        assert groups == [90, 30], groups
        assert max(groups) <= int(metadata["row_group_rows_max"])
        names = parquet.schema_arrow.names
        assert not any(name.startswith("pm") for name in names), names
        assert names[-2:] == ["soil_moisture_pct", "soil_temperature_c"]

        table = parquet.read()
        mono = table.column("monotonic_us").to_pylist()
        steps = {b - a for a, b in zip(mono, mono[1:])}
        assert steps == {int(metadata["sample_interval_ms"]) * 1000}, steps
        assert len(mono) == 120

        connection = duckdb.connect()
        rows, step, nulls = connection.execute(
            "SELECT count(*), max(monotonic_us) - min(monotonic_us), "
            "count(*) - count(soil_moisture_pct) FROM read_parquet(?)",
            [str(output)]).fetchone()
        assert rows == 120 and step == 119 * 30_000_000 and nulls == 24
        duck_meta = dict(connection.execute(
            "SELECT decode(key), decode(value) FROM parquet_kv_metadata(?)",
            [str(output)]).fetchall())
        assert duck_meta["sample_interval_ms"] == "30000"
    print("Default footer bytes pinned, OS- BLE name and os- mDNS host. "
          "SL- soil fixture at 30 s wrote 90 + 30 rows with matching "
          "sample_interval_ms, read by PyArrow and DuckDB")


if __name__ == "__main__":
    main()
