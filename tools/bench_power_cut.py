#!/usr/bin/env python3
"""Power cut bench harness for #35 (issue #61 item 2).

Each cycle opens the console port once, waits for at least three `PARQUET ROW`
lines, sends `parquet flush` and cues a cut at a delay aimed at one stage of
the file's life.

- write. The flush was sent and `PARQUET GROUP` has not printed, so the row
  group write or its fsync is running.
- footer-rename. `PARQUET GROUP` printed and `PARQUET READY` has not, so the
  footer write, the fsync or the FAT rename is running.
- idle. No flush is sent and the cut lands between rows, as a control.

A cut that lands later than aimed is counted as after-ready, and one that
lands before the flush command went out is counted as before-flush.

The stage a cut really hit is read back from the lines seen before the port
dropped, so a slow hand or a fast card only moves a cut to another bucket.
Delays are learned from the measured flush to GROUP and flush to READY
latencies of earlier cycles. After the cut the harness waits for the console
to come back, reopens it straight away and records the boot evidence
(`PARQUET RECOVERY`, `QUARANTINE`, `PARTIAL`, `SCAN`, crosslink suspects).

Every cycle log is written line by line to `--out` (default the ignored
`firmware/esp-idf-waveshare-sim7670g/artifacts/powercut-<UTC date>/`), and
`summary.json` is rewritten after every cycle with the per boot loss account.

Subcommands

    run        drive the cycles against a port (owner bench only)
    analyze    rebuild the summary from saved cycle logs
    manifest   SHA-256 and reader check of every file under a directory
    compare    check that every baseline file kept its SHA-256

Examples, for the owner

    pixi run bench-power-cut manifest <fetched-dir> --out baseline.json
    pixi run bench-power-cut run --port /dev/cu.wchusbserial5B901533371 \
        --cutter beep --reaction-ms 400 --cycles 20
    pixi run bench-power-cut run --port <console> --cutter uhubctl \
        --hub <location> --hub-port <n> --cycles 20
    pixi run bench-power-cut analyze <out-dir>/cycle-*.log
    pixi run bench-power-cut compare baseline.json <fetched-dir>

Opening the Waveshare CH343 console resets the board, so the harness opens it
once per cycle, as soon as the device node is back after a cut. The tests use
a fake port, a fake clock and a fake cutter, and never open a real port.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass, field
import datetime
import hashlib
import json
import os
from pathlib import Path
import random
import re
import statistics
import subprocess
import sys
import time
from typing import Callable, Iterable

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ARTIFACTS = ROOT / "firmware/esp-idf-waveshare-sim7670g/artifacts"
PHASES = ("before-flush", "write", "footer-rename", "after-ready", "idle")
TARGETS = ("write", "footer-rename", "idle")

LINE = re.compile(r"PARQUET (BEGIN|ROW|GROUP|READY|QUARANTINE|RECOVERY|PARTIAL|SCAN|FLUSH|ERROR)\b(.*)")
PAIR = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(\S*)")
NUMERIC = {"sequence", "rows", "first", "last", "ordinal", "bytes", "quarantined", "retained",
           "quarantine_files", "quarantine_bytes", "files", "retained_partials", "buffered", "dropped",
           "row_groups", "columns", "monotonic_us"}


# ----------------------------------------------------------------------------
# Log parsing


def parse_line(line: str) -> dict | None:
    """Turn one console line into an event, or None when it is not a Parquet event."""
    match = LINE.search(line)
    if not match:
        return None
    kind, rest = match.group(1), match.group(2)
    if kind == "RECOVERY" and "crosslink-suspect" in rest:
        kind = "CROSSLINK"
    event: dict = {"kind": kind}
    for key, value in PAIR.findall(rest):
        if key in NUMERIC:
            try:
                event[key] = int(value)
                continue
            except ValueError:
                pass
        event[key] = value
    return event


@dataclass
class Boot:
    boot: str
    rows_seen: set[int] = field(default_factory=set)
    groups: dict[str, list[tuple[int, int]]] = field(default_factory=dict)  # stem -> [(first, last)]
    ready: dict[str, tuple[int, int]] = field(default_factory=dict)  # name -> (first, last)
    quarantined: list[dict] = field(default_factory=list)
    recovery: list[dict] = field(default_factory=list)
    crosslinks: list[str] = field(default_factory=list)
    partial: list[dict] = field(default_factory=list)
    scan: list[dict] = field(default_factory=list)
    errors: list[dict] = field(default_factory=list)
    dropped_max: int = 0


def ready_stem(name: str) -> str:
    """`<stem>-<last>-<attempt>.parquet` back to the GROUP `file=` stem."""
    base = name[: -len(".parquet")] if name.endswith(".parquet") else name
    return base.rsplit("-", 2)[0]


def partial_stem(source: str) -> str:
    """`<stem>-<attempt>.partial` back to the stem."""
    base = source[: -len(".partial")] if source.endswith(".partial") else source
    return base.rsplit("-", 1)[0]


def collect(events: Iterable[dict]) -> list[Boot]:
    # Lines are attributed to the boot of the last BEGIN. The logger starts
    # the storage worker before it prints BEGIN, so on a fast mount the
    # RECOVERY and QUARANTINE lines of a new boot can land under the previous
    # one. Totals across boots are unaffected.
    boots: list[Boot] = []
    current: Boot | None = None
    for event in events:
        kind = event["kind"]
        if kind == "BEGIN" or current is None:
            current = Boot(str(event.get("boot", "unknown")) if kind == "BEGIN" else "unknown")
            boots.append(current)
            if kind == "BEGIN":
                continue
        if kind == "ROW" and isinstance(event.get("sequence"), int):
            current.rows_seen.add(event["sequence"])
            if isinstance(event.get("dropped"), int):
                current.dropped_max = max(current.dropped_max, event["dropped"])
        elif kind == "GROUP" and isinstance(event.get("first"), int):
            current.groups.setdefault(event.get("file", "?"), []).append((event["first"], event["last"]))
        elif kind == "READY" and isinstance(event.get("first"), int):
            current.ready[event.get("name", "?")] = (event["first"], event["last"])
        elif kind == "QUARANTINE":
            current.quarantined.append(event)
        elif kind == "RECOVERY":
            current.recovery.append(event)
        elif kind == "CROSSLINK":
            current.crosslinks.append(event.get("file", "?"))
        elif kind == "PARTIAL":
            current.partial.append(event)
        elif kind == "SCAN":
            current.scan.append(event)
        elif kind in ("ERROR", "FLUSH"):
            current.errors.append(event)
    return boots


def span(ranges: Iterable[tuple[int, int]]) -> set[int]:
    rows: set[int] = set()
    for first, last in ranges:
        rows.update(range(first, last + 1))
    return rows


def account(events: Iterable[dict]) -> dict:
    """Per boot loss account. Sequence numbers restart with every boot."""
    boots = collect(events)
    report = []
    all_ready: dict[str, str] = {}
    duplicates = []
    for boot in boots:
        for name in boot.ready:
            if name in all_ready:
                duplicates.append(name)
            all_ready[name] = boot.boot
    ready_stems = {ready_stem(name) for name in all_ready}
    for boot in boots:
        finalized_stems = {ready_stem(name) for name in boot.ready}
        finalized = span(boot.ready.values())
        grouped = span(r for ranges in boot.groups.values() for r in ranges)
        open_stems = sorted(stem for stem in boot.groups if stem not in finalized_stems)
        in_partial = span(r for stem in open_stems for r in boot.groups[stem])
        ram_lost = boot.rows_seen - grouped - finalized
        seen = boot.rows_seen | grouped | finalized
        log_gaps = set(range(min(seen), max(seen) + 1)) - seen if seen else set()
        recovered = sum(e.get("quarantined", 0) for e in boot.recovery if isinstance(e.get("quarantined"), int))
        report.append({
            "boot": boot.boot,
            "rows_seen": len(boot.rows_seen),
            "first": min(seen) if seen else None,
            "last": max(seen) if seen else None,
            "finalized_rows": len(finalized),
            "finalized_files": sorted(boot.ready),
            "in_partial_rows": len(in_partial),
            "open_files_at_end": open_stems,
            "ram_lost_rows": len(ram_lost),
            "ram_lost_ranges": ranges_of(ram_lost),
            "log_gap_rows": len(log_gaps),
            "dropped_max": boot.dropped_max,
            "recovery_quarantined": recovered,
            "quarantined": [e.get("source") for e in boot.quarantined],
            "quarantined_after_ready": [e.get("source") for e in boot.quarantined
                                        if partial_stem(str(e.get("source", ""))) in ready_stems],
            "crosslink_suspects": boot.crosslinks,
            "partial": boot.partial[-1] if boot.partial else None,
            "scan": boot.scan[-1] if boot.scan else None,
            "errors": boot.errors,
        })
    totals = {
        "boots": len(report),
        "finalized_rows": sum(b["finalized_rows"] for b in report),
        "in_partial_rows": sum(b["in_partial_rows"] for b in report),
        "ram_lost_rows": sum(b["ram_lost_rows"] for b in report),
        "log_gap_rows": sum(b["log_gap_rows"] for b in report),
        "recovery_quarantined": sum(b["recovery_quarantined"] for b in report),
        "quarantined_after_ready": sum(len(b["quarantined_after_ready"]) for b in report),
        "crosslink_suspects": sum(len(b["crosslink_suspects"]) for b in report),
        "duplicate_ready_names": sorted(set(duplicates)),
    }
    return {"boots": report, "totals": totals}


def ranges_of(values: set[int]) -> list[list[int]]:
    out: list[list[int]] = []
    for value in sorted(values):
        if out and value == out[-1][1] + 1:
            out[-1][1] = value
        else:
            out.append([value, value])
    return out


def classify(lines: list[tuple[float, str]], flush_at: float | None, cut_at: float) -> dict:
    """The stage a cut hit, from the lines the console printed before it dropped."""
    group_at = ready_at = None
    last = None
    rows = 0
    for when, line in lines:
        if when > cut_at:
            break
        event = parse_line(line)
        if event is None:
            continue
        last = line
        if event["kind"] == "ROW":
            rows += 1
        if flush_at is None or when < flush_at:
            continue
        if event["kind"] == "GROUP" and group_at is None:
            group_at = when
        elif event["kind"] == "READY" and ready_at is None:
            ready_at = when
    if flush_at is None:
        phase = "idle"
    elif ready_at is not None:
        phase = "after-ready"
    elif group_at is not None:
        phase = "footer-rename"
    else:
        phase = "write"
    return {
        "phase": phase,
        "group_latency_s": None if group_at is None else round(group_at - flush_at, 4),
        "ready_latency_s": None if ready_at is None else round(ready_at - flush_at, 4),
        "rows_in_cycle": rows,
        "last_event_line": last,
    }


# ----------------------------------------------------------------------------
# Scheduling


def plan_targets(cycles: int, seed: int, mix: tuple[int, int, int] = (5, 10, 5)) -> list[str]:
    """The issue mix, 5 write, 10 footer-rename and 5 idle per 20, shuffled."""
    total = sum(mix)
    targets: list[str] = []
    for target, weight in zip(TARGETS, mix):
        targets += [target] * round(cycles * weight / total)
    while len(targets) < cycles:
        targets.append("footer-rename")
    targets = targets[:cycles]
    random.Random(seed).shuffle(targets)
    return targets


@dataclass
class Latency:
    group_s: float = 0.3
    ready_s: float = 0.9
    group_seen: list[float] = field(default_factory=list)
    ready_seen: list[float] = field(default_factory=list)

    def learn(self, group: float | None, ready: float | None) -> None:
        if group is not None:
            self.group_seen.append(group)
            self.group_s = statistics.median(self.group_seen)
        if ready is not None:
            self.ready_seen.append(ready)
            self.ready_s = statistics.median(self.ready_seen)
        if self.ready_s <= self.group_s:
            self.ready_s = self.group_s + 0.05

    def cut_delay(self, target: str, rng: random.Random) -> float:
        """Seconds after the flush at which the power should drop."""
        if target == "write":
            return rng.uniform(0.0, self.group_s * 0.9)
        if target == "footer-rename":
            return rng.uniform(self.group_s, self.ready_s)
        raise ValueError(target)


# ----------------------------------------------------------------------------
# Cutters. A cutter only cues or switches power, it never touches the port.


class BeepCutter:
    """Hand cut. Rings the terminal bell and prints a cue, the owner pulls USB."""

    name = "beep"

    def __init__(self, stream=None, command: list[str] | None = None, runner=subprocess.run):
        self.stream = stream or sys.stderr
        self.command = command
        self.runner = runner

    def cue(self, cycle: int) -> None:
        self.stream.write(f"\a>>> CUT NOW, cycle {cycle}. Pull USB, wait 5 s, plug back in.\n")
        self.stream.flush()
        if self.command:
            self.runner(self.command, check=False)

    def restore(self, cycle: int) -> None:
        pass


class UhubctlCutter:
    """Switches VBUS on one hub port with uhubctl, then powers it back on."""

    name = "uhubctl"

    def __init__(self, hub: str, port: str, off_s: float, clock, runner=subprocess.run):
        self.hub, self.port, self.off_s = hub, port, off_s
        self.clock, self.runner = clock, runner

    def _switch(self, action: str) -> None:
        self.runner(["uhubctl", "-l", self.hub, "-p", self.port, "-a", action], check=True,
                    capture_output=True)

    def cue(self, cycle: int) -> None:
        self._switch("off")

    def restore(self, cycle: int) -> None:
        self.clock.sleep(self.off_s)
        self._switch("on")


class Clock:
    def now(self) -> float:
        return time.monotonic()

    def sleep(self, seconds: float) -> None:
        time.sleep(max(0.0, seconds))


# ----------------------------------------------------------------------------
# The cycle runner


class Disconnected(Exception):
    pass


class LineReader:
    """Non blocking line splitter over a port whose read() times out quickly."""

    def __init__(self, port, clock, sink: Callable[[float, str], None]):
        self.port, self.clock, self.sink = port, clock, sink
        self.pending = bytearray()

    def poll(self) -> list[tuple[float, str]]:
        try:
            chunk = self.port.read(4096)
        except (OSError, IOError) as error:  # serial.SerialException is an OSError
            raise Disconnected(str(error)) from None
        if not chunk:
            return []
        self.pending.extend(chunk)
        out = []
        while b"\n" in self.pending:
            raw, _, tail = self.pending.partition(b"\n")
            self.pending = bytearray(tail)
            line = raw.rstrip(b"\r").decode("utf-8", "replace")
            when = self.clock.now()
            self.sink(when, line)
            out.append((when, line))
        if len(self.pending) > 1 << 20:
            raise ValueError("unterminated serial line over 1 MiB")
        return out


@dataclass
class Settings:
    cycles: int = 20
    seed: int = 35
    reaction_s: float = 0.0
    rows_before_flush: int = 3
    open_timeout_s: float = 120.0
    rows_timeout_s: float = 120.0
    cut_timeout_s: float = 20.0
    idle_window_s: tuple[float, float] = (1.0, 9.0)
    calibrate: int = 2


@dataclass
class CycleRecord:
    index: int
    target: str
    planned_delay_s: float | None = None
    opened_at: float | None = None
    flush_at: float | None = None
    cue_at: float | None = None
    cut_at: float | None = None
    cut_detected: bool = False
    boot: str | None = None
    phase: str | None = None
    group_latency_s: float | None = None
    ready_latency_s: float | None = None
    rows_in_cycle: int = 0
    last_event_line: str | None = None
    reopen_s: float | None = None
    boot_evidence: list[str] = field(default_factory=list)
    log: str | None = None
    note: str | None = None


class Bench:
    def __init__(self, settings: Settings, open_port: Callable[[], object], port_present: Callable[[], bool],
                 cutter, clock, out_dir: Path, log=None):
        self.s = settings
        self.open_port = open_port
        self.port_present = port_present
        self.cutter = cutter
        self.clock = clock
        self.out = Path(out_dir)
        self.rng = random.Random(settings.seed)
        self.latency = Latency()
        self.records: list[CycleRecord] = []
        self.log = log or (lambda message: print(message, file=sys.stderr, flush=True))

    def wait_present(self, deadline: float) -> bool:
        while self.clock.now() < deadline:
            if self.port_present():
                return True
            self.clock.sleep(0.1)
        return False

    def calibrate(self) -> None:
        """Flush without a cut to learn the GROUP and READY latencies."""
        if self.s.calibrate <= 0:
            return
        path = self.out / "calibrate.log"
        with path.open("a") as handle:
            port, reader = self._open(handle)
            try:
                for attempt in range(self.s.calibrate):
                    lines = self._wait_rows(reader, self.s.rows_before_flush)
                    flush_at = self._flush(port, handle)
                    end = flush_at + self.s.cut_timeout_s
                    while self.clock.now() < end:
                        lines += reader.poll()
                        if any(parse_line(l) and parse_line(l)["kind"] == "READY" for w, l in lines if w >= flush_at):
                            break
                        self.clock.sleep(0.01)
                    result = classify(lines, flush_at, self.clock.now())
                    self.latency.learn(result["group_latency_s"], result["ready_latency_s"])
                    self.log(f"calibrate {attempt + 1}: group {result['group_latency_s']} s, "
                             f"ready {result['ready_latency_s']} s")
            finally:
                port.close()

    def _open(self, handle):
        if not self.wait_present(self.clock.now() + self.s.open_timeout_s):
            raise TimeoutError("console port did not come back")
        port = self.open_port()

        def sink(when: float, line: str) -> None:
            handle.write(f"{when:.3f} {line}\n")
            handle.flush()

        return port, LineReader(port, self.clock, sink)

    def _wait_rows(self, reader: LineReader, count: int) -> list[tuple[float, str]]:
        lines: list[tuple[float, str]] = []
        rows = 0
        deadline = self.clock.now() + self.s.rows_timeout_s
        while rows < count:
            if self.clock.now() > deadline:
                raise TimeoutError(f"fewer than {count} PARQUET ROW lines")
            new = reader.poll()
            lines += new
            rows += sum(1 for _, line in new if (e := parse_line(line)) and e["kind"] == "ROW")
            if not new:
                self.clock.sleep(0.01)
        return lines

    def _flush(self, port, handle) -> float:
        # Unlike parquet_device.send_command this keeps the input buffer, so
        # no ROW line is dropped right before the flush.
        payload = b"parquet flush\n"
        try:
            written = port.write(payload)
        except (OSError, IOError) as error:
            raise Disconnected(str(error)) from None
        if written != len(payload):
            raise IOError("short serial command write")
        when = self.clock.now()
        handle.write(f"{when:.3f} >>> parquet flush\n")
        handle.flush()
        return when

    def cycle(self, index: int, target: str) -> CycleRecord:
        record = CycleRecord(index, target)
        path = self.out / f"cycle-{index:02d}.log"
        record.log = str(path)
        with path.open("a") as handle:
            handle.write(f"# cycle {index} target={target}\n")
            port, reader = self._open(handle)
            record.opened_at = self.clock.now()
            if self.records and self.records[-1].cut_at is not None:
                self.records[-1].reopen_s = round(record.opened_at - self.records[-1].cut_at, 3)
            lines: list[tuple[float, str]] = []
            try:
                lines = self._wait_rows(reader, self.s.rows_before_flush)
                # The power should drop `delay` after the flush. A hand needs
                # `reaction_s` from cue to cut, so when the delay is shorter
                # than that the cue goes first and the flush follows it.
                now = self.clock.now()
                if target == "idle":
                    delay = self.rng.uniform(*self.s.idle_window_s)
                    actions = [(now + max(0.0, delay - self.s.reaction_s), "cue")]
                else:
                    delay = self.latency.cut_delay(target, self.rng)
                    lead = delay - self.s.reaction_s
                    actions = [(now, "flush"), (now + lead, "cue")] if lead >= 0 else \
                        [(now, "cue"), (now - lead, "flush")]
                record.planned_delay_s = round(delay, 4)
                for when, action in actions:
                    while self.clock.now() < when:
                        lines += reader.poll()
                        self.clock.sleep(0.001)
                    lines += reader.poll()  # everything printed up to this action
                    if action == "flush":
                        record.flush_at = self._flush(port, handle)
                        continue
                    record.cue_at = self.clock.now()
                    handle.write(f"{record.cue_at:.3f} >>> cut cue delay={delay:.3f}\n")
                    handle.flush()
                    self.cutter.cue(index)
                end = record.cue_at + self.s.cut_timeout_s
                while self.clock.now() < end:
                    lines += reader.poll()
                    self.clock.sleep(0.001)
                record.note = "no cut detected, the port stayed up"
                record.cut_at = None
            except Disconnected as error:
                record.cut_detected = True
                record.cut_at = self.clock.now()
                handle.write(f"{record.cut_at:.3f} >>> port dropped ({error})\n")
            finally:
                try:
                    port.close()
                except (OSError, IOError):
                    pass
            self.cutter.restore(index)
        boots = [parse_line(line) for _, line in lines]
        record.boot = next((str(e.get("boot")) for e in boots if e and e["kind"] == "BEGIN"), None)
        record.boot_evidence = [line for _, line in lines
                                if (e := parse_line(line)) and e["kind"] in
                                ("BEGIN", "RECOVERY", "CROSSLINK", "QUARANTINE", "PARTIAL", "SCAN")]
        result = classify(lines, record.flush_at, record.cut_at if record.cut_at is not None else float("inf"))
        record.phase = result["phase"] if record.cut_detected else None
        if record.cut_detected and target != "idle" and record.flush_at is None:
            record.phase = "before-flush"  # the hand beat the flush command
        record.group_latency_s = result["group_latency_s"]
        record.ready_latency_s = result["ready_latency_s"]
        record.rows_in_cycle = result["rows_in_cycle"]
        record.last_event_line = result["last_event_line"]
        self.latency.learn(record.group_latency_s, record.ready_latency_s)
        return record

    def run(self) -> dict:
        self.out.mkdir(parents=True, exist_ok=True)
        self.calibrate()
        for index, target in enumerate(plan_targets(self.s.cycles, self.s.seed), start=1):
            self.log(f"cycle {index}/{self.s.cycles} target {target}")
            record = self.cycle(index, target)
            self.records.append(record)
            self.log(f"cycle {index} hit {record.phase or 'no cut'} ({record.last_event_line})")
            summary = self.summary()
            (self.out / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
        return self.summary()

    def summary(self) -> dict:
        # The calibration boot comes first, its RAM rows are lost when cycle 1
        # reopens the port, which resets the Waveshare.
        calibration = self.out / "calibrate.log"
        events = events_from_log(calibration) if calibration.exists() else []
        for record in self.records:
            events += events_from_log(Path(record.log))
        phases = {phase: sum(1 for r in self.records if r.phase == phase) for phase in PHASES}
        phases["no-cut"] = sum(1 for r in self.records if not r.cut_detected)
        return {
            "settings": asdict(self.s),
            "cutter": getattr(self.cutter, "name", type(self.cutter).__name__),
            "latency": {"group_s": self.latency.group_s, "ready_s": self.latency.ready_s},
            "phases": phases,
            "cycles": [asdict(r) for r in self.records],
            "loss": account(events),
        }


def events_from_log(path: Path) -> list[dict]:
    events = []
    for raw in Path(path).read_text(errors="replace").splitlines():
        if raw.startswith("#"):
            continue
        _, _, line = raw.partition(" ")
        event = parse_line(line)
        if event is not None:
            events.append(event)
    return events


# ----------------------------------------------------------------------------
# Baselines


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def manifest(root: Path, *, inspect: bool = True) -> dict:
    files = {}
    for path in sorted(p for p in Path(root).rglob("*") if p.is_file()):
        entry = {"bytes": path.stat().st_size, "sha256": sha256(path)}
        if inspect and path.suffix == ".parquet":
            from parquet_device import validate_file

            try:
                check = validate_file(path, allow_nonfinite=True)
                entry.update(readers_match=check["readers_match"], rows=check["rows"])
            except Exception as error:  # record the failure, keep hashing the rest
                entry.update(readers_match=False, error=f"{type(error).__name__}: {error}")
        files[path.relative_to(root).as_posix()] = entry
    return {"root": str(root), "files": files}


def compare(baseline: dict, current: dict) -> dict:
    old, new = baseline["files"], current["files"]
    changed = sorted(n for n in old if n in new and old[n]["sha256"] != new[n]["sha256"])
    missing = sorted(n for n in old if n not in new)
    added = sorted(n for n in new if n not in old)
    unreadable = sorted(n for n, e in new.items() if e.get("readers_match") is False)
    return {"ok": not (changed or missing or unreadable), "changed": changed, "missing": missing,
            "added": added, "unreadable": unreadable, "kept": len(old) - len(changed) - len(missing)}


# ----------------------------------------------------------------------------


def serial_port(name: str, baud: int):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from parquet_device import open_port

    return lambda: open_port(name, baud)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    run = sub.add_parser("run", help="drive cut cycles against the console port (owner bench only)")
    run.add_argument("--port", required=True)
    run.add_argument("--baud", type=int, default=115200)
    run.add_argument("--cycles", type=int, default=20)
    run.add_argument("--seed", type=int, default=35)
    run.add_argument("--cutter", choices=["beep", "uhubctl"], default="beep")
    run.add_argument("--reaction-ms", type=float, default=None,
                     help="cue this much earlier than the target (default 400 for beep, 0 for uhubctl)")
    run.add_argument("--beep-command", nargs="+", help="extra cue command, for example afplay <sound>")
    run.add_argument("--hub", help="uhubctl -l location")
    run.add_argument("--hub-port", help="uhubctl -p port")
    run.add_argument("--off-s", type=float, default=5.0)
    run.add_argument("--calibrate", type=int, default=2, help="flushes without a cut to learn latencies")
    run.add_argument("--out", type=Path)

    analyze = sub.add_parser("analyze", help="loss account from saved cycle logs")
    analyze.add_argument("logs", type=Path, nargs="+")

    man = sub.add_parser("manifest", help="SHA-256 and reader check of every file under a directory")
    man.add_argument("root", type=Path)
    man.add_argument("--out", type=Path, required=True)
    man.add_argument("--no-inspect", action="store_true")

    cmp_ = sub.add_parser("compare", help="baseline manifest against a directory")
    cmp_.add_argument("baseline", type=Path)
    cmp_.add_argument("root", type=Path)

    args = parser.parse_args(argv)
    if args.command == "analyze":
        events = []
        for path in args.logs:
            events += events_from_log(path)
        print(json.dumps(account(events), indent=2, sort_keys=True))
        return 0
    if args.command == "manifest":
        args.out.write_text(json.dumps(manifest(args.root, inspect=not args.no_inspect), indent=2,
                                       sort_keys=True) + "\n")
        return 0
    if args.command == "compare":
        result = compare(json.loads(args.baseline.read_text()), manifest(args.root))
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0 if result["ok"] else 1

    clock = Clock()
    if args.cutter == "uhubctl":
        if not (args.hub and args.hub_port):
            parser.error("--cutter uhubctl needs --hub and --hub-port")
        cutter = UhubctlCutter(args.hub, args.hub_port, args.off_s, clock)
        reaction = 0.0 if args.reaction_ms is None else args.reaction_ms / 1000
    else:
        cutter = BeepCutter(command=args.beep_command)
        reaction = 0.4 if args.reaction_ms is None else args.reaction_ms / 1000
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out = args.out or DEFAULT_ARTIFACTS / f"powercut-{stamp}"
    settings = Settings(cycles=args.cycles, seed=args.seed, reaction_s=reaction, calibrate=args.calibrate)
    bench = Bench(settings, serial_port(args.port, args.baud), lambda: os.path.exists(args.port), cutter, clock, out)
    summary = bench.run()
    print(json.dumps({"out": str(out), "phases": summary["phases"], "totals": summary["loss"]["totals"]},
                     indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
