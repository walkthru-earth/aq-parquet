"""Summarize a device Parquet file: row groups, footer facts, statistics, readers.

Run with ``pixi run python tools/inspect_parquet.py <file.parquet> [...]``.
Prints what the bench record needs (sizes, groups, annotation, statistics on
the time/sequence columns) and cross-checks PyArrow, DuckDB and the raw footer.
Exit status is nonzero when a check fails.
"""

from __future__ import annotations

from pathlib import Path
import struct
import sys

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq

sys.path.insert(0, str(Path(__file__).resolve().parent))
import parquet_footer as footer  # noqa: E402

KEY_COLUMNS = ("sequence", "monotonic_us", "event_time_utc_us", "pm25_atmospheric_ug_m3", "accel_x_g")


def inspect(path: Path) -> bool:
    ok = True
    data = path.read_bytes()
    footer_length = struct.unpack("<I", data[-8:-4])[0]
    meta = footer.read_footer(path)
    parquet = pq.ParquetFile(path)
    md = parquet.metadata
    kv = {k.decode(): v.decode() for k, v in md.metadata.items()}
    print(f"{path.name}: {len(data)} bytes, footer {footer_length} ({100 * footer_length / len(data):.1f}%), "
          f"{md.num_rows} rows, {md.num_row_groups} row groups, {md.num_columns} columns")
    print(f"  schema_version={kv.get('schema_version')} dictionary_version={kv.get('dictionary_version')} "
          f"firmware={kv.get('firmware')} compression={kv.get('compression')} rotation_interval_s={kv.get('rotation_interval_s')} "
          f"row_groups={kv.get('row_groups')} created_by={md.created_by!r}")
    schema = parquet.schema_arrow
    for name in ("event_time_utc_us", "clock_anchor_utc_us"):
        if name in schema.names:
            print(f"  {name}: {schema.field(name).type}")
    # Iceberg v2 registration needs a unique positive field_id on every leaf.
    ids = [element.get(footer.SCHEMA_FIELD_ID) for element in meta[footer.FILE_SCHEMA][1:]]
    if all(isinstance(i, int) and i > 0 for i in ids) and len(set(ids)) == len(ids):
        print(f"  field_id: {min(ids)}..{max(ids)} on {len(ids)} leaves")
    else:
        print("  !! field_id missing or duplicated on some leaves")
        ok = False
    orders = meta.get(footer.FILE_COLUMN_ORDERS)
    print(f"  column_orders: {'TYPE_ORDER x %d' % len(orders) if orders else 'absent'}")
    names = schema.names
    for g in range(md.num_row_groups):
        group = md.row_group(g)
        raw = meta[footer.FILE_ROW_GROUPS][g]
        sorting = [(s.column_index, s.descending) for s in group.sorting_columns]
        print(f"  group {g}: rows={group.num_rows} bytes={group.total_byte_size} "
              f"compressed={raw.get(footer.GROUP_COMPRESSED)} offset={raw.get(footer.GROUP_OFFSET)} "
              f"ordinal={raw.get(footer.GROUP_ORDINAL)} sorting={sorting}")
        for name in KEY_COLUMNS:
            if name not in names:
                continue
            column = group.column(names.index(name))
            stats = column.statistics
            raw_stats = raw[footer.GROUP_COLUMNS][names.index(name)][footer.CHUNK_META][footer.META_STATISTICS]
            bounds = f"min={stats.min} max={stats.max}" if stats.has_min_max else "no bounds"
            nan = f" nan_count={raw_stats[footer.STAT_NANS]}" if footer.STAT_NANS in raw_stats else ""
            deprecated = " DEPRECATED-min/max-present" if footer.STAT_MIN_DEPRECATED in raw_stats else ""
            print(f"    {name}: nulls={stats.null_count} {bounds}{nan}{deprecated}")
            if deprecated:
                ok = False
        if sorting != [(names.index("sequence"), False)]:
            print("    !! sorting_columns is not [sequence ascending]")
            ok = False
    table = parquet.read()
    seq = table.column("sequence").to_pylist()
    if seq != sorted(seq) or len(set(seq)) != len(seq):
        print("  !! sequence is not strictly increasing")
        ok = False
    with duckdb.connect() as connection:
        duck_rows = connection.execute("SELECT count(*) FROM read_parquet(?)", [str(path)]).fetchone()[0]
        duck_groups = connection.execute(
            "SELECT count(DISTINCT row_group_id) FROM parquet_metadata(?)", [str(path)]).fetchone()[0]
        utc_type = connection.execute(
            "SELECT typeof(event_time_utc_us) FROM read_parquet(?) LIMIT 1", [str(path)]).fetchone()
    print(f"  DuckDB: rows={duck_rows} row_groups={duck_groups} event_time_utc_us type={utc_type[0] if utc_type else None}")
    if duck_rows != md.num_rows or duck_groups != md.num_row_groups:
        print("  !! DuckDB disagrees with the footer")
        ok = False
    return ok


def main() -> None:
    paths = [Path(p) for p in sys.argv[1:]]
    if not paths:
        raise SystemExit(__doc__)
    results = [inspect(path) for path in paths]
    print("PASS" if all(results) else "FAIL")
    raise SystemExit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
