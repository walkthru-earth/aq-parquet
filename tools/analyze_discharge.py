#!/usr/bin/env python3
"""Battery discharge run analysis for #28 (issue #61 item 1).

Reads every finalized Parquet file of one run, plots the MAX17048 columns
against time and, given board logs, counts power-critical closes and small
files per boot. Nothing here opens a serial port or touches the board.

Columns plotted, Waveshare schema v5 (`telemetry_fields.inc`)

    battery_mv                          VCELL, mV
    battery_soc_raw_percent             SOC, uncapped percent
    battery_charge_rate_percent_per_h   CRATE, percent per hour
    battery_health_state                0 unknown, 1 external_power_likely,
                                        2 ok, 3 low, 4 critical, 5 settling

Time is `event_time_utc_us`. Rows without UTC (an `unsynced/boot=` file from
a boot that never got host time) cannot be placed on the run's time axis,
so they are counted as unplaced and still count toward the per boot file
table. Files written before schema v5 lack the last four columns. They are
read anyway, the missing columns stay null and the summary lists them.

Output, under `--out` (default the ignored
`firmware/esp-idf-waveshare-sim7670g/artifacts/discharge-<UTC stamp>/`)

    discharge.svg   four panels against UTC, gaps and nulls break the line
    summary.json    everything the printed table shows, plus the details

A short table goes to stdout. Lines starting with `!` are pass criteria from
#61 item 1 that the data fails. They are a row gap longer than `--gap-s`
before the first critical row, Critical to cutoff under 182 s (twice the
91 s retry budget), more than one small file from reboot loop boots, two
power-critical closes or two `action=finalize` lines in one boot, a finalize
with no close, a power-close give up, an unreadable file, or a battery
column absent from every file. The gap at cutoff and the gaps after it, such
as the jump to a recharge boot, are expected and not flagged.

After the discharge run, for the owner (#61 item 1, after step 8)

    pixi run parquet-device command --port /dev/cu.wchusbserial5B901533371 "parquet list"
    pixi run parquet-device fetch --port /dev/cu.wchusbserial5B901533371 <name> \\
        --out firmware/esp-idf-waveshare-sim7670g/artifacts/discharge-<date>/files
    pixi run analyze-discharge firmware/esp-idf-waveshare-sim7670g/artifacts/discharge-<date>/files \\
        --log firmware/esp-idf-waveshare-sim7670g/artifacts/discharge-<date>/ble-log-1.txt \\
        --log firmware/esp-idf-waveshare-sim7670g/artifacts/discharge-<date>/ble-log-2.txt \\
        --log firmware/esp-idf-waveshare-sim7670g/artifacts/discharge-<date>/boot-after-discharge.log \\
        --out firmware/esp-idf-waveshare-sim7670g/artifacts/discharge-<date>/report

Run `fetch` once per name from `parquet list`. `--log` takes any number of
serial captures (`pixi run capture`) or LOG_TAIL dumps (`pixi run ble-sync
log`), oldest first. A log is split into boots at the ROM banner (`ESP-ROM:`)
or at a second `PARQUET BEGIN`, so one capture spanning a reboot loop is
fine. Lines before the first `PARQUET BEGIN` of a boot, such as `PARQUET
RECOVERY`, count toward that boot. LOG_TAIL is a ring, so polls of one boot
repeat each other. A boot from a later log joins an earlier one when the
`boot=` ids match, or, when the BEGIN line scrolled out, when the event lines
of one hold the other or overlap by at least two lines, and only the new
lines count. A later boot that names its id joins an earlier one without an
id only when it holds all of it, so a loop boot whose first lines repeat the
tail of the boot before stays apart. Boots within one log are never joined.
The logs carry no wall time, so the time from Critical to cutoff comes from
the Parquet rows. It runs from the first critical row to the last row of
the same boot before a gap over `--gap-s`, so a later recharge boot does not
stretch it. A later boot that holds several earlier polls without an id
takes in all of them.

A small file has fewer than `--small-rows` rows (default 30, five minutes at
10 s). The first file of every boot is short because startup lands mid
window, and the power-critical close is short too, so only small files from
reboot loop boots count toward the `!`. In the Parquet rows a loop boot is
not the boot that went critical, never reads above `--loop-mv` (default
3500, the Low threshold), and has no placed row before the first critical
row. In the logs a loop boot comes after the first boot that logged
`state=critical` and never logged `ok` or `external_power_likely`.
"""

from __future__ import annotations

import argparse
import datetime
import html
import json
import math
from pathlib import Path
import re
import statistics
import sys
from typing import Iterable

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ARTIFACTS = ROOT / "firmware/esp-idf-waveshare-sim7670g/artifacts"

BATTERY_COLUMNS = (
    "battery_mv",
    "battery_soc_raw_percent",
    "battery_charge_rate_percent_per_h",
    "battery_health_state",
)
ID_COLUMNS = ("boot_id_hi", "boot_id_lo", "sequence", "monotonic_us", "event_time_utc_us")
STATE_NAMES = {0: "unknown", 1: "external_power_likely", 2: "ok", 3: "low", 4: "critical", 5: "settling"}
MASK64 = (1 << 64) - 1

DEFAULT_SMALL_ROWS = 30
DEFAULT_GAP_S = 30.0
DEFAULT_LOOP_MV = 3500  # Low enter, kBatteryThresholds
RETRY_BUDGET_S = 91  # power-close retries, about 91 s in all

# ----------------------------------------------------------------------------
# Parquet side


def find_files(root: Path) -> tuple[list[Path], list[Path]]:
    """Finalized `.parquet` files under `root`, and the skipped quarantine files."""
    files, skipped = [], []
    for path in sorted(Path(root).rglob("*.parquet")):
        if not path.is_file():
            continue
        if "quarantine" in path.relative_to(root).parts:
            skipped.append(path)
        else:
            files.append(path)
    return files, skipped


def boot_text(hi, lo) -> str | None:
    if hi is None or lo is None:
        return None
    return f"{int(hi) & MASK64:016x}{int(lo) & MASK64:016x}"


def boot_from_path(path: Path) -> str:
    for part in path.parts:
        if part.startswith("boot="):
            return part[5:]
    match = re.search(r"_([0-9a-f]{32})_", path.name)
    return match.group(1) if match else "unknown"


def read_file(path: Path) -> tuple[dict, list[dict]]:
    """One file to (file record, rows). Absent or null values are None."""
    import pyarrow as pa
    import pyarrow.parquet as pq

    handle = pq.ParquetFile(path)
    names = set(handle.schema_arrow.names)
    wanted = [c for c in ID_COLUMNS + BATTERY_COLUMNS if c in names]
    table = handle.read(columns=wanted)
    columns: dict[str, list] = {}
    for name in wanted:
        column = table.column(name)
        if pa.types.is_timestamp(column.type):
            column = column.cast(pa.timestamp("us", tz=column.type.tz)).cast(pa.int64())
        columns[name] = column.to_pylist()
    count = table.num_rows
    blank = [None] * count
    rows = []
    for i in range(count):
        row = {name: (columns[name][i] if name in columns else None) for name in ID_COLUMNS + BATTERY_COLUMNS}
        for name in BATTERY_COLUMNS:
            value = row[name]
            if isinstance(value, float) and not math.isfinite(value):
                row[name] = None
        row["boot"] = boot_text(row.pop("boot_id_hi"), row.pop("boot_id_lo")) or boot_from_path(path)
        row["utc_us"] = row.pop("event_time_utc_us")
        rows.append(row)
    boots = sorted({row["boot"] for row in rows}) or [boot_from_path(path)]
    record = {
        "path": str(path),
        "rows": count,
        "boot": boots[0],
        "absent": [c for c in BATTERY_COLUMNS if c not in names],
        "unplaced_rows": sum(1 for v in columns.get("event_time_utc_us", blank) if v is None),
    }
    return record, rows


def read_run(root: Path) -> dict:
    files, skipped = find_files(root)
    records, rows, unreadable = [], [], []
    for path in files:
        try:
            record, file_rows = read_file(path)
        except Exception as error:  # keep going, a torn file must not hide the rest
            unreadable.append({"path": str(path), "error": f"{type(error).__name__}: {error}"})
            continue
        records.append(record)
        rows += file_rows
    return {"files": records, "rows": rows, "unreadable": unreadable, "skipped": [str(p) for p in skipped]}


def iso(us: int | None) -> str | None:
    if us is None:
        return None
    moment = datetime.datetime.fromtimestamp(us / 1e6, tz=datetime.timezone.utc)
    return moment.strftime("%Y-%m-%dT%H:%M:%SZ")


def placed(rows: Iterable[dict]) -> list[dict]:
    """Rows with UTC, sorted by time, duplicates by (boot, sequence) dropped."""
    seen, out = set(), []
    for row in sorted((r for r in rows if r["utc_us"] is not None), key=lambda r: r["utc_us"]):
        key = (row["boot"], row["sequence"])
        if key in seen and row["sequence"] is not None:
            continue
        seen.add(key)
        out.append(row)
    return out


def summarize_rows(run: dict, *, small_rows: int, gap_s: float, loop_mv: int = DEFAULT_LOOP_MV) -> dict:
    rows = run["rows"]
    series = placed(rows)
    summary: dict = {
        "files": len(run["files"]),
        "rows": len(rows),
        "placed_rows": len(series),
        "unplaced_rows": len(rows) - sum(1 for r in rows if r["utc_us"] is not None),
        "unreadable": run["unreadable"],
        "skipped_quarantine": len(run["skipped"]),
        "absent_columns": sorted({c for f in run["files"] for c in f["absent"]}),
        "absent_in_every_file": [c for c in BATTERY_COLUMNS
                                 if run["files"] and all(c in f["absent"] for f in run["files"])],
        "nulls": {c: sum(1 for r in rows if r[c] is None) for c in BATTERY_COLUMNS},
    }
    critical = next((r for r in series if r["battery_health_state"] == 4), None)
    if series:
        start, end = series[0]["utc_us"], series[-1]["utc_us"]
        summary.update(start=iso(start), end=iso(end), hours=round((end - start) / 3.6e9, 3))
        gaps = [(b["utc_us"] - a["utc_us"]) / 1e6 for a, b in zip(series, series[1:])]
        long_gaps = [(series[i]["utc_us"], g) for i, g in enumerate(gaps) if g > gap_s]
        # The gap at cutoff and anything after it, such as the recharge boot,
        # are expected. Only a gap before the first critical row is a lost row.
        before = [g for at, g in long_gaps if critical is None or at < critical["utc_us"]]
        summary["gaps"] = {
            "threshold_s": gap_s,
            "count": len(long_gaps),
            "before_critical": len(before),
            "largest_s": round(max((g for _, g in long_gaps), default=0.0), 1),
            "largest_before_critical_s": round(max(before, default=0.0), 1),
            "first": [{"after": iso(at), "seconds": round(g, 1)} for at, g in long_gaps[:10]],
        }
        if gaps:
            summary["median_step_s"] = round(statistics.median(gaps), 2)
    for column in BATTERY_COLUMNS[:3]:
        values = [(r["utc_us"], r[column]) for r in series if r[column] is not None]
        if values:
            only = [v for _, v in values]
            summary[column] = {"first": values[0][1], "last": values[-1][1], "min": min(only), "max": max(only)}
    summary["states"] = state_track(series, gap_s=gap_s)
    summary["loop_mv"] = loop_mv
    summary["boots"] = boots_from_run(run, small_rows=small_rows, critical=critical, loop_mv=loop_mv)
    summary["small_files"] = sum(b["small_files"] for b in summary["boots"])
    summary["loop_small_files"] = sum(b["small_files"] for b in summary["boots"] if b["loop"])
    return summary


def critical_segment(series: list[dict], gap_s: float) -> list[dict]:
    """Rows from the first critical row on, while the boot stays the same and no step exceeds `gap_s`."""
    start = next((i for i, r in enumerate(series) if r["battery_health_state"] == 4), None)
    if start is None:
        return []
    segment = [series[start]]
    for row in series[start + 1:]:
        last = segment[-1]
        if row["boot"] != last["boot"] or (row["utc_us"] - last["utc_us"]) / 1e6 > gap_s:
            break
        segment.append(row)
    return segment


def state_track(series: list[dict], *, gap_s: float = DEFAULT_GAP_S) -> dict:
    """Transitions and first entry times of `battery_health_state`, placed rows only."""
    transitions, first_entry, counts = [], {}, {}
    previous = None
    for row in series:
        code = row["battery_health_state"]
        if code is None:
            continue
        name = STATE_NAMES.get(code, f"code-{code}")
        counts[name] = counts.get(name, 0) + 1
        first_entry.setdefault(name, row["utc_us"])
        if previous is not None and code != previous:
            transitions.append({"at": iso(row["utc_us"]), "from": STATE_NAMES.get(previous, str(previous)),
                                "to": name, "mv": row["battery_mv"]})
        previous = code
    track = {"rows": counts, "first": {k: iso(v) for k, v in first_entry.items()}, "transitions": transitions}
    segment = critical_segment(series, gap_s)
    if segment:
        track["critical_to_last_row_s"] = round((segment[-1]["utc_us"] - segment[0]["utc_us"]) / 1e6, 1)
        track["critical_last_row"] = iso(segment[-1]["utc_us"])
        mv = [r["battery_mv"] for r in segment if r["battery_mv"] is not None]
        if mv:
            track["last_mv"] = mv[-1]
    return track


def boots_from_run(run: dict, *, small_rows: int, critical: dict | None, loop_mv: int) -> list[dict]:
    """Files, rows and small files per boot, and whether the boot is part of a reboot loop.

    A loop boot is not the boot that went critical, its cell never read above
    `loop_mv`, and its placed rows, when it has any, all come at or after the
    first critical row. So the short first file and the power-critical close
    of the discharge boot, and a recharge boot on USB, never count as loop.
    """
    boots: dict[str, dict] = {}
    for record in run["files"]:
        entry = boots.setdefault(record["boot"], {"boot": record["boot"], "files": 0, "rows": 0, "small_files": 0,
                                                  "unplaced_rows": 0, "loop": False})
        entry["files"] += 1
        entry["rows"] += record["rows"]
        entry["unplaced_rows"] += record["unplaced_rows"]
        if record["rows"] < small_rows:
            entry["small_files"] += 1
    if critical is None:
        return list(boots.values())
    by_boot: dict[str, list[dict]] = {}
    for row in run["rows"]:
        by_boot.setdefault(row["boot"], []).append(row)
    for name, entry in boots.items():
        rows = by_boot.get(name, [])
        mv = [r["battery_mv"] for r in rows if r["battery_mv"] is not None]
        times = [r["utc_us"] for r in rows if r["utc_us"] is not None]
        entry["loop"] = (name != critical["boot"] and bool(mv) and max(mv) <= loop_mv
                         and (not times or min(times) >= critical["utc_us"]))
    return list(boots.values())


# ----------------------------------------------------------------------------
# Log side

LOG_EVENT = re.compile(r"(POWER STATE|PARQUET (?:BEGIN|READY|FLUSH|ERROR|SCAN|RECOVERY|PARTIAL))\b(.*)")
ROM_BANNER = re.compile(r"ESP-ROM:")
PAIR = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(\S*)")


def split_log(text: str, *, source: str = "log") -> list[dict]:
    """Split one serial capture or LOG_TAIL into boots of event lines.

    A boot starts at the ROM banner, or at a `PARQUET BEGIN` when the current
    boot already had one. Lines before the first BEGIN of a boot, such as
    `PARQUET RECOVERY`, stay with that boot. An event keeps only the text from
    its keyword on, so the same line in a capture and in a LOG_TAIL compare
    equal.
    """
    boots: list[dict] = []
    current = None

    def start() -> dict:
        boot = {"sources": [source], "boot": None, "begun": False, "events": []}
        boots.append(boot)
        return boot

    for line in text.splitlines():
        if ROM_BANNER.search(line):
            if current is None or current["events"] or current["begun"]:
                current = start()
            continue
        match = LOG_EVENT.search(line)
        if not match:
            continue
        kind = match.group(1)
        if kind == "PARQUET BEGIN":
            if current is None or current["begun"]:
                current = start()
            current["begun"] = True
            current["boot"] = dict(PAIR.findall(match.group(2))).get("boot")
            continue
        if current is None:
            current = start()
        current["events"].append(f"{kind}{match.group(2)}".rstrip())
    for boot in boots:
        del boot["begun"]
    return boots


def overlap(old: list[str], new: list[str]) -> int:
    """Length of the longest suffix of `old` that is also a prefix of `new`."""
    for k in range(min(len(old), len(new)), 0, -1):
        if old[-k:] == new[:k]:
            return k
    return 0


def contains(outer: list[str], inner: list[str]) -> bool:
    n = len(inner)
    return 0 < n <= len(outer) and any(outer[i:i + n] == inner for i in range(len(outer) - n + 1))


def merge_boots(chunks: list[dict]) -> list[dict]:
    """Join the pieces of one boot that several logs repeat, logs given in time order.

    LOG_TAIL is a ring, so polls of one boot overlap and an old BEGIN line can
    scroll out. A piece joins an earlier boot from another log when the boot
    ids match, or, when either id is unknown, when one holds the other or the
    earlier one's tail overlaps the piece's head by at least two events. A
    piece with a known id joins a boot without one only when it holds all of
    its events, two or more. A piece that holds several such boots takes in
    all of them. Only the new events are added. Boots within one log are
    never joined, its own split stands.
    """
    merged: list[dict] = []
    for chunk in chunks:
        new = chunk["events"]
        target = None
        for boot in reversed(merged):
            if chunk["sources"][0] in boot["sources"]:
                continue
            old = boot["events"]
            same = chunk["boot"] is not None and chunk["boot"] == boot["boot"]
            unknown = chunk["boot"] is None or boot["boot"] is None
            # A piece that names its boot printed its BEGIN, so it starts no
            # later than any piece of that boot without an id. It joins one
            # only by holding it, never by overlap, or a loop boot whose first
            # lines repeat the tail of the boot before would vanish into it.
            held = len(old) >= 2 and contains(new, old)
            loose = chunk["boot"] is None and (contains(old, new) or overlap(old, new) >= 2)
            if same or (unknown and (held or loose)):
                target = boot
                break
        if target is None:
            merged.append({"sources": list(chunk["sources"]), "boot": chunk["boot"], "events": list(new)})
            continue
        old = target["events"]
        if contains(new, old) or not old:
            target["events"] = list(new)
            # Polls without an id that share no line stay apart until a piece
            # holds them all, so it takes in every one of them, not only the
            # newest, or an older poll's lines would count twice.
            for boot in [b for b in merged if b is not target and b["boot"] is None
                         and chunk["sources"][0] not in b["sources"]
                         and len(b["events"]) >= 2 and contains(new, b["events"])]:
                merged.remove(boot)
                target["sources"] = [*boot["sources"], *target["sources"]]
        elif not contains(old, new):
            target["events"] = old + new[overlap(old, new):]
        target["boot"] = target["boot"] or chunk["boot"]
        target["sources"].append(chunk["sources"][0])
    return merged


def count_boot(boot: dict, index: int, small_rows: int) -> dict:
    out = {"source": ", ".join(Path(s).name for s in boot["sources"]), "index": index, "boot": boot["boot"],
           "lines": len(boot["events"]), "states": [], "finalize_requests": 0, "closed": 0, "retries": 0,
           "gave_up": 0, "nothing_open": 0, "ready_files": 0, "small_files": 0, "ready_rows": [],
           "scan_files": None, "quarantined": 0, "errors": 0, "loop": False}
    for event in boot["events"]:
        match = LOG_EVENT.match(event)
        kind = match.group(1)
        fields = dict(PAIR.findall(match.group(2)))
        if kind == "POWER STATE":
            out["states"].append(fields.get("state", "?"))
            if fields.get("action") == "finalize":
                out["finalize_requests"] += 1
        elif kind == "PARQUET FLUSH" and fields.get("reason") == "power-critical":
            result = fields.get("result")
            if result == "closed":
                out["closed"] += 1
            elif result == "nothing-open":
                out["nothing_open"] += 1
            else:
                out["retries"] += 1
        elif kind == "PARQUET ERROR":
            if fields.get("operation") == "power-close":
                out["gave_up"] += 1
            else:
                out["errors"] += 1
        elif kind == "PARQUET READY":
            out["ready_files"] += 1
            try:
                rows = int(fields.get("rows", ""))
            except ValueError:
                continue
            out["ready_rows"].append(rows)
            if rows < small_rows:
                out["small_files"] += 1
        elif kind == "PARQUET SCAN":
            out["scan_files"] = fields.get("files")
        elif kind == "PARQUET RECOVERY":
            try:
                out["quarantined"] += int(fields.get("quarantined", "0"))
            except ValueError:
                pass
    return out


def parse_logs(texts: list[tuple[str, str]], *, small_rows: int = DEFAULT_SMALL_ROWS) -> list[dict]:
    """(source, text) pairs in time order to counted boots, overlapping polls joined.

    A loop boot comes after the first boot that logged `state=critical` and
    never logged `ok` or `external_power_likely`, so the recharge boot is not
    one.
    """
    chunks = []
    for source, text in texts:
        chunks += split_log(text, source=source)
    boots = [count_boot(boot, i, small_rows) for i, boot in enumerate(merge_boots(chunks))]
    first = next((b["index"] for b in boots if "critical" in b["states"]), None)
    for boot in boots:
        boot["loop"] = (first is not None and boot["index"] > first
                        and not {"ok", "external_power_likely"} & set(boot["states"]))
    return boots


def parse_log(text: str, *, source: str = "log", small_rows: int = DEFAULT_SMALL_ROWS) -> list[dict]:
    """One log to counted boots."""
    return parse_logs([(source, text)], small_rows=small_rows)


def summarize_logs(boots: list[dict]) -> dict:
    return {
        "boots": len(boots),
        "finalize_requests": sum(b["finalize_requests"] for b in boots),
        "closed": sum(b["closed"] for b in boots),
        "retries": sum(b["retries"] for b in boots),
        "gave_up": sum(b["gave_up"] for b in boots),
        "small_files": sum(b["small_files"] for b in boots),
        "loop_small_files": sum(b["small_files"] for b in boots if b["loop"]),
        "boots_with_repeat_close": [b["index"] for b in boots if b["closed"] > 1],
        "boots_with_repeat_finalize": [b["index"] for b in boots if b["finalize_requests"] > 1],
        "boots_with_unclosed_finalize": [b["index"] for b in boots if b["finalize_requests"]
                                         and not (b["closed"] or b["gave_up"] or b["nothing_open"])],
        "per_boot": boots,
    }


def findings(rows: dict | None, logs: dict | None) -> list[str]:
    """Pass criteria from #61 item 1 that the data fails."""
    out = []
    if rows:
        gaps = rows.get("gaps", {})
        if gaps.get("before_critical"):
            out.append(f"{gaps['before_critical']} row gaps longer than {gaps['threshold_s']:g} s before the "
                       f"first critical row, largest {gaps['largest_before_critical_s']:g} s")
        budget = rows["states"].get("critical_to_last_row_s")
        if budget is not None and budget < 2 * RETRY_BUDGET_S:
            out.append(f"critical to last row {budget:g} s is under {2 * RETRY_BUDGET_S} s, twice the "
                       f"{RETRY_BUDGET_S} s retry budget, shrink the budget")
        if rows["loop_small_files"] > 1:
            out.append(f"{rows['loop_small_files']} small Parquet files from reboot loop boots, more than one "
                       f"means a guard follow-up on #28")
        if rows["unreadable"]:
            out.append(f"{len(rows['unreadable'])} files failed to read")
        if rows["absent_in_every_file"]:
            out.append("no file has " + ", ".join(rows["absent_in_every_file"]) + ", is this schema v5?")
    if logs:
        def listed(key: str) -> str:
            return ", ".join(str(i) for i in logs[key])

        if logs["boots_with_repeat_close"]:
            out.append("more than one power-critical close in log boots " + listed("boots_with_repeat_close"))
        if logs["boots_with_repeat_finalize"]:
            out.append("more than one action=finalize in log boots " + listed("boots_with_repeat_finalize")
                       + ", the re-arm rule let a second request through")
        if logs["boots_with_unclosed_finalize"]:
            out.append("action=finalize with no close, give up or nothing-open in log boots "
                       + listed("boots_with_unclosed_finalize"))
        if logs["gave_up"]:
            out.append(f"{logs['gave_up']} power-close give ups (PARQUET ERROR operation=power-close)")
        if logs["loop_small_files"] > 1:
            out.append(f"{logs['loop_small_files']} small files from reboot loop boots in the logs")
    return out


# ----------------------------------------------------------------------------
# Plot, a plain SVG so no plotting dependency is needed

PANELS = (
    ("battery_mv", "battery_mv (mV)"),
    ("battery_soc_raw_percent", "battery_soc_raw_percent (%)"),
    ("battery_charge_rate_percent_per_h", "battery_charge_rate_percent_per_h (%/h)"),
    ("battery_health_state", "battery_health_state"),
)
WIDTH, PANEL_H, LEFT, RIGHT, TOP, GAP = 960, 170, 90, 20, 40, 40


def nice_ticks(low: float, high: float, count: int = 5) -> list[float]:
    if high <= low:
        high = low + 1
    raw = (high - low) / count
    magnitude = 10 ** math.floor(math.log10(raw))
    step = min((m * magnitude for m in (1, 2, 5, 10) if m * magnitude >= raw), default=raw)
    first = math.ceil(low / step) * step
    ticks, value = [], first
    while value <= high + step * 1e-9:
        ticks.append(round(value, 10))
        value += step
    return ticks


def render_svg(series: list[dict], *, gap_s: float, title: str) -> str:
    height = TOP + len(PANELS) * (PANEL_H + GAP) + 20
    plot_w = WIDTH - LEFT - RIGHT
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{height}" '
             f'viewBox="0 0 {WIDTH} {height}" font-family="sans-serif" font-size="11">',
             f'<rect width="{WIDTH}" height="{height}" fill="#ffffff"/>',
             f'<text x="{LEFT}" y="20" font-size="14" fill="#111">{html.escape(title)}</text>']
    if not series:
        parts.append(f'<text x="{LEFT}" y="{TOP + 20}" fill="#a00">no rows with event_time_utc_us</text></svg>')
        return "\n".join(parts) + "\n"
    t0, t1 = series[0]["utc_us"], series[-1]["utc_us"]
    span = max(t1 - t0, 1)

    def x_of(us: int) -> float:
        return LEFT + (us - t0) / span * plot_w

    hours = span / 3.6e9
    critical = next((r["utc_us"] for r in series if r["battery_health_state"] == 4), None)
    for index, (column, label) in enumerate(PANELS):
        top = TOP + index * (PANEL_H + GAP)
        values = [r[column] for r in series if r[column] is not None]
        parts.append(f'<text x="{LEFT}" y="{top - 6}" fill="#111">{html.escape(label)}</text>')
        parts.append(f'<rect x="{LEFT}" y="{top}" width="{plot_w}" height="{PANEL_H}" fill="none" stroke="#999"/>')
        if column == "battery_health_state":
            low, high = -0.5, 5.5
            ticks = [(code, f"{code} {name.replace('external_power_likely', 'ext power')}")
                     for code, name in STATE_NAMES.items()]
        elif values:
            low, high = min(values), max(values)
            pad = (high - low) * 0.05 or 1
            low, high = low - pad, high + pad
            ticks = [(t, f"{t:g}") for t in nice_ticks(low, high)]
        else:
            parts.append(f'<text x="{LEFT + 10}" y="{top + PANEL_H / 2}" fill="#a00">all null or absent</text>')
            continue

        def y_of(value: float, top=top, low=low, high=high) -> float:
            return top + PANEL_H - (value - low) / (high - low) * PANEL_H

        for value, text in ticks:
            if low <= value <= high:
                y = y_of(value)
                parts.append(f'<line x1="{LEFT}" x2="{LEFT + plot_w}" y1="{y:.1f}" y2="{y:.1f}" stroke="#eee"/>')
                parts.append(f'<text x="{LEFT - 4}" y="{y + 4:.1f}" text-anchor="end" fill="#444">'
                             f'{html.escape(text)}</text>')
        if critical is not None:
            x = x_of(critical)
            parts.append(f'<line x1="{x:.1f}" x2="{x:.1f}" y1="{top}" y2="{top + PANEL_H}" stroke="#c00" '
                         f'stroke-dasharray="4 3"/>')
        step = column == "battery_health_state"
        for segment in segments(series, column, gap_s):
            points = []
            for i, row in enumerate(segment):
                x, y = x_of(row["utc_us"]), y_of(row[column])
                if step and i:
                    points.append(f"{x:.1f},{points[-1].split(',')[1]}")
                points.append(f"{x:.1f},{y:.1f}")
            if len(points) == 1:
                cx, cy = points[0].split(",")
                parts.append(f'<circle cx="{cx}" cy="{cy}" r="1.5" fill="#1f5fa8"/>')
            else:
                parts.append(f'<polyline fill="none" stroke="#1f5fa8" stroke-width="1.2" '
                             f'points="{" ".join(points)}"/>')
    axis_y = TOP + len(PANELS) * (PANEL_H + GAP) - GAP + 14
    for tick in nice_ticks(0, hours):
        if tick > hours:
            continue
        x = LEFT + tick / hours * plot_w if hours else LEFT
        parts.append(f'<text x="{x:.1f}" y="{axis_y}" text-anchor="middle" fill="#444">{tick:g} h</text>')
    parts.append(f'<text x="{LEFT}" y="{axis_y + 16}" fill="#444">hours since {iso(t0)}, red dashes mark '
                 f'the first critical row, breaks are gaps over {gap_s:g} s or nulls</text>')
    parts.append("</svg>")
    return "\n".join(parts) + "\n"


def segments(series: list[dict], column: str, gap_s: float) -> list[list[dict]]:
    out, current, last = [], [], None
    for row in series:
        value = row[column]
        if value is None or (last is not None and (row["utc_us"] - last) / 1e6 > gap_s):
            if current:
                out.append(current)
            current = []
        if value is not None:
            current.append(row)
        last = row["utc_us"]
    if current:
        out.append(current)
    return out


# ----------------------------------------------------------------------------
# Report


def table(rows: dict | None, logs: dict | None, problems: list[str]) -> str:
    lines = []
    if rows:
        lines.append(f"files {rows['files']}  rows {rows['rows']}  placed {rows['placed_rows']}  "
                     f"unplaced {rows['unplaced_rows']}  quarantine skipped {rows['skipped_quarantine']}")
        if rows.get("start"):
            lines.append(f"span {rows['start']} to {rows['end']}, {rows['hours']} h, median step "
                         f"{rows.get('median_step_s', '-')} s")
        for column in BATTERY_COLUMNS[:3]:
            stats = rows.get(column)
            if stats:
                lines.append(f"{column:<34} first {stats['first']:>8g}  last {stats['last']:>8g}  "
                             f"min {stats['min']:>8g}  max {stats['max']:>8g}  nulls {rows['nulls'][column]}")
            else:
                lines.append(f"{column:<34} no values, nulls {rows['nulls'][column]}")
        states = rows["states"]
        first = ", ".join(f"{k} {v}" for k, v in states["first"].items())
        lines.append(f"{'battery_health_state':<34} first seen {first or 'none'}, "
                     f"nulls {rows['nulls']['battery_health_state']}")
        if "critical_to_last_row_s" in states:
            lines.append(f"critical to last row {states['critical_to_last_row_s']:g} s, same boot, up to "
                         f"{states['critical_last_row']} (retry budget about {RETRY_BUDGET_S} s, keep it when "
                         f"this is at least {2 * RETRY_BUDGET_S} s)")
        if rows["absent_columns"]:
            lines.append("absent in some files " + ", ".join(rows["absent_columns"]))
        lines.append("")
        lines.append(f"{'boot (parquet)':<34} {'files':>5} {'rows':>7} {'small':>5} {'no utc':>6} {'loop':>4}")
        for boot in rows["boots"]:
            lines.append(f"{boot['boot']:<34} {boot['files']:>5} {boot['rows']:>7} {boot['small_files']:>5} "
                         f"{boot['unplaced_rows']:>6} {'yes' if boot['loop'] else '':>4}")
    if logs:
        lines.append("")
        lines.append(f"{'boot (log)':<34} {'final':>5} {'close':>5} {'retry':>5} {'gaveup':>6} {'ready':>5} "
                     f"{'small':>5} {'loop':>4}")
        for boot in logs["per_boot"]:
            name = f"#{boot['index']} {(boot['boot'] or '?')[-12:]} {boot['source']}"[:34]
            lines.append(f"{name:<34} {boot['finalize_requests']:>5} {boot['closed']:>5} {boot['retries']:>5} "
                         f"{boot['gave_up']:>6} {boot['ready_files']:>5} {boot['small_files']:>5} "
                         f"{'yes' if boot['loop'] else '':>4}")
        lines.append(f"{'total':<34} {logs['finalize_requests']:>5} {logs['closed']:>5} {logs['retries']:>5} "
                     f"{logs['gave_up']:>6} {'':>5} {logs['small_files']:>5}")
    if problems:
        lines.append("")
        lines += [f"! {p}" for p in problems]
    return "\n".join(lines) + "\n"


def analyze(root: Path | None, logs: list[Path], out: Path, *, small_rows: int, gap_s: float,
            loop_mv: int = DEFAULT_LOOP_MV) -> dict:
    rows_summary = series = None
    if root is not None:
        run = read_run(root)
        rows_summary = summarize_rows(run, small_rows=small_rows, gap_s=gap_s, loop_mv=loop_mv)
        series = placed(run["rows"])
    log_summary = None
    if logs:
        texts = [(str(path), Path(path).read_text(errors="replace")) for path in logs]
        log_summary = summarize_logs(parse_logs(texts, small_rows=small_rows))
    problems = findings(rows_summary, log_summary)
    out.mkdir(parents=True, exist_ok=True)
    result = {"parquet": rows_summary, "logs": log_summary, "findings": problems,
              "small_rows": small_rows, "gap_s": gap_s, "loop_mv": loop_mv}
    if series is not None:
        title = f"discharge run {Path(root).resolve().name}"
        (out / "discharge.svg").write_text(render_svg(series, gap_s=gap_s, title=title))
        result["plot"] = str(out / "discharge.svg")
    (out / "summary.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    result["table"] = table(rows_summary, log_summary, problems)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     epilog="See the module docstring for the full #61 item 1 command list.")
    parser.add_argument("root", type=Path, nargs="?", help="folder with the run's fetched Parquet files")
    parser.add_argument("--log", type=Path, action="append", default=[],
                        help="serial capture or LOG_TAIL text, repeat for several, oldest first")
    parser.add_argument("--out", type=Path, help="report folder (default artifacts/discharge-<UTC stamp>)")
    parser.add_argument("--small-rows", type=int, default=DEFAULT_SMALL_ROWS,
                        help=f"a file with fewer rows is small (default {DEFAULT_SMALL_ROWS})")
    parser.add_argument("--gap-s", type=float, default=DEFAULT_GAP_S,
                        help=f"report row gaps longer than this (default {DEFAULT_GAP_S:g} s)")
    parser.add_argument("--loop-mv", type=int, default=DEFAULT_LOOP_MV,
                        help=f"a boot after Critical that never reads above this is a reboot loop boot "
                             f"(default {DEFAULT_LOOP_MV} mV)")
    args = parser.parse_args(argv)
    if args.root is None and not args.log:
        parser.error("give a Parquet folder, at least one --log, or both")
    if args.root is not None and not args.root.is_dir():
        parser.error(f"{args.root} is not a folder")
    for path in args.log:
        if not path.is_file():
            parser.error(f"{path} is not a file")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out = args.out or DEFAULT_ARTIFACTS / f"discharge-{stamp}"
    result = analyze(args.root, args.log, out, small_rows=args.small_rows, gap_s=args.gap_s,
                     loop_mv=args.loop_mv)
    sys.stdout.write(result["table"])
    print(f"\nwrote {out / 'summary.json'}" + (f" and {result['plot']}" if "plot" in result else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
