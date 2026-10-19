"""Host test for tools/analyze_discharge.py on a synthetic discharge run.

Run with ``pixi run analyze-discharge-test``.

The fixture is generated here with PyArrow in the archive layout of
docs/shared/telemetry-pipeline.md, so no binary is committed. It covers a
synced boot with mixed timestamp encodings, nulls and NaN in the battery
columns and one long gap, unsynced reboot loop boots with small files, a
pre schema v5 file without the new battery columns, a quarantine file, a
torn file and board logs with several boots.
"""

from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile

import pyarrow as pa
import pyarrow.parquet as pq

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze_discharge as ad  # noqa: E402

STATION = "station=00000000-0000-4000-8000-000000000001"
T0 = 1_792_454_400_000_000  # 2026-10-20T00:00:00Z in microseconds
BOOT_A = (0x0A0A0A0A0A0A0A0A, -0x0505050505050506)  # lo is negative on purpose
BOOT_B = (0x0B, 0x0B)
BOOT_C = (0x0C, 0x0C)
BOOT_D = (0x0D, 0x0D)


def require(condition: bool, description: str) -> None:
    if not condition:
        raise AssertionError(description)


def hex_boot(boot: tuple[int, int]) -> str:
    return ad.boot_text(*boot)


def write(path: Path, columns: dict, *, timestamp: bool = True) -> None:
    fields = []
    for name, values in columns.items():
        if name == "event_time_utc_us":
            kind = pa.timestamp("us", tz="UTC") if timestamp else pa.int64()
        elif name in ("battery_soc_raw_percent", "battery_charge_rate_percent_per_h"):
            kind = pa.float32()
        elif name in ("battery_mv", "battery_health_state", "clock_status"):
            kind = pa.int32()
        else:
            kind = pa.int64()
        fields.append(pa.field(name, kind))
    path.parent.mkdir(parents=True, exist_ok=True)
    pq.write_table(pa.table({f.name: pa.array(columns[f.name], type=f.type) for f in fields}), path)


def rows(boot, first: int, count: int, *, utc_start: int | None, mv_start: int, state, v5: bool = True,
         gap_after: int | None = None) -> dict:
    seq = list(range(first, first + count))
    utc = None
    if utc_start is not None:
        utc, t = [], utc_start
        for i in range(count):
            utc.append(t)
            t += 10_000_000 if i != gap_after else 120_000_000
    columns = {
        "boot_id_hi": [boot[0]] * count,
        "boot_id_lo": [boot[1]] * count,
        "sequence": seq,
        "monotonic_us": [s * 10_000_000 for s in seq],
        "event_time_utc_us": utc if utc is not None else [None] * count,
        "battery_mv": [mv_start - i for i in range(count)],
    }
    if v5:
        columns["battery_soc_raw_percent"] = [100.0 - (mv_start - 4100 + i) * -0.1 for i in range(count)]
        columns["battery_charge_rate_percent_per_h"] = [-5.0] * count
        columns["battery_health_state"] = [state(mv_start - i) for i in range(count)]
    return columns


def state_for(mv: int) -> int:
    return 4 if mv <= 3350 else 3 if mv <= 3500 else 2


def build_run(root: Path) -> None:
    dated = root / "output" / STATION / "year=2026" / "month=10" / "day=20"
    boot_a = hex_boot(BOOT_A)
    # Pre v5 file of an earlier boot, no soc, crate or state columns.
    write(dated / f"data_0000_{hex_boot(BOOT_D)}_1-60-0.parquet",
          rows(BOOT_D, 1, 60, utc_start=T0 - 700_000_000, mv_start=4180, state=state_for, v5=False),
          timestamp=False)
    first = rows(BOOT_A, 1, 90, utc_start=T0, mv_start=3440, state=state_for)
    first["battery_mv"][5] = None  # gauge VCELL read failure
    first["battery_soc_raw_percent"][6] = float("nan")
    first["battery_charge_rate_percent_per_h"][7] = None
    first["battery_health_state"][8] = None  # absent battery, never on a v5 board but must not crash
    write(dated / f"data_0000_{boot_a}_1-90-0.parquet", first)
    second = rows(BOOT_A, 91, 90, utc_start=T0 + 900_000_000, mv_start=3350, state=state_for, gap_after=40)
    write(dated / f"data_0015_{boot_a}_91-180-0.parquet", second, timestamp=False)
    # Reboot loop near cutoff, unsynced and small.
    for boot, count in ((BOOT_B, 5), (BOOT_C, 3)):
        name = hex_boot(boot)
        write(root / "output" / STATION / "unsynced" / f"boot={name}" / f"data_unsynced_{name}_1-{count}-0.parquet",
              rows(boot, 1, count, utc_start=None, mv_start=3300, state=state_for))
    quarantine = root / "output" / "quarantine" / STATION / "year=2026" / "q.parquet"
    write(quarantine, rows(BOOT_A, 181, 2, utc_start=T0, mv_start=3300, state=state_for))
    (dated / "torn.parquet").write_bytes(b"PAR1 torn")


LOG = """\
# capture started
ESP-ROM:esp32s3-20210327
rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)
12.001 PARQUET BEGIN schema=waveshare-v5 columns=62 boot={a} station=x codec=lz4 crc32_check=ok
POWER STATE state=ok mv=3600
PARQUET READY name=data_0000_{a}_1-90-0.parquet rows=90 row_groups=1 first=1 last=90
POWER STATE state=low mv=3500
POWER STATE state=critical mv=3350 action=finalize
PARQUET FLUSH reason=power-critical result=closed attempt=1 files=1
ESP-ROM:esp32s3-20210327
rst:0xf (BROWNOUT),boot:0x8 (SPI_FAST_FLASH_BOOT)
PARQUET RECOVERY quarantined=1 retained=0
PARQUET BEGIN schema=waveshare-v5 boot={b} station=x
PARQUET SCAN files=4 retained_partials=0
POWER STATE state=critical mv=3310 action=finalize
PARQUET FLUSH reason=power-critical result=busy-retry storage=absent attempt=1 files=0 retry_ms=1000
PARQUET FLUSH reason=power-critical result=closed attempt=2 files=1
PARQUET READY name=data_unsynced_{b}_1-5-0.parquet rows=5 row_groups=1 first=1 last=5
ESP-ROM:esp32s3-20210327
PARQUET BEGIN schema=waveshare-v5 boot={c} station=x
POWER STATE state=critical mv=3290 action=finalize
PARQUET FLUSH reason=power-critical result=closed attempt=1 files=1
PARQUET READY name=data_unsynced_{c}_1-3-0.parquet rows=3 row_groups=1 first=1 last=3
POWER STATE state=low mv=3430
POWER STATE state=critical mv=3290 action=finalize
PARQUET FLUSH reason=power-critical result=closed attempt=1 files=1
PARQUET ERROR operation=power-close reason=busy-retry attempts=8 files=0 closed=0
"""

LOG_TAIL = """\
PARQUET READY name=a.parquet rows=88 row_groups=1 first=1 last=88
PARQUET BEGIN schema=waveshare-v5 boot={c} station=x
POWER STATE state=critical mv=null action=finalize
PARQUET FLUSH reason=power-critical result=nothing-open attempt=2 files=0
PARQUET READY name=b.parquet rows=oops
"""


def test_parse_log() -> None:
    a, b, c = (hex_boot(x) for x in (BOOT_A, BOOT_B, BOOT_C))
    boots = ad.parse_log(LOG.format(a=a, b=b, c=c), source="capture.log")
    require(len(boots) == 3, f"three boots split at the ROM banner, got {len(boots)}")
    require([x["boot"] for x in boots] == [a, b, c], f"boot ids {[x['boot'] for x in boots]}")
    first, second, third = boots
    require(first["closed"] == 1 and first["finalize_requests"] == 1 and first["small_files"] == 0, f"{first}")
    require(first["states"] == ["ok", "low", "critical"], f"states {first['states']}")
    require(second["quarantined"] == 1, "RECOVERY before BEGIN stays with its own boot")
    require(second["retries"] == 1 and second["closed"] == 1 and second["scan_files"] == "4", f"{second}")
    require(second["small_files"] == 1 and second["ready_rows"] == [5], f"{second}")
    require(third["closed"] == 2 and third["gave_up"] == 1 and third["small_files"] == 1, f"{third}")
    summary = ad.summarize_logs(boots)
    require(summary["closed"] == 4 and summary["boots_with_repeat_close"] == [2], f"{summary}")
    require(summary["small_files"] == 2 and summary["gave_up"] == 1, f"{summary}")
    problems = ad.findings(None, summary)
    require(any("more than one power-critical close" in p for p in problems), f"{problems}")
    require(any("give ups" in p for p in problems), f"{problems}")

    # A LOG_TAIL has no ROM banner. Lines before the first BEGIN join its boot,
    # as RECOVERY lines do in a capture.
    tail = ad.parse_log(LOG_TAIL.format(c=c), source="tail.txt")
    require(len(tail) == 1 and tail[0]["boot"] == c, f"{tail}")
    require(tail[0]["nothing_open"] == 1 and tail[0]["closed"] == 0 and tail[0]["ready_files"] == 2, f"{tail}")
    require(tail[0]["ready_rows"] == [88] and tail[0]["small_files"] == 0, "a malformed rows= is not small")
    require(ad.parse_log("") == [], "empty log")
    require(len(ad.parse_log("ESP-ROM:x\nESP-ROM:y\n")) == 1, "back to back banners are one boot")


def test_run(work: Path) -> None:
    root = work / "files"
    build_run(root)
    run = ad.read_run(root)
    require(len(run["files"]) == 5, f"five readable files, got {len(run['files'])}")
    require(len(run["skipped"]) == 1 and len(run["unreadable"]) == 1, f"{run['skipped']} {run['unreadable']}")
    summary = ad.summarize_rows(run, small_rows=30, gap_s=30.0)
    require(summary["rows"] == 60 + 90 + 90 + 5 + 3, f"rows {summary['rows']}")
    require(summary["placed_rows"] == 240 and summary["unplaced_rows"] == 8, f"{summary}")
    require(summary["absent_columns"] == ["battery_charge_rate_percent_per_h", "battery_health_state",
                                          "battery_soc_raw_percent"], f"{summary['absent_columns']}")
    require(summary["absent_in_every_file"] == [], "v5 files exist")
    require(summary["nulls"]["battery_mv"] == 1, f"{summary['nulls']}")
    require(summary["nulls"]["battery_soc_raw_percent"] == 60 + 1, "absent plus NaN count as null")
    require(summary["nulls"]["battery_charge_rate_percent_per_h"] == 61, f"{summary['nulls']}")
    require(summary["nulls"]["battery_health_state"] == 61, f"{summary['nulls']}")
    require(summary["gaps"]["count"] == 2, f"the 120 s gap and the boot gap, {summary['gaps']}")
    require(summary["gaps"]["largest_s"] == 120.0, f"{summary['gaps']}")
    require(summary["battery_mv"]["first"] == 4180 and summary["battery_mv"]["min"] == 3261, f"{summary}")
    states = summary["states"]
    require(list(states["first"]) == ["low", "critical"], f"first entries {states['first']}")
    require(states["first"]["critical"] == ad.iso(T0 + 90 * 10_000_000), f"{states['first']}")
    require([t["to"] for t in states["transitions"]] == ["critical"], f"{states['transitions']}")
    require(states["critical_to_last_row_s"] > 900, f"{states}")
    boots = {b["boot"]: b for b in summary["boots"]}
    require(boots[hex_boot(BOOT_A)]["files"] == 2 and boots[hex_boot(BOOT_A)]["small_files"] == 0, f"{boots}")
    require(boots[hex_boot(BOOT_B)]["small_files"] == 1 and boots[hex_boot(BOOT_B)]["unplaced_rows"] == 5, "B")
    require(summary["small_files"] == 2, f"two small reboot loop files, got {summary['small_files']}")
    require(hex_boot(BOOT_A).endswith("fafafafafafafafa"), "negative boot_id_lo prints as unsigned hex")

    svg = ad.render_svg(ad.placed(run["rows"]), gap_s=30.0, title="t")
    require(svg.startswith("<svg") and svg.rstrip().endswith("</svg>"), "svg wraps")
    require(svg.count("<polyline") >= 8, f"each panel has segments, got {svg.count('<polyline')}")
    require("stroke-dasharray" in svg, "critical marker drawn")
    require("all null or absent" not in svg, "every panel has values")

    out = work / "report"
    log = work / "capture.log"
    log.write_text(LOG.format(a=hex_boot(BOOT_A), b=hex_boot(BOOT_B), c=hex_boot(BOOT_C)))
    buffer = io.StringIO()
    with contextlib.redirect_stdout(buffer):
        require(ad.main([str(root), "--log", str(log), "--out", str(out)]) == 0, "cli")
    text = buffer.getvalue()
    require("critical to last row" in text and "boot (log)" in text and "! " in text, text)
    require((out / "discharge.svg").is_file(), "plot written")
    saved = json.loads((out / "summary.json").read_text())
    require(saved["parquet"]["placed_rows"] == 240 and saved["logs"]["closed"] == 4, "summary.json")
    require(any("small Parquet files" in p for p in saved["findings"]), f"{saved['findings']}")

    # Log only, no Parquet folder.
    with contextlib.redirect_stdout(io.StringIO()):
        require(ad.main(["--log", str(log), "--out", str(work / "log-only")]) == 0, "log only")
    require(not (work / "log-only" / "discharge.svg").exists(), "no plot without Parquet")


def test_absent_battery(work: Path) -> None:
    root = work / "old"
    columns = rows(BOOT_D, 1, 40, utc_start=T0, mv_start=4000, state=state_for, v5=False)
    del columns["battery_mv"]
    write(root / "data_0000_x_1-40-0.parquet", columns)
    run = ad.read_run(root)
    summary = ad.summarize_rows(run, small_rows=30, gap_s=30.0)
    require(summary["absent_in_every_file"] == list(ad.BATTERY_COLUMNS), f"{summary['absent_in_every_file']}")
    require(summary["nulls"]["battery_mv"] == 40 and "battery_mv" not in summary, f"{summary}")
    require(summary["states"]["first"] == {} and "critical_to_last_row_s" not in summary["states"], "no states")
    require(run["files"][0]["boot"] == hex_boot(BOOT_D), "boot from the id columns")
    svg = ad.render_svg(ad.placed(run["rows"]), gap_s=30.0, title="t")
    require(svg.count("all null or absent") == 3, "three panels empty, state panel has fixed ticks")
    require(any("is this schema v5" in p for p in ad.findings(summary, None)), "absent columns flagged")
    require("no rows with event_time_utc_us" in ad.render_svg([], gap_s=30.0, title="t"), "empty plot")
    table = ad.table(summary, None, [])
    require("no values" in table, table)

    # No identity columns, the boot comes from the path.
    bare = work / "bare" / "unsynced" / "boot=feed"
    write(bare / "data.parquet", {"battery_mv": [3700, None]})
    record, file_rows = ad.read_file(bare / "data.parquet")
    require(record["boot"] == "feed" and record["unplaced_rows"] == 2, f"{record}")
    require([r["battery_mv"] for r in file_rows] == [3700, None], f"{file_rows}")


def main() -> None:
    require("serial" not in sys.modules, "the analysis must not import pyserial")
    test_parse_log()
    with tempfile.TemporaryDirectory(prefix="aq-discharge-") as temporary:
        work = Path(temporary)
        test_run(work)
        test_absent_battery(work)
    require("serial" not in sys.modules, "no serial module was loaded")
    print("PASS discharge analysis on a synthetic run and logs, no port opened")


if __name__ == "__main__":
    main()
