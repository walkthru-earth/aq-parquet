"""Test the exact firmware dictionary, row layout and clock mapping on the host.

Optional --dictionary-out exports a machine-readable dictionary exclusively.
Synthetic fixtures do not establish device timing, SD durability or OGC compliance.
"""

from __future__ import annotations

import argparse
import contextlib
import datetime
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tempfile

from test_location import compile_h3
from types import SimpleNamespace
from unittest.mock import patch

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq
import parquet_device


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def check_file(path: Path, dictionary: dict, anchored: bool, compressed: bool, located: bool) -> None:
    parquet = pq.ParquetFile(path)
    table = parquet.read()
    fields = dictionary["fields"]
    require(table.num_rows == 90, "row count")
    require(table.column_names == [f["name"] for f in fields], "dictionary/schema order")
    types = {1: pa.int32(), 2: pa.int64(), 4: pa.float32()}
    utc_fields = {f["name"] for f in fields if f["unit"] == "ns_since_unix_epoch"}
    require(utc_fields == {"event_time_utc_ns", "clock_anchor_utc_ns"}, "UTC instant fields")
    for field, definition in zip(table.schema, fields, strict=True):
        expected = pa.timestamp("ns", tz="UTC") if field.name in utc_fields else types[definition["type"]]
        require(field.type == expected and field.nullable, f"schema: {field.name}")
    metadata = parquet.metadata.metadata
    require(metadata[b"country_iso3166_1_alpha2"] == (b"US" if located else b"unknown"), "declared country")
    require(metadata[b"country_source"] == b"owner-declared", "country provenance")
    require(metadata[b"location_source"] == b"owner-provisioned-h3-grid-center", "center provenance")
    require(metadata[b"h3_cell_id"] == (b"85283473fffffff" if located else b"unknown"), "published cell metadata")
    require(metadata[b"h3_resolution"] == (b"5" if located else b"unknown") and
            metadata[b"h3_max_resolution"] == b"5", "location resolution metadata")
    require(metadata[b"h3_center_units"] == b"1e-7 degrees", "fixed point center units")
    require(metadata[b"schema_version"].decode() == dictionary["schema"] == "cores3-telemetry-v4", "schema version")
    require(metadata[b"dictionary_version"].decode() == dictionary["dictionary"] == "cores3-telemetry-v3",
            "dictionary version includes appended H3 fields")
    require(metadata[b"dictionary_sha256"].decode() == dictionary["sha256"], "dictionary digest")
    require(parquet.metadata.created_by == f"m5stack-aq-parquet version 0.2 (build {dictionary['firmware']})",
            "firmware identity in created_by")
    group = parquet.metadata.row_group(0)
    require(parquet.metadata.num_row_groups == 1 and group.num_rows == 90, "one 90-row group")
    require([(s.column_index, s.descending) for s in group.sorting_columns] == [(fields.index(
        next(f for f in fields if f["name"] == "sequence")), False)], "rows declared sorted by sequence")
    by_name = {f["name"]: i for i, f in enumerate(fields)}
    sequence = group.column(by_name["sequence"]).statistics
    require((sequence.min, sequence.max, sequence.null_count) == (0, 89, 0), "sequence statistics")
    utc = group.column(by_name["event_time_utc_ns"]).statistics
    require(utc.null_count == (0 if anchored else 90) and utc.has_min_max == anchored, "UTC statistics")
    if anchored:
        # PyArrow presents TIMESTAMP statistics as datetimes (microseconds).
        epoch = datetime.datetime(1970, 1, 1, tzinfo=datetime.timezone.utc)
        nanos = [(bound - epoch) // datetime.timedelta(microseconds=1) * 1000 for bound in (utc.min, utc.max)]
        require(nanos == [1788890005000000000, 1788890895000000000], "UTC range in footer")
    ambient = group.column(by_name["ambient_temperature_c"]).statistics
    require(ambient.null_count == 90 and not ambient.has_min_max, "all-null column has no bounds")
    # Cast the annotated instants back to their INT64 nanoseconds for value checks.
    table = pa.table({name: column.cast(pa.int64()) if name in utc_fields else column
                      for name, column in zip(table.column_names, table.columns)})
    require(metadata[b"deployment_id"] == metadata[b"calibration_id"] == b"unknown",
            "no fabricated deployment/calibration")
    config = json.loads(metadata[b"acquisition_config"])
    require(config["sample_interval_ms"] == 10000 and config["pms_stale_after_ms"] == 5000,
            "configuration contract")
    for i in range(len(fields)):
        require(parquet.metadata.row_group(0).column(i).compression ==
                ("LZ4" if compressed else "UNCOMPRESSED"), "column codec")
    for i, row in enumerate(table.to_pylist()):
        for name, value in (("h3_cell_id", 0x85283473fffffff), ("h3_resolution", 5),
                            ("h3_center_lat_e7", 373457934), ("h3_center_lon_e7", -1219763760)):
            require(row[name] == (value if located else None), f"nullable published location: {name}")
        now = 20000000 + i * 10000000
        require(row["schema_version"] == dictionary["schema_version"] == 4 and row["sequence"] == i, "identity")
        require(row["collection_completed_mono_us"] == now + 1234, "completion time")
        require(row["pms_received_mono_us"] == (now - 250000 if i else None), "PMS receipt")
        require(row["clock_anchor_mono_us"] == (15000000 if anchored else None), "anchor mono")
        require(row["clock_anchor_utc_ns"] == (1788890000000000000 if anchored else None), "anchor UTC")
        require(row["event_time_utc_ns"] ==
                (1788890000000000000 + (now - 15000000) * 1000 if anchored else None), "UTC estimate")
        require(row["clock_epoch"] == row["clock_status"] == int(anchored), "clock status")
        require(row["ambient_temperature_c"] is None and row["battery_current_ma"] is None,
                "unsupported measurements stay null")
    with duckdb.connect() as connection:
        # DuckDB presents the annotated columns as TIMESTAMPTZ (microseconds);
        # the firmware values are microsecond-derived, so epoch_ns round-trips.
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
    # Unrelated asynchronous status lines must not terminate a schema reply.
    response = iter(["PARQUET STATUS buffered=2", "PARQUET SCHEMA BEGIN",
                     "PARQUET FIELD name=sequence", "PARQUET SCHEMA END", "AFTER"])
    with patch.object(parquet_device, "send_command") as send, \
            patch.object(parquet_device, "lines_until", return_value=response), \
            contextlib.redirect_stdout(io.StringIO()):
        require(parquet_device.command(None, SimpleNamespace(text="parquet schema", timeout=1)) == 0,
                "schema command completion")
        send.assert_called_once_with(None, "parquet schema")
        require(next(response) == "AFTER", "schema command consumes through END only")
    root = Path(__file__).resolve().parents[1]
    firmware = root / "firmware/esp-idf-cores3/bringup"
    common = root / "firmware/common/src"
    with tempfile.TemporaryDirectory(prefix="m5-contract-") as temporary:
        directory = Path(temporary)
        executable = directory / "fixture"
        command = ["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror"]
        for include in (common, root / "firmware/common/logger/src",
                        root / "firmware/common/runtime/src", root / "firmware/common/location/src",
                        directory / "h3"):
            command += ["-I", str(include)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        command += [*compile_h3(root, directory, args.sanitize),
                    str(root / "firmware/common/location/src/aq_location.cpp"),
                    str(root / "tools/telemetry_contract_fixture.cpp"),
                    str(common / "parquet_writer.cpp"), str(common / "lz4_codec.cpp"),
                    "-o", str(executable)]
        subprocess.run(command, check=True)
        dictionary = json.loads(subprocess.check_output([str(executable), "dictionary"], text=True))
        require(dictionary["sha256"] == hashlib.sha256((firmware / "telemetry_fields.inc").read_bytes()).hexdigest(),
                "stale compiled dictionary digest")
        fields = dictionary["fields"]
        baseline = [(f["name"], f["type"]) for f in fields[:73]]
        # Golden names/types/order from the 47d8f83 hardware-baseline schema.
        require(hashlib.sha256(json.dumps(baseline, separators=(",", ":")).encode()).hexdigest()
                == "55506b55208628afffbba8d7ef0f82df7469b62969e130263a3cf1cdb38aff16",
                "original 73-column schema changed")
        require(hashlib.sha256(json.dumps([(f["name"], f["type"]) for f in fields[:77]],
                                         separators=(",", ":")).encode()).hexdigest()
                == "be30fc5e84fcd266957831ede8fad352f5fd04d7ddebc0cabc56d812cf44a357",
                "original 77-column prefix changed")
        require(len(fields) == len({f["name"] for f in fields}) == 81, "unique dictionary fields")
        require(len({f["property"] for f in fields}) == 81, "unique property identifiers")
        for field in fields:
            require(re.fullmatch(r"[a-z][a-z0-9_]*", field["name"]) is not None, "safe field token")
            require(all(field[key] for key in ("procedure", "unit", "validity")), "complete metadata")
            require(field["property"] == "urn:walkthru-earth:cores3:property:" + field["name"], "local vocabulary")
        for anchored in (False, True):
            for compressed in (False, True):
                for located in (False, True):
                    path = directory / f"{anchored}-{compressed}-{located}.parquet"
                    subprocess.run([str(executable), str(path), "lz4" if compressed else "none",
                                    "anchored" if anchored else "unsynced",
                                    "located" if located else "unset"], check=True)
                    check_file(path, dictionary, anchored, compressed, located)
                    print(f"PASS 90 x 81 contract: anchored={anchored} lz4={compressed} location={located}; both readers", flush=True)
        if args.dictionary_out:
            with args.dictionary_out.open("x") as output:
                json.dump(dictionary, output, indent=2)
                output.write("\n")
    print("PASS dictionary digest, schema, nulls, clock correction and provenance")


if __name__ == "__main__":
    main()
