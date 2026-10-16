"""Measure device Parquet files and compare host side encodings on their rows.

Run with ``pixi run size-survey <mode> <file.parquet> [...]``.

- ``survey`` prints one JSON line per file with size, footer, row groups, pages
  per column chunk (walked from the page headers), encodings, codecs, page index
  and bloom filter presence.
- ``encodings`` rewrites each file's rows with PyArrow in codec, dictionary,
  DELTA, BYTE_STREAM_SPLIT and page index variants and prints the sizes.
- ``compact`` concatenates same schema files, sorts them by event time and
  prints the size of one compacted file per variant.

The encoding numbers come from the PyArrow writer on the host, not from the
firmware writer. They size the options in docs/shared/parquet-small-files.md.
"""

from __future__ import annotations

import io
import json
from pathlib import Path
import struct
import sys

import pyarrow as pa
import pyarrow.parquet as pq


def varint(data: bytes, i: int) -> tuple[int, int]:
    value = shift = 0
    while True:
        byte = data[i]
        i += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return value, i


def zigzag(n: int) -> int:
    return (n >> 1) ^ -(n & 1)


def skip(data: bytes, i: int, kind: int) -> int:
    if kind in (1, 2):
        return i
    if kind == 3:
        return i + 1
    if kind in (4, 5, 6):
        return varint(data, i)[1]
    if kind == 7:
        return i + 8
    if kind == 8:
        n, i = varint(data, i)
        return i + n
    if kind in (9, 10):
        header = data[i]
        i += 1
        n, element = header >> 4, header & 0x0F
        if n == 15:
            n, i = varint(data, i)
        for _ in range(n):
            i = skip(data, i, element)
        return i
    if kind == 12:
        return thrift_struct(data, i)[1]
    raise ValueError(f"unknown Thrift compact type {kind}")


def thrift_struct(data: bytes, i: int) -> tuple[dict[int, int], int]:
    """Return the integer fields of one Thrift compact struct and its end."""
    fields: dict[int, int] = {}
    last = 0
    while True:
        header = data[i]
        i += 1
        if header == 0:
            return fields, i
        delta, kind = header >> 4, header & 0x0F
        if delta:
            field = last + delta
        else:
            raw, i = varint(data, i)
            field = zigzag(raw)
        last = field
        if kind in (5, 6):
            raw, i = varint(data, i)
            fields[field] = zigzag(raw)
        else:
            i = skip(data, i, kind)


def pages_in_chunk(data: bytes, start: int, total: int) -> tuple[int, bool]:
    pages, i, end = 0, start, start + total
    while i < end:
        header, body = thrift_struct(data, i)
        pages += 1
        i = body + header[3]  # PageHeader.compressed_page_size
    return pages, i == end


def survey(path: Path) -> dict:
    data = path.read_bytes()
    footer = struct.unpack("<I", data[-8:-4])[0]
    metadata = pq.ParquetFile(path).metadata
    kv = {k.decode(): v.decode() for k, v in (metadata.metadata or {}).items()}
    pages, encodings, codecs = set(), set(), set()
    column_index, offset_index, bloom = set(), set(), set()
    exact = True
    for g in range(metadata.num_row_groups):
        group = metadata.row_group(g)
        for c in range(group.num_columns):
            chunk = group.column(c)
            count, ok = pages_in_chunk(data, chunk.data_page_offset, chunk.total_compressed_size)
            pages.add(count)
            exact &= ok
            encodings.update(chunk.encodings)
            codecs.add(chunk.compression)
            column_index.add(chunk.has_column_index)
            offset_index.add(chunk.has_offset_index)
            bloom.add(getattr(chunk, "bloom_filter_offset", None) not in (None, 0, -1))
    rows = metadata.num_rows
    return {
        "file": path.name,
        "bytes": len(data),
        "rows": rows,
        "row_groups": metadata.num_row_groups,
        "rows_per_rg": [metadata.row_group(g).num_rows for g in range(metadata.num_row_groups)],
        "columns": metadata.num_columns,
        "footer": footer,
        "footer_pct": round(100 * footer / len(data), 1),
        "tail_needed": footer + 8,
        "data_bytes": len(data) - footer - 12,
        "bytes_per_row": round(len(data) / rows, 1) if rows else None,
        "kv_entries": len(kv),
        "kv_bytes": sum(len(k) + len(v) for k, v in kv.items()),
        "pages_per_chunk": sorted(pages),
        "pages_exact": exact,
        "encodings": sorted(encodings),
        "codecs": sorted(codecs),
        "column_index": sorted(column_index),
        "offset_index": sorted(offset_index),
        "bloom": sorted(bloom),
        "firmware": kv.get("firmware"),
        "schema_version": kv.get("schema_version"),
        "rotation": kv.get("rotation_interval_s"),
        "created_by": metadata.created_by,
    }


def write(table: pa.Table, **options) -> dict:
    sink = io.BytesIO()
    settings = dict(use_dictionary=False, data_page_version="1.0", store_schema=False,
                    write_statistics=True, write_page_index=False)
    settings.update(options)
    pq.write_table(table, sink, **settings)
    data = sink.getvalue()
    if not pq.read_table(io.BytesIO(data)).equals(table):
        raise AssertionError(f"round trip changed rows for {options}")
    footer = struct.unpack("<I", data[-8:-4])[0]
    return {"bytes": len(data), "footer": footer, "data": len(data) - footer - 12}


def variants(table: pa.Table) -> dict:
    ints = [f.name for f in table.schema if pa.types.is_integer(f.type) or pa.types.is_timestamp(f.type)]
    floats = [f.name for f in table.schema if pa.types.is_floating(f.type)]
    delta = {name: "DELTA_BINARY_PACKED" for name in ints}
    split = {name: "BYTE_STREAM_SPLIT" for name in floats}
    out = {}
    for codec in ("NONE", "LZ4", "SNAPPY", "ZSTD"):
        out[f"plain/{codec}"] = write(table, compression=codec)
        out[f"plain+pageindex/{codec}"] = write(table, compression=codec, write_page_index=True)
        out[f"dict/{codec}"] = write(table, compression=codec, use_dictionary=True)
        out[f"delta-int/{codec}"] = write(table, compression=codec, column_encoding=delta)
        out[f"delta-int+bss-float/{codec}"] = write(table, compression=codec, column_encoding={**delta, **split})
    return out


def sort_keys(table: pa.Table) -> list[tuple[str, str]]:
    keys = [name for name in ("station_id", "h3_cell_id", "event_time_utc_us", "monotonic_us")
            if name in table.column_names]
    return [(name, "ascending") for name in keys]


def compact(paths: list[Path]) -> dict:
    tables = [pq.read_table(p).replace_schema_metadata(None) for p in paths]
    table = pa.concat_tables(tables, promote_options="default")
    table = table.sort_by(sort_keys(table))
    device = sum(p.stat().st_size for p in paths)
    results = {}
    for codec in ("NONE", "LZ4", "SNAPPY", "ZSTD"):
        results[f"plain/{codec}"] = write(table, compression=codec)
        results[f"dict/{codec}"] = write(table, compression=codec, use_dictionary=True)
        results[f"dict+pageindex/{codec}"] = write(table, compression=codec, use_dictionary=True,
                                                   write_page_index=True)
    rows = table.num_rows
    return {
        "files": len(paths),
        "rows": rows,
        "columns": table.num_columns,
        "device_bytes_total": device,
        "device_bytes_per_row": round(device / rows, 1),
        "variants": {k: dict(v, per_row=round(v["bytes"] / rows, 1)) for k, v in results.items()},
    }


def main(argv: list[str]) -> int:
    if len(argv) < 2 or argv[0] not in ("survey", "encodings", "compact"):
        print(__doc__, file=sys.stderr)
        return 2
    mode, paths = argv[0], [Path(p) for p in argv[1:]]
    if mode == "compact":
        print(json.dumps(compact(paths)))
        return 0
    for path in paths:
        if mode == "survey":
            if not path.is_file():
                print(json.dumps({"file": path.name, "error": "not a regular file"}))
                continue
            print(json.dumps(survey(path)))
        else:
            table = pq.read_table(path).replace_schema_metadata(None)
            print(json.dumps({"file": path.name, "rows": table.num_rows, "columns": table.num_columns,
                              "device_bytes": path.stat().st_size, "variants": variants(table)}))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
