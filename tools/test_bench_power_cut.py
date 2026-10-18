"""Host test for tools/bench_power_cut.py with a simulated board.

Run with ``pixi run bench-power-cut-test``.

A fake clock drives a fake board that prints the firmware `PARQUET` lines with
their real formats, answers `parquet flush` with `GROUP` and `READY` after
fixed latencies, resets on every port open like the Waveshare CH343, and loses
power when the fake cutter says so. The board keeps the ground truth of every
row (finalized, left in a partial, lost from RAM) and of the stage each cut
hit, and the test checks the harness classification and loss account against
it. No serial port, hub or board is touched.
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
import bench_power_cut as bpc  # noqa: E402


def require(condition: bool, description: str) -> None:
    if not condition:
        raise AssertionError(description)


class FakeClock:
    def __init__(self) -> None:
        self.t = 1000.0

    def now(self) -> float:
        return self.t

    def sleep(self, seconds: float) -> None:
        self.t += max(0.0, seconds)


class FakeBoard:
    """A Waveshare logger reduced to its console lines and card state."""

    def __init__(self, clock: FakeClock, *, row_s=10.0, group_s=0.25, ready_s=0.8, off_s=5.0):
        self.clock = clock
        self.row_s, self.group_s, self.ready_s, self.off_s = row_s, group_s, ready_s, off_s
        self.powered = self.present = True
        self.running = False
        self.boot = None
        self.boots = 0
        self.outputs: list[tuple[float, str]] = []
        self.buffer: list[int] = []
        self.pending: dict | None = None
        self.flushed = False
        self.partials: list[str] = []
        self.quarantine_files = 0
        self.finalized_files = 0
        self.cut_at: float | None = None
        self.restore_at: float | None = None
        self.truth: dict[str, dict[str, set[int]]] = {}
        self.cut_phases: list[str] = []
        self.opens = 0

    # Timeline -------------------------------------------------------------

    def emit(self, when: float, line: str) -> None:
        self.outputs.append((when, line))

    def mine(self) -> dict[str, set[int]]:
        return self.truth.setdefault(self.boot, {"final": set(), "partial": set(), "lost": set(), "seen": set()})

    def advance(self) -> None:
        now = self.clock.now()
        while True:
            times = []
            if self.running:
                times.append((self.next_row, "row"))
                if self.pending and not self.pending["grouped"]:
                    times.append((self.pending["group_t"], "group"))
                if self.pending:
                    times.append((self.pending["ready_t"], "ready"))
            if self.cut_at is not None:
                times.append((self.cut_at, "cut"))
            if not self.powered and self.restore_at is not None:
                times.append((self.restore_at, "restore"))
            due = sorted(t for t in times if t[0] <= now)
            if not due:
                return
            when, kind = due[0]
            getattr(self, f"_{kind}")(when)

    def _row(self, when: float) -> None:
        sequence = self.sequence
        self.sequence += 1
        self.buffer.append(sequence)
        self.mine()["seen"].add(sequence)
        self.emit(when, f"I (1) aq: PARQUET ROW sequence={sequence} monotonic_us={int(when * 1e6)} "
                        f"jitter_us=12 buffered={len(self.buffer)} dropped=0")
        self.next_row = when + self.row_s

    def _group(self, when: float) -> None:
        p = self.pending
        p["grouped"] = True
        self.partials.append(p["stem"])
        self.emit(when, f"PARQUET GROUP file={p['stem']} ordinal=0 rows={len(p['rows'])} first={p['rows'][0]} "
                        f"last={p['rows'][-1]} bytes=9000 writer_us=1200 codec=lz4 codec_us=300")

    def _ready(self, when: float) -> None:
        p = self.pending
        self.pending = None
        self.partials.remove(p["stem"])
        self.finalized_files += 1
        self.mine()["final"].update(p["rows"])
        self.emit(when, f"PARQUET READY name={p['stem']}-{p['rows'][-1]}-1.parquet rows={len(p['rows'])} "
                        f"row_groups=1 first={p['rows'][0]} last={p['rows'][-1]} bytes=11000 crc32=0badf00d "
                        f"write_us=1 sync_us=2")

    def _cut(self, when: float) -> None:
        self.cut_at = None
        if not self.powered:
            return
        p = self.pending
        if p is None:
            phase = "after-ready" if self.flushed else "idle"
        elif not p["grouped"]:
            phase = "write"
            self.mine()["lost"].update(p["rows"])
        else:
            phase = "footer-rename"
            self.mine()["partial"].update(p["rows"])
        self.cut_phases.append(phase)
        self.mine()["lost"].update(self.buffer)
        self.buffer, self.pending = [], None
        self.powered = self.present = self.running = False
        self.restore_at = when + self.off_s

    def _restore(self, when: float) -> None:
        # The board powers up, but the harness reopens within 0.1 s and that
        # open resets it before the logger reaches its SD recovery.
        self.powered = self.present = True
        self.restore_at = None

    def reset(self) -> None:
        if self.boot is not None and self.running:
            self.mine()["lost"].update(self.buffer)
            if self.pending is not None:
                key = "partial" if self.pending["grouped"] else "lost"
                self.mine()[key].update(self.pending["rows"])
        self.boots += 1
        self.boot = f"b{self.boots:04x}"
        self.mine()
        self.sequence, self.buffer, self.pending, self.flushed = 0, [], None, False
        now = self.clock.now()
        found = list(self.partials)
        self.partials.clear()
        self.quarantine_files += len(found)
        self.emit(now + 0.2, f"PARQUET BEGIN schema=v5 columns=62 sample_s=10 interval_s=900 max_rows=90 "
                             f"max_row_groups=8 psram_workspace_bytes=1 row_bytes=1 boot={self.boot} "
                             f"station=st codec=lz4 crc32_check=on")
        for stem in found:
            self.emit(now + 0.3, f"PARQUET QUARANTINE source={stem}-1.partial target=quarantine/{stem}-1.partial "
                                 f"bytes=4096 origin=boot")
        self.emit(now + 0.31, f"PARQUET RECOVERY quarantined={len(found)} repaired=0 deleted=0")
        self.emit(now + 0.32, f"PARQUET PARTIAL retained=0 quarantine_files={self.quarantine_files} "
                              f"quarantine_bytes={4096 * self.quarantine_files} recovery=host-only")
        self.emit(now + 0.33, f"PARQUET SCAN files={self.finalized_files} retained_partials=0 "
                              f"quarantine_files={self.quarantine_files} quarantine_bytes=0")
        self.running = True
        self.next_row = now + 1.0

    def flush(self) -> None:
        now = self.clock.now()
        if not self.buffer or self.pending:
            self.emit(now + 0.01, "PARQUET ERROR operation=flush reason=empty")
            return
        rows, self.buffer = self.buffer, []
        self.flushed = True
        self.pending = {"rows": rows, "grouped": False, "group_t": now + self.group_s,
                        "ready_t": now + self.ready_s, "stem": f"Y=2026/M=10/aq_{self.boot}_{rows[0]}"}

    # Port surface ---------------------------------------------------------

    def is_present(self) -> bool:
        self.advance()
        return self.present

    def open(self) -> "FakePort":
        self.advance()
        if not self.present:
            raise OSError("no such device")
        self.opens += 1
        self.outputs.clear()  # nothing printed while the port was closed is seen
        self.reset()  # opening the CH343 console resets the Waveshare
        return FakePort(self)


class FakePort:
    def __init__(self, board: FakeBoard) -> None:
        self.board = board
        self.closed = False
        self.written: list[bytes] = []

    def read(self, size: int) -> bytes:
        board = self.board
        board.advance()
        if self.closed or not board.powered:
            raise OSError("device disconnected")
        now = board.clock.now()
        due = sorted((o for o in board.outputs if o[0] <= now), key=lambda o: o[0])
        board.outputs = [o for o in board.outputs if o[0] > now]
        return "".join(line + "\r\n" for _, line in due).encode()

    def write(self, data: bytes) -> int:
        self.board.advance()
        if not self.board.powered:
            raise OSError("device disconnected")
        self.written.append(data)
        if data == b"parquet flush\n":
            self.board.flush()
        return len(data)

    def close(self) -> None:
        self.closed = True


class FakeCutter:
    name = "fake"

    def __init__(self, board: FakeBoard, clock: FakeClock, lag: float = 0.0, act: bool = True) -> None:
        self.board, self.clock, self.lag, self.act = board, clock, lag, act
        self.cues: list[int] = []
        self.restores: list[int] = []

    def cue(self, cycle: int) -> None:
        self.cues.append(cycle)
        if self.act:
            self.board.cut_at = self.clock.now() + self.lag

    def restore(self, cycle: int) -> None:
        self.restores.append(cycle)


def make_bench(out: Path, *, cycles=20, lag=0.0, reaction=0.0, act=True, seed=35):
    clock = FakeClock()
    board = FakeBoard(clock)
    cutter = FakeCutter(board, clock, lag=lag, act=act)
    settings = bpc.Settings(cycles=cycles, seed=seed, reaction_s=reaction)
    messages: list[str] = []
    bench = bpc.Bench(settings, board.open, board.is_present, cutter, clock, out, log=messages.append)
    return bench, board, cutter, messages


# ----------------------------------------------------------------------------


def test_parse_line() -> None:
    begin = bpc.parse_line("I (812) aq: PARQUET BEGIN schema=v5 columns=62 sample_s=10 interval_s=900 max_rows=90 "
                           "max_row_groups=8 psram_workspace_bytes=1 row_bytes=2 boot=abc123 station=s1 "
                           "codec=lz4 crc32_check=on")
    require(begin["kind"] == "BEGIN" and begin["boot"] == "abc123" and begin["columns"] == 62, f"BEGIN {begin}")
    row = bpc.parse_line("PARQUET ROW sequence=17 monotonic_us=99 jitter_us=-3 buffered=4 dropped=1")
    require(row == {"kind": "ROW", "sequence": 17, "monotonic_us": 99, "jitter_us": "-3", "buffered": 4,
                    "dropped": 1}, f"ROW {row}")
    group = bpc.parse_line("PARQUET GROUP file=Y=2026/M=10/aq_b_0 ordinal=0 rows=90 first=0 last=89 bytes=9 "
                           "writer_us=1 codec=lz4 codec_us=2")
    require(group["file"] == "Y=2026/M=10/aq_b_0" and group["first"] == 0 and group["last"] == 89, f"GROUP {group}")
    ready = bpc.parse_line("PARQUET READY name=Y=2026/M=10/aq_b_0-89-1.parquet rows=90 row_groups=1 first=0 last=89")
    require(bpc.ready_stem(ready["name"]) == "Y=2026/M=10/aq_b_0", "READY stem")
    require(bpc.partial_stem("Y=2026/M=10/aq_b_0-3.partial") == "Y=2026/M=10/aq_b_0", "partial stem")
    cross = bpc.parse_line("PARQUET RECOVERY crosslink-suspect file=Y=1/x-1.partial action=none")
    require(cross["kind"] == "CROSSLINK" and cross["file"] == "Y=1/x-1.partial", f"crosslink {cross}")
    remount = bpc.parse_line("PARQUET RECOVERY quarantined=2 repaired=0 deleted=0 origin=remount")
    require(remount["kind"] == "RECOVERY" and remount["quarantined"] == 2, f"recovery {remount}")
    require(bpc.parse_line("PARQUET DATA offset=0 hex=00") is None, "DATA is not an event")
    require(bpc.parse_line("POWER STATE state=ok") is None, "other lines ignored")


def test_account() -> None:
    lines = [
        "PARQUET BEGIN boot=A columns=62",
        *[f"PARQUET ROW sequence={i} buffered={i % 5 + 1} dropped=0" for i in range(14)],
        "PARQUET GROUP file=P/aq_A_0 ordinal=0 rows=5 first=0 last=4",
        "PARQUET READY name=P/aq_A_0-4-1.parquet rows=5 row_groups=1 first=0 last=4",
        "PARQUET GROUP file=P/aq_A_5 ordinal=0 rows=5 first=5 last=9",
        "PARQUET BEGIN boot=B columns=62",
        "PARQUET QUARANTINE source=P/aq_A_5-1.partial target=q/aq_A_5-1.partial bytes=10 origin=boot",
        "PARQUET QUARANTINE source=P/aq_A_0-1.partial target=q/aq_A_0-1.partial bytes=10 origin=boot",
        "PARQUET RECOVERY quarantined=2 repaired=0 deleted=0",
        "PARQUET RECOVERY crosslink-suspect file=P/aq_A_0-1.partial action=none",
        "PARQUET SCAN files=1 retained_partials=0 quarantine_files=2 quarantine_bytes=20",
        "PARQUET ROW sequence=0 buffered=1 dropped=0",
        "PARQUET ROW sequence=2 buffered=2 dropped=3",
        "PARQUET GROUP file=P/aq_B_0 ordinal=0 rows=3 first=0 last=2",
        "PARQUET READY name=P/aq_B_0-2-1.parquet rows=3 row_groups=1 first=0 last=2",
        "PARQUET BEGIN boot=C columns=62",
        "PARQUET READY name=P/aq_A_0-4-1.parquet rows=5 row_groups=1 first=0 last=4",
    ]
    report = bpc.account(bpc.parse_line(line) for line in lines)
    a, b, c = report["boots"]
    require(a["boot"] == "A" and a["finalized_rows"] == 5 and a["in_partial_rows"] == 5, f"boot A {a}")
    require(a["ram_lost_rows"] == 4 and a["ram_lost_ranges"] == [[10, 13]], f"boot A RAM {a}")
    require(a["open_files_at_end"] == ["P/aq_A_5"], "boot A open file")
    require(b["recovery_quarantined"] == 2 and b["crosslink_suspects"] == ["P/aq_A_0-1.partial"], f"boot B {b}")
    require(b["quarantined_after_ready"] == ["P/aq_A_0-1.partial"], "finalized stem quarantined is flagged")
    require(b["log_gap_rows"] == 0 and b["finalized_rows"] == 3 and b["dropped_max"] == 3, f"boot B rows {b}")
    totals = report["totals"]
    require(totals["duplicate_ready_names"] == ["P/aq_A_0-4-1.parquet"], f"duplicates {totals}")
    require(totals["ram_lost_rows"] == 4 and totals["in_partial_rows"] == 5, f"totals {totals}")
    require(c["boot"] == "C" and c["finalized_rows"] == 5 and totals["boots"] == 3, f"boot C {c}")
    gap = bpc.account(bpc.parse_line(l) for l in ["PARQUET ROW sequence=1", "PARQUET ROW sequence=4"])
    require(gap["boots"][0]["boot"] == "unknown" and gap["boots"][0]["log_gap_rows"] == 2, "gaps before BEGIN")


def test_classify_and_plan() -> None:
    lines = [(1.0, "PARQUET ROW sequence=0"), (2.0, "PARQUET GROUP file=x first=0 last=0"),
             (3.0, "PARQUET READY name=x-0-1.parquet first=0 last=0")]
    require(bpc.classify(lines, None, 5.0)["phase"] == "idle", "idle")
    require(bpc.classify(lines, 1.5, 1.9)["phase"] == "write", "write")
    hit = bpc.classify(lines, 1.5, 2.5)
    require(hit["phase"] == "footer-rename" and hit["group_latency_s"] == 0.5, f"footer {hit}")
    hit = bpc.classify(lines, 1.5, 4.0)
    require(hit["phase"] == "after-ready" and hit["ready_latency_s"] == 1.5, f"after {hit}")
    require(bpc.classify(lines, 2.5, 4.0)["phase"] == "after-ready", "a GROUP before the flush is ignored")
    plan = bpc.plan_targets(20, 7)
    require(sorted(plan) == sorted(["write"] * 5 + ["footer-rename"] * 10 + ["idle"] * 5), f"mix {plan}")
    require(plan == bpc.plan_targets(20, 7) and plan != bpc.plan_targets(20, 8), "seeded shuffle")
    require(len(bpc.plan_targets(3, 1)) == 3, "short runs keep the cycle count")
    latency = bpc.Latency()
    latency.learn(0.2, 0.6)
    latency.learn(0.4, 1.0)
    require(abs(latency.group_s - 0.3) < 1e-9 and abs(latency.ready_s - 0.8) < 1e-9, "median latencies")
    import random
    rng = random.Random(1)
    for _ in range(200):
        require(0 <= latency.cut_delay("write", rng) < 0.3, "write delay before GROUP")
        require(0.3 <= latency.cut_delay("footer-rename", rng) <= 0.8, "footer delay between GROUP and READY")


def test_bench_run(work: Path) -> None:
    out = work / "run"
    bench, board, cutter, messages = make_bench(out)
    summary = bench.run()
    records = summary["cycles"]
    require(len(records) == 20 and cutter.cues == list(range(1, 21)), "twenty cued cycles")
    require(cutter.restores == list(range(1, 21)), "restore after every cycle")
    require(all(r["cut_detected"] for r in records), "every cut detected")
    require([r["phase"] for r in records] == board.cut_phases, "harness phase equals the board truth")
    require([r["phase"] for r in records] == [r["target"] for r in records], "zero lag hits every target")
    require(summary["phases"] == {"before-flush": 0, "write": 5, "footer-rename": 10, "after-ready": 0, "idle": 5,
                                  "no-cut": 0},
            f"phases {summary['phases']}")
    require(abs(summary["latency"]["group_s"] - 0.25) < 0.02 and abs(summary["latency"]["ready_s"] - 0.8) < 0.02,
            f"learned latency {summary['latency']}")
    require(board.opens == 21, "calibration plus one open per cycle")
    for previous, record in zip(records, records[1:]):
        require(previous["reopen_s"] is not None and 5.0 <= previous["reopen_s"] < 5.5, "reopen time recorded")
    # Boot evidence of the cycle after a footer-rename cut shows the quarantine.
    for previous, record in zip(records, records[1:]):
        quarantined = [l for l in record["boot_evidence"] if "PARQUET QUARANTINE" in l]
        require(bool(quarantined) == (previous["phase"] == "footer-rename"), f"quarantine evidence {record['index']}")
    # Loss account equals the board truth, boot by boot.
    by_boot = {b["boot"]: b for b in summary["loss"]["boots"]}
    require(set(by_boot) == set(board.truth), "every boot accounted")
    for boot, truth in board.truth.items():
        got = by_boot[boot]
        require(got["finalized_rows"] == len(truth["final"]), f"{boot} finalized")
        require(got["in_partial_rows"] == len(truth["partial"]), f"{boot} partial {got} {truth}")
        require(got["ram_lost_rows"] == len(truth["lost"] - truth["partial"] - truth["final"]), f"{boot} RAM lost")
        require(got["rows_seen"] == len(truth["seen"]) and got["log_gap_rows"] == 0, f"{boot} seen")
    totals = summary["loss"]["totals"]
    torn = sum(1 for r in records[:-1] if r["phase"] == "footer-rename")
    require(totals["recovery_quarantined"] == torn and totals["in_partial_rows"] > 0, f"totals {totals}")
    require(totals["quarantined_after_ready"] == 0 and not totals["duplicate_ready_names"], "no anomalies")
    # Logs are on disk per cycle, written as lines arrive, and analyze agrees.
    logs = sorted(out.glob("cycle-*.log"))
    require(len(logs) == 20 and (out / "calibrate.log").exists(), "cycle logs")
    require(json.loads((out / "summary.json").read_text())["loss"] == summary["loss"], "summary file")
    text = logs[0].read_text()
    require(">>> parquet flush" in text or "target=idle" in text, "flush marker in the log")
    require(">>> port dropped" in text and ">>> cut cue" in text, "cut markers in the log")
    stdout = io.StringIO()
    with contextlib.redirect_stdout(stdout):
        code = bpc.main(["analyze", str(out / "calibrate.log"), *map(str, logs)])
    require(code == 0 and json.loads(stdout.getvalue()) == summary["loss"], "analyze matches the run")
    require(any("cycle 20/20" in m for m in messages), "progress messages")


def test_reaction_and_missed_cut(work: Path) -> None:
    # A 0.3 s hand, cued 0.3 s early, still lands each cut in its target stage.
    bench, board, _, _ = make_bench(work / "react", cycles=8, lag=0.3, reaction=0.3, seed=3)
    summary = bench.run()
    require([r["phase"] for r in summary["cycles"]] == board.cut_phases, "harness agrees with the board")
    require([r["phase"] for r in summary["cycles"]] == [r["target"] for r in summary["cycles"]], "reaction offset")
    # The same hand with no offset drifts late, and the harness reports where it landed.
    bench, board, _, _ = make_bench(work / "late", cycles=8, lag=0.6, reaction=0.0, seed=3)
    summary = bench.run()
    require([r["phase"] for r in summary["cycles"]] == board.cut_phases, "late cuts classified by the log")
    require(summary["phases"]["after-ready"] > 0, f"late cuts land after READY {summary['phases']}")
    # A hand faster than the configured reaction can beat the flush command.
    bench, board, _, _ = make_bench(work / "early", cycles=8, lag=0.0, reaction=0.3, seed=3)
    summary = bench.run()
    early = [r for r in summary["cycles"] if r["phase"] == "before-flush"]
    require(early and all(r["flush_at"] is None and r["target"] != "idle" for r in early), "before-flush cuts")
    require(summary["phases"]["before-flush"] == len(early), "before-flush counted")
    # A missed cue keeps the port up, is reported, and the next cycle carries on.
    bench, board, cutter, _ = make_bench(work / "missed", cycles=2, act=False)
    summary = bench.run()
    require(summary["phases"]["no-cut"] == 2 and all(r["note"] for r in summary["cycles"]), "missed cut noted")
    require(all(r["phase"] is None for r in summary["cycles"]), "no phase without a cut")


def test_cutters() -> None:
    calls: list[list[str]] = []
    clock = FakeClock()
    hub = bpc.UhubctlCutter("1-1", "3", 5.0, clock, runner=lambda cmd, **kw: calls.append(cmd))
    hub.cue(1)
    start = clock.now()
    hub.restore(1)
    require(calls == [["uhubctl", "-l", "1-1", "-p", "3", "-a", "off"], ["uhubctl", "-l", "1-1", "-p", "3", "-a", "on"]],
            f"uhubctl calls {calls}")
    require(clock.now() - start == 5.0, "power stays off for off_s")
    stream = io.StringIO()
    beeps: list[list[str]] = []
    beep = bpc.BeepCutter(stream, ["afplay", "cue.aiff"], runner=lambda cmd, **kw: beeps.append(cmd))
    beep.cue(4)
    require(stream.getvalue().startswith("\a>>> CUT NOW, cycle 4") and beeps == [["afplay", "cue.aiff"]], "beep cue")


def test_manifest_compare(work: Path) -> None:
    root = work / "card"
    (root / "Y=2026").mkdir(parents=True)
    pq.write_table(pa.table({"sequence": [1, 2, 3]}), root / "Y=2026/a.parquet")
    (root / "notes.txt").write_text("hello")
    baseline = bpc.manifest(root)
    entry = baseline["files"]["Y=2026/a.parquet"]
    require(entry["readers_match"] is True and entry["rows"] == 3 and len(entry["sha256"]) == 64, f"manifest {entry}")
    require(bpc.compare(baseline, bpc.manifest(root))["ok"], "unchanged tree compares clean")
    (root / "notes.txt").write_text("changed")
    (root / "Y=2026/new.parquet").write_bytes(b"PAR1 torn")
    result = bpc.compare(baseline, bpc.manifest(root))
    require(not result["ok"] and result["changed"] == ["notes.txt"], f"changed {result}")
    require(result["added"] == ["Y=2026/new.parquet"] and result["unreadable"] == ["Y=2026/new.parquet"], "unreadable")
    (root / "Y=2026/a.parquet").unlink()
    require(bpc.compare(baseline, bpc.manifest(root))["missing"] == ["Y=2026/a.parquet"], "missing")
    base = work / "baseline.json"
    with contextlib.redirect_stdout(io.StringIO()):
        require(bpc.main(["manifest", str(root), "--out", str(base)]) == 0, "manifest cli")
        require(bpc.main(["compare", str(base), str(root)]) == 1, "compare cli fails on unreadable files")


def main() -> None:
    require("serial" not in sys.modules, "the harness must not import pyserial in tests")
    test_parse_line()
    test_account()
    test_classify_and_plan()
    test_cutters()
    with tempfile.TemporaryDirectory(prefix="aq-powercut-") as temporary:
        work = Path(temporary)
        test_bench_run(work)
        test_reaction_and_missed_cut(work)
        test_manifest_compare(work)
    require("serial" not in sys.modules, "no serial module was loaded")
    print("PASS power cut bench harness on a simulated board, no port opened")


if __name__ == "__main__":
    main()
