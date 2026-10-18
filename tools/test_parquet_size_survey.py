"""Host tests for parquet_size_survey.py on a small PyArrow file; no device is opened."""

from __future__ import annotations

from collections.abc import Iterator
from contextlib import contextmanager
import faulthandler
import os
from pathlib import Path
import sys
import tempfile
import time
import traceback

import pyarrow as pa
import pyarrow.parquet as pq

import parquet_size_survey as survey

# The whole body takes about a second. A step that stalls past this bound
# prints every Python thread and exits 1 instead of reaching the gate timeout.
BODY_LIMIT_S = 120


@contextmanager
def step(name: str) -> Iterator[None]:
    start = time.monotonic()
    print(f"step {name} ...", flush=True)
    yield
    print(f"step {name} ok in {time.monotonic() - start:.2f} s", flush=True)


def make_file(path: Path, rows: int, *, index: bool = False, groups: int = 1) -> None:
    table = pa.table({
        "event_time_utc_us": pa.array([1_800_000_000_000_000 + i * 10_000_000 for i in range(rows)],
                                      pa.timestamp("us", tz="UTC")),
        "sequence": pa.array(range(rows), pa.int64()),
        "pm2_5": pa.array([5.0 + (i % 7) * 0.1 for i in range(rows)], pa.float32()),
    })
    pq.write_table(table, path, use_dictionary=False, compression="NONE", data_page_version="1.0",
                   write_page_index=index, row_group_size=-(-rows // groups))


def run() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        plain, indexed = root / "plain.parquet", root / "indexed.parquet"
        with step("write fixtures"):
            make_file(plain, 90, groups=2)
            make_file(indexed, 90, index=True)

        with step("survey"):
            facts = survey.survey(plain)
            assert facts["rows"] == 90 and facts["row_groups"] == 2 and facts["rows_per_rg"] == [45, 45]
            assert facts["pages_per_chunk"] == [1] and facts["pages_exact"]
            assert facts["codecs"] == ["UNCOMPRESSED"]
            assert facts["column_index"] == [False] and facts["offset_index"] == [False]
            assert facts["bytes"] == plain.stat().st_size
            assert facts["data_bytes"] + facts["footer"] + 12 == facts["bytes"]
            assert survey.survey(indexed)["column_index"] == [True]

        with step("variants"):
            table = pq.read_table(plain)
            sizes = survey.variants(table)
            assert sizes["delta-int/NONE"]["data"] < sizes["plain/NONE"]["data"]
            assert sizes["plain+pageindex/NONE"]["bytes"] > sizes["plain/NONE"]["bytes"]

        with step("compact"):
            merged = survey.compact([plain, indexed])
            assert merged["files"] == 2 and merged["rows"] == 180
            assert set(merged["variants"]) >= {"plain/LZ4", "dict/LZ4", "dict+pageindex/LZ4"}

        with step("usage error"):
            assert survey.main(["bogus"]) == 2


def main() -> None:
    faulthandler.dump_traceback_later(BODY_LIMIT_S, exit=True)
    status = 0
    try:
        run()
        print("parquet size survey: ok", flush=True)
    except BaseException:  # report it, then exit without interpreter teardown
        traceback.print_exc()
        status = 1
    # Issue #60. Runs 39042291160 and 39121587458 hung after the ok line until
    # the gate timeout, while a 60 s faulthandler watchdog armed after it never
    # fired. Py_FinalizeEx cancels that watchdog, so the stall was later, in
    # native teardown of the Arrow C++ runtime (static destructors, thread
    # pools, allocator) that the test cannot observe. Every check has already
    # run and the temporary directory is gone, so skip that teardown.
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(status)


if __name__ == "__main__":
    main()
