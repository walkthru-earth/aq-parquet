"""Host tests for parquet_size_survey.py on a small PyArrow file; no device is opened."""

from __future__ import annotations

from pathlib import Path
import tempfile

import pyarrow as pa
import pyarrow.parquet as pq

import parquet_size_survey as survey


def make_file(path: Path, rows: int, *, index: bool = False, groups: int = 1) -> None:
    table = pa.table({
        "event_time_utc_us": pa.array([1_800_000_000_000_000 + i * 10_000_000 for i in range(rows)],
                                      pa.timestamp("us", tz="UTC")),
        "sequence": pa.array(range(rows), pa.int64()),
        "pm2_5": pa.array([5.0 + (i % 7) * 0.1 for i in range(rows)], pa.float32()),
    })
    pq.write_table(table, path, use_dictionary=False, compression="NONE", data_page_version="1.0",
                   write_page_index=index, row_group_size=-(-rows // groups))


def main() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        plain, indexed = root / "plain.parquet", root / "indexed.parquet"
        make_file(plain, 90, groups=2)
        make_file(indexed, 90, index=True)

        facts = survey.survey(plain)
        assert facts["rows"] == 90 and facts["row_groups"] == 2 and facts["rows_per_rg"] == [45, 45]
        assert facts["pages_per_chunk"] == [1] and facts["pages_exact"]
        assert facts["codecs"] == ["UNCOMPRESSED"]
        assert facts["column_index"] == [False] and facts["offset_index"] == [False]
        assert facts["bytes"] == plain.stat().st_size
        assert facts["data_bytes"] + facts["footer"] + 12 == facts["bytes"]
        assert survey.survey(indexed)["column_index"] == [True]

        table = pq.read_table(plain)
        sizes = survey.variants(table)
        assert sizes["delta-int/NONE"]["data"] < sizes["plain/NONE"]["data"]
        assert sizes["plain+pageindex/NONE"]["bytes"] > sizes["plain/NONE"]["bytes"]

        merged = survey.compact([plain, indexed])
        assert merged["files"] == 2 and merged["rows"] == 180
        assert set(merged["variants"]) >= {"plain/LZ4", "dict/LZ4", "dict+pageindex/LZ4"}

        assert survey.main(["bogus"]) == 2
    print("parquet size survey: ok")


if __name__ == "__main__":
    main()
