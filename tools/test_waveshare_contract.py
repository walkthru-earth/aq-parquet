"""Check the Waveshare PMS5003T dictionary and synthetic files with two readers.

Host fixtures establish field mapping and file contracts, not SD endurance,
power-loss durability, environmental accuracy or device sampling timing.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def check_file(path: Path, dictionary: dict, anchored: bool, compressed: bool) -> None:
    parquet = pq.ParquetFile(path)
    table = parquet.read()
    fields = dictionary["fields"]
    require(table.num_rows == 90, "row count")
    require(table.column_names == [field["name"] for field in fields], "dictionary order")
    utc_fields = {field["name"] for field in fields if field["unit"] == "ns_since_unix_epoch"}
    require(utc_fields == {"event_time_utc_ns", "clock_anchor_utc_ns"}, "UTC annotation scope")
    types = {1: pa.int32(), 2: pa.int64(), 4: pa.float32()}
    for field, definition in zip(table.schema, fields, strict=True):
        expected = pa.timestamp("ns", tz="UTC") if field.name in utc_fields else types[definition["type"]]
        require(field.type == expected and field.nullable, f"schema: {field.name}")
    metadata = parquet.metadata.metadata
    require(metadata[b"schema_version"].decode() == dictionary["schema"] ==
            "waveshare-sim7670g-telemetry-v1", "schema identity")
    require(metadata[b"dictionary_version"].decode() == dictionary["dictionary"], "dictionary identity")
    require(metadata[b"dictionary_uri"].decode() == dictionary["uri"], "dictionary URI")
    require(metadata[b"dictionary_sha256"].decode() == dictionary["sha256"], "dictionary digest")
    require(parquet.metadata.created_by ==
            f"aq-parquet version 0.1 (build {dictionary['firmware']})", "Waveshare writer identity")
    require(metadata[b"deployment_id"] == metadata[b"calibration_id"] == b"unknown", "provenance")
    config = json.loads(metadata[b"acquisition_config"])
    require(config["sample_interval_ms"] == 10000 and config["pms_stale_after_ms"] == 5000 and
            config["pms_warmup_us"] == 30000000 and config["pms_model"] == "PMS5003T", "configuration")
    require((config["pms_uart_rx"], config["pms_uart_tx"], config["pms_baud"]) == (1, 2, 9600),
            "board UART mapping")
    group = parquet.metadata.row_group(0)
    require(parquet.metadata.num_row_groups == 1 and group.num_rows == 90, "row groups")
    by_name = {field["name"]: i for i, field in enumerate(fields)}
    require([(column.column_index, column.descending) for column in group.sorting_columns] ==
            [(by_name["sequence"], False)], "sequence ordering declaration")
    sequence = group.column(by_name["sequence"]).statistics
    require((sequence.min, sequence.max, sequence.null_count) == (0, 89, 0), "sequence bounds")
    ambient = group.column(by_name["ambient_temperature_c"]).statistics
    require(ambient.null_count == 5 and abs(ambient.min + 3.7) < 1e-6 and
            abs(ambient.max + 3.7) < 1e-6, "signed temperature bounds")
    utc = group.column(by_name["event_time_utc_ns"]).statistics
    require(utc.null_count == (0 if anchored else 90) and utc.has_min_max == anchored, "UTC bounds")
    for name in ("battery_mv", "battery_percent"):
        stats = group.column(by_name[name]).statistics
        require(stats.null_count == 90 and not stats.has_min_max, "no installed battery")
    for i in range(len(fields)):
        require(group.column(i).compression == ("LZ4" if compressed else "UNCOMPRESSED"), "codec")
    table = pa.table({name: column.cast(pa.int64()) if name in utc_fields else column
                      for name, column in zip(table.column_names, table.columns, strict=True)})
    for i, row in enumerate(table.to_pylist()):
        now = 10000000 + i * 10000000
        require(row["schema_version"] == 1 and row["sequence"] == i, "row identity")
        require(row["monotonic_us"] == now and row["scheduled_us"] == now - 123 and
                row["sample_jitter_us"] == 123, "synthetic cadence")
        require(row["collection_completed_mono_us"] == now + 1234, "collection completion")
        require(row["pms_received_mono_us"] == (now - 250000 if i else None), "frame receipt")
        require(row["pms_status"] == ([0, 1, 2, 3, 5][i] if i < 5 else 4), "PMS gates")
        require(row["pms_frames"] == 1 and row["pms_checksum_errors"] == 2 and
                row["pms_length_errors"] == 3, "visible parser counters")
        for offset, name in enumerate(("pm1_cf1_ug_m3", "pm25_cf1_ug_m3", "pm10_cf1_ug_m3",
                                       "pm1_atmospheric_ug_m3", "pm25_atmospheric_ug_m3",
                                       "pm10_atmospheric_ug_m3", "particles_gt03_per_01l",
                                       "particles_gt05_per_01l", "particles_gt10_per_01l",
                                       "particles_gt25_per_01l")):
            require(row[name] == (offset + 1 if i >= 5 else None), f"PMS word mapping: {name}")
        require(row["ambient_temperature_c"] is None if i < 5 else
                abs(row["ambient_temperature_c"] + 3.7) < 1e-6, "temperature validity")
        require(row["relative_humidity_percent"] is None if i < 5 or i == 6 else
                abs(row["relative_humidity_percent"] - 63.4) < 1e-5, "RH validity and range")
        require(row["gauge_status"] == 0 and row["battery_mv"] is None and
                row["battery_percent"] is None, "USB-only battery nulls")
        require(row["clock_status"] == row["clock_epoch"] == int(anchored), "clock provenance")
        require(row["clock_anchor_mono_us"] == (15000000 if anchored else None) and
                row["clock_anchor_utc_ns"] == (1788890000000000000 if anchored else None), "anchor")
        require(row["event_time_utc_ns"] ==
                (1788890000000000000 + (now - 15000000) * 1000 if anchored else None), "UTC estimate")
    with duckdb.connect() as connection:
        other = connection.execute(
            "SELECT * REPLACE (epoch_ns(event_time_utc_ns) AS event_time_utc_ns, "
            "epoch_ns(clock_anchor_utc_ns) AS clock_anchor_utc_ns) "
            "FROM read_parquet(?, hive_partitioning=false)", [str(path)]).to_arrow_table()
    require(table.equals(other, check_metadata=False), "PyArrow/DuckDB values and nulls")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--dictionary-out", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    firmware = root / "firmware/arduino-waveshare-sim7670g/logger"
    common = root / "firmware/common"
    with tempfile.TemporaryDirectory(prefix="waveshare-contract-") as temporary:
        directory = Path(temporary)
        executable = directory / "fixture"
        command = ["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror"]
        for include in ("src", "logger/src", "connectivity/src", "runtime/src"):
            command += ["-I", str(common / include)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        command += [str(root / "tools/test_waveshare_contract.cpp"),
                    str(common / "src/parquet_writer.cpp"), str(common / "src/lz4_codec.cpp"),
                    str(common / "src/pms_frame.cpp"), "-o", str(executable)]
        subprocess.run(command, check=True)
        dictionary = json.loads(subprocess.check_output([str(executable), "dictionary"], text=True))
        require(dictionary["sha256"] == hashlib.sha256((firmware / "telemetry_fields.inc").read_bytes()).hexdigest(),
                "stale compiled dictionary digest")
        fields = dictionary["fields"]
        names = {field["name"] for field in fields}
        require(len(fields) == len(names) == 49, "unique versioned fields")
        require(len({field["property"] for field in fields}) == len(fields), "property identifiers")
        require(not names.intersection({"particles_gt50_per_01l", "particles_gt100_per_01l",
                                        "imu_temperature_c", "accel_x_g", "rtc_read_ok", "touch_points"}),
                "no invented PMS5003T bins or CoreS3 peripherals")
        require(dictionary["firmware"] == "arduino-waveshare-parquet-v1", "firmware identity")
        for field in fields:
            require(re.fullmatch(r"[a-z][a-z0-9_]*", field["name"]) is not None, "safe field name")
            require(all(field[key] for key in ("procedure", "unit", "validity")), "complete metadata")
            require(field["property"] == "urn:walkthru-earth:waveshare-sim7670g:property:" + field["name"],
                    "local vocabulary")
        for anchored in (False, True):
            for compressed in (False, True):
                path = directory / f"{anchored}-{compressed}.parquet"
                subprocess.run([str(executable), str(path), "lz4" if compressed else "none",
                                "anchored" if anchored else "unsynced"], check=True)
                check_file(path, dictionary, anchored, compressed)
                print(f"PASS Waveshare 90 x 49: anchored={anchored} lz4={compressed}; both readers", flush=True)
        if args.dictionary_out:
            with args.dictionary_out.open("x") as output:
                json.dump(dictionary, output, indent=2)
                output.write("\n")
    print("PASS PMS5003T mapping, null gates, dictionary, clock epochs and provenance")


if __name__ == "__main__":
    main()
