"""Host test for tools/analyze_discharge.py on a synthetic discharge run.

Run with ``pixi run analyze-discharge-test``.

The fixture is generated here with PyArrow in the archive layout of
docs/shared/telemetry-pipeline.md, so no binary is committed. It covers a
synced boot with mixed timestamp encodings, nulls and NaN in the battery
columns and one long gap, unsynced reboot loop boots with small files, a
pre schema v5 file without the new battery columns, a quarantine file, a
torn file and board logs with several boots. It also covers a clean single
boot run with a recharge boot after it, which must have no findings, the
loop boot rules, Critical to cutoff under 182 s, overlapping LOG_TAIL polls
of one boot, the one line overlap, one line piece, two named ids and named
loop boot merge limits, a named capture holding several polls, the
first critical boot never being a loop boot, a nothing-open answer to a
finalize, and the finalize findings. Each of those checks was confirmed
to fail against the code before the verify-77 review, or against a mutant.
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

THING = "thing=00000000-0000-4000-8000-000000000001"
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
    dated = root / "output" / THING / "year=2026" / "month=10" / "day=20"
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
        write(root / "output" / THING / "unsynced" / f"boot={name}" / f"data_unsynced_{name}_1-{count}-0.parquet",
              rows(boot, 1, count, utc_start=None, mv_start=3300, state=state_for))
    quarantine = root / "output" / "quarantine" / THING / "year=2026" / "q.parquet"
    write(quarantine, rows(BOOT_A, 181, 2, utc_start=T0, mv_start=3300, state=state_for))
    (dated / "torn.parquet").write_bytes(b"PAR1 torn")


LOG = """\
# capture started
ESP-ROM:esp32s3-20210327
rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)
12.001 PARQUET BEGIN schema=waveshare-v5 columns=62 boot={a} thing=x codec=lz4 crc32_check=ok
POWER STATE state=ok mv=3600
PARQUET READY name=data_0000_{a}_1-90-0.parquet rows=90 row_groups=1 first=1 last=90
POWER STATE state=low mv=3500
POWER STATE state=critical mv=3350 action=finalize
PARQUET FLUSH reason=power-critical result=closed attempt=1 files=1
ESP-ROM:esp32s3-20210327
rst:0xf (BROWNOUT),boot:0x8 (SPI_FAST_FLASH_BOOT)
PARQUET RECOVERY quarantined=1 retained=0
PARQUET BEGIN schema=waveshare-v5 boot={b} thing=x
PARQUET SCAN files=4 retained_partials=0
POWER STATE state=critical mv=3310 action=finalize
PARQUET FLUSH reason=power-critical result=busy-retry storage=absent attempt=1 files=0 retry_ms=1000
PARQUET FLUSH reason=power-critical result=closed attempt=2 files=1
PARQUET READY name=data_unsynced_{b}_1-5-0.parquet rows=5 row_groups=1 first=1 last=5
ESP-ROM:esp32s3-20210327
PARQUET BEGIN schema=waveshare-v5 boot={c} thing=x
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
PARQUET BEGIN schema=waveshare-v5 boot={c} thing=x
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
    require([x["loop"] for x in boots] == [False, True, True], "boots after the first critical are loop boots")
    require(summary["loop_small_files"] == 2, f"{summary}")
    problems = ad.findings(None, summary)
    require(any("more than one power-critical close" in p for p in problems), f"{problems}")
    require(any("give ups" in p for p in problems), f"{problems}")
    require(summary["boots_with_repeat_finalize"] == [2], f"{summary}")
    require(any("re-arm rule" in p for p in problems), f"two finalize requests in one boot, {problems}")
    require(any("small files from reboot loop boots" in p for p in problems), f"{problems}")
    require(summary["boots_with_unclosed_finalize"] == [], "every finalize here got a close or give up")

    # A LOG_TAIL has no ROM banner. Lines before the first BEGIN join its boot,
    # as RECOVERY lines do in a capture.
    tail = ad.parse_log(LOG_TAIL.format(c=c), source="tail.txt")
    require(len(tail) == 1 and tail[0]["boot"] == c, f"{tail}")
    require(tail[0]["nothing_open"] == 1 and tail[0]["closed"] == 0 and tail[0]["ready_files"] == 2, f"{tail}")
    require(tail[0]["ready_rows"] == [88] and tail[0]["small_files"] == 0, "a malformed rows= is not small")
    require(ad.parse_log("") == [], "empty log")
    require(len(ad.parse_log("ESP-ROM:x\nESP-ROM:y\n")) == 1, "back to back banners are one boot")

    # A second BEGIN without a ROM banner starts a new boot, as in a LOG_TAIL
    # that spans a reboot.
    split = ad.parse_log(f"PARQUET BEGIN boot={a}\nPOWER STATE state=ok mv=3600\n"
                         f"PARQUET BEGIN boot={b}\nPOWER STATE state=low mv=3480\n", source="tail.txt")
    require([x["boot"] for x in split] == [a, b], f"second BEGIN splits, {split}")
    require([x["states"] for x in split] == [["ok"], ["low"]], f"{split}")

    # A finalize that never got a close, give up or nothing-open.
    hung = ad.summarize_logs(ad.parse_log(f"PARQUET BEGIN boot={a}\n"
                                          "POWER STATE state=critical mv=3340 action=finalize\n"))
    require(hung["boots_with_unclosed_finalize"] == [0], f"{hung}")
    require(any("action=finalize with no close" in p for p in ad.findings(None, hung)), f"{hung}")
    # A finalize with nothing open has nothing to close, so it is answered.
    idle = ad.summarize_logs(ad.parse_log(f"PARQUET BEGIN boot={a}\n"
                                          "POWER STATE state=critical mv=3340 action=finalize\n"
                                          "PARQUET FLUSH reason=power-critical result=nothing-open attempt=1 files=0\n"))
    require(idle["boots_with_unclosed_finalize"] == [], f"nothing-open answers a finalize, {idle}")

    # The boot that went critical first is not a loop boot, even when it never
    # logged ok, so its own short close file does not count.
    reset_at_low = ad.parse_log(f"PARQUET BEGIN boot={a}\nPOWER STATE state=low mv=3480\n"
                                "POWER STATE state=critical mv=3340 action=finalize\n"
                                "PARQUET FLUSH reason=power-critical result=closed attempt=1 files=1\n"
                                "PARQUET READY name=x rows=4 row_groups=1 first=1 last=4\n")
    require([x["loop"] for x in reset_at_low] == [False], f"the first critical boot is no loop boot, {reset_at_low}")
    require(ad.summarize_logs(reset_at_low)["loop_small_files"] == 0, f"{reset_at_low}")

    # Loop boots without storage print no BEGIN and repeat the same lines.
    # Boots of one log are never joined, even then.
    same = "ESP-ROM:x\nPOWER STATE state=critical mv=null action=finalize\n" \
           "PARQUET FLUSH reason=power-critical result=nothing-open attempt=1 files=0\n"
    require(len(ad.parse_log(same * 3, source="capture.log")) == 3, "identical boots in one log stay apart")


POLLS = (
    # Poll 1, early in the boot.
    """\
PARQUET BEGIN schema=waveshare-v5 boot={e} thing=x
POWER STATE state=ok mv=3600
PARQUET READY name=data_0000_{e}_1-12-0.parquet rows=12 row_groups=1 first=1 last=12
POWER STATE state=low mv=3500
""",
    # Poll 2, the same boot again, BEGIN still in the ring.
    """\
PARQUET BEGIN schema=waveshare-v5 boot={e} thing=x
POWER STATE state=ok mv=3600
PARQUET READY name=data_0000_{e}_1-12-0.parquet rows=12 row_groups=1 first=1 last=12
POWER STATE state=low mv=3500
POWER STATE state=critical mv=3350 action=finalize
PARQUET FLUSH reason=power-critical result=closed attempt=1 files=1
""",
    # Poll 3 after the recharge reboot, BEGIN of the old boot scrolled out.
    """\
POWER STATE state=critical mv=3350 action=finalize
PARQUET FLUSH reason=power-critical result=closed attempt=1 files=1
PARQUET READY name=data_0100_{e}_13-20-0.parquet rows=8 row_groups=1 first=13 last=20
ESP-ROM:esp32s3-20210327
PARQUET BEGIN schema=waveshare-v5 boot={f} thing=x
POWER STATE state=external_power_likely mv=4250
PARQUET READY name=data_0500_{f}_1-9-0.parquet rows=9 row_groups=1 first=1 last=9
""",
)


def test_overlapping_polls(work: Path) -> None:
    e, f = "e" * 32, "f" * 32
    paths = []
    for i, text in enumerate(POLLS):
        path = work / f"ble-log-{i + 1}.txt"
        path.write_text(text.format(e=e, f=f))
        paths.append(path)
    boots = ad.parse_logs([(str(p), p.read_text()) for p in paths])
    require([x["boot"] for x in boots] == [e, f], f"polls of one boot join, {boots}")
    first, recharge = boots
    require(first["closed"] == 1 and first["finalize_requests"] == 1, f"one close, not one per poll, {first}")
    require(first["ready_rows"] == [12, 8] and first["states"] == ["ok", "low", "critical"], f"{first}")
    require(not first["loop"] and not recharge["loop"], "the recharge boot is not a loop boot")
    summary = ad.summarize_logs(boots)
    require(summary["closed"] == 1 and summary["loop_small_files"] == 0, f"{summary}")
    require(ad.findings(None, summary) == [], f"{ad.findings(None, summary)}")

    # Through the CLI as well, in time order.
    buffer = io.StringIO()
    args = [arg for p in paths for arg in ("--log", str(p))]
    with contextlib.redirect_stdout(buffer):
        require(ad.main(args + ["--out", str(work / "polls")]) == 0, "cli")
    require("\n! " not in buffer.getvalue(), buffer.getvalue())

    # A tail that lost its BEGIN, then the full serial capture of that boot.
    # The capture holds the tail, so nothing counts twice.
    capture = work / "capture.log"
    later = "\n".join(POLLS[2].splitlines()[2:]).format(e=e, f=f)
    capture.write_text("ESP-ROM:esp32s3-20210327\n" + POLLS[1].format(e=e) + later + "\n")
    tail = work / "tail.txt"
    tail.write_text("POWER STATE state=low mv=3500\n" + "\n".join(POLLS[2].splitlines()[:2]).format(e=e) + "\n")
    boots = ad.parse_logs([(str(tail), tail.read_text()), (str(capture), capture.read_text())])
    require([x["boot"] for x in boots] == [e, f], f"{boots}")
    require(boots[0]["closed"] == 1 and boots[0]["finalize_requests"] == 1, f"{boots[0]}")
    require(boots[0]["ready_rows"] == [12, 8], f"{boots[0]}")

    # One shared line is not an overlap. A tail without BEGIN whose first line
    # repeats the last line of an earlier poll is a later boot.
    one = [("ble-log-1.txt", f"PARQUET BEGIN boot={e}\nPOWER STATE state=ok mv=3600\n"
                             "POWER STATE state=low mv=3480\n"),
           ("ble-log-2.txt", "POWER STATE state=low mv=3480\nPOWER STATE state=critical mv=3290 action=finalize\n"
                             "PARQUET FLUSH reason=power-critical result=closed attempt=1 files=1\n")]
    boots = ad.parse_logs(one)
    require([x["boot"] for x in boots] == [e, None], f"a one line overlap does not join, {boots}")

    # The card is out (#61 step 6), so no SCAN line breaks the overlap. Loop
    # boot g prints BEGIN, then the same two lines the tail of the boot before
    # ended with. It names its id, so it joins the earlier boot without one
    # only by holding it, and its own close still counts.
    g = "a" * 32
    close = ("POWER STATE state=critical mv=3290 action=finalize\n"
             "PARQUET FLUSH reason=power-critical result=closed attempt=1 files=1\n")
    card_out = [("ble-log-1.txt", "POWER STATE state=low mv=3480\n" + close),
                ("capture.log", f"ESP-ROM:esp32s3-20210327\nPARQUET BEGIN boot={g}\n" + close
                                + "PARQUET READY name=x rows=2 row_groups=1 first=1 last=2\n")]
    boots = ad.parse_logs(card_out)
    require([x["boot"] for x in boots] == [None, g], f"a named loop boot stays apart, {boots}")
    require([x["closed"] for x in boots] == [1, 1], f"both closes count, {boots}")

    # Two named boots never join, even when one holds the other.
    h = "b" * 32
    boots = ad.parse_logs([("a.log", f"PARQUET BEGIN boot={g}\n" + close),
                           ("b.log", f"PARQUET BEGIN boot={h}\n" + close
                            + "PARQUET READY name=x rows=2 row_groups=1 first=1 last=2\n")])
    require([x["boot"] for x in boots] == [g, h], f"different ids stay apart, {boots}")

    # A one line piece without an id is too little to join, even when held.
    boots = ad.parse_logs([("ble-log-1.txt", "POWER STATE state=critical mv=3290 action=finalize\n"),
                           ("capture.log", f"PARQUET BEGIN boot={e}\nPOWER STATE state=low mv=3480\n" + close)])
    require([x["boot"] for x in boots] == [None, e], f"a held one line piece stays apart, {boots}")

    # A poll without an id taken from the middle of an earlier named capture.
    ok_low = "POWER STATE state=ok mv=3600\nPOWER STATE state=low mv=3480\n"
    boots = ad.parse_logs([("capture.log", f"PARQUET BEGIN boot={e}\nPARQUET SCAN files=3\n" + ok_low + close),
                           ("ble-log-1.txt", ok_low)])
    require([x["boot"] for x in boots] == [e] and boots[0]["closed"] == 1, f"a held poll joins, {boots}")

    # Two polls without an id that share no line, then the named capture that
    # holds both. Every poll joins it, so no READY counts twice.
    ready = "PARQUET READY name=y rows=90 row_groups=1 first=1 last=90\n"
    boots = ad.parse_logs([("ble-log-1.txt", ok_low + ready),
                           ("ble-log-2.txt", close),
                           ("capture.log", f"PARQUET BEGIN boot={e}\n" + ok_low + ready + close)])
    require([x["boot"] for x in boots] == [e], f"the capture takes in both polls, {boots}")
    require(boots[0]["ready_rows"] == [90] and boots[0]["closed"] == 1, f"{boots[0]}")
    # It takes in only polls without an id that it holds. A named boot whose
    # lines it repeats and a poll of another boot stay apart.
    boots = ad.parse_logs([("a.log", f"PARQUET BEGIN boot={g}\n" + close),
                           ("ble-log-1.txt", "POWER STATE state=external_power_likely mv=4250\n"
                                             "PARQUET READY name=z rows=9 row_groups=1 first=1 last=9\n"),
                           ("ble-log-2.txt", ok_low),
                           ("capture.log", f"PARQUET BEGIN boot={e}\n" + ok_low + close)])
    require([x["boot"] for x in boots] == [g, None, e], f"only held polls without an id join, {boots}")
    require([x["closed"] for x in boots] == [1, 0, 1], f"{boots}")
    # The two event minimum holds there too.
    boots = ad.parse_logs([("ble-log-1.txt", "POWER STATE state=ok mv=3600\n"),
                           ("ble-log-2.txt", "POWER STATE state=low mv=3480\n" + close),
                           ("capture.log", f"PARQUET BEGIN boot={e}\n" + ok_low + close)])
    require([x["boot"] for x in boots] == [None, e], f"a held one line poll stays apart, {boots}")

    # Card out, loop boots print no BEGIN. Two loop boots of one log, both
    # held by a later named capture, stay two boots, since boots of one log
    # never join, and every close counts.
    rom = "ESP-ROM:esp32s3-20210327\n"
    boots = ad.parse_logs([("loop.log", rom + close + rom + close),
                           ("capture.log", f"{rom}PARQUET BEGIN boot={e}\nPOWER STATE state=low mv=3480\n"
                                           + close + "PARQUET READY name=x rows=2 row_groups=1 first=1 last=2\n")])
    require([x["source"] for x in boots] == ["loop.log", "loop.log, capture.log"], f"one log stays split, {boots}")
    require(sum(x["closed"] for x in boots) == 2, f"{boots}")
    # The capture holds an earlier boot of its own log as well. That boot is
    # not taken in either.
    boots = ad.parse_logs([("ble-log-1.txt", "POWER STATE state=low mv=3480\n"
                                             "POWER STATE state=critical mv=3290 action=finalize\n"),
                           ("capture.log", rom + close + f"{rom}PARQUET BEGIN boot={e}\n"
                                           "POWER STATE state=low mv=3480\n" + close)])
    require([x["source"] for x in boots] == ["ble-log-1.txt, capture.log", "capture.log"],
            f"a boot of the capture's own log stays apart, {boots}")
    require([x["boot"] for x in boots] == [e, None] and [x["closed"] for x in boots] == [1, 1], f"{boots}")
    # The target comes from a third log, so the two loop boots share no log
    # with it. Once it takes in one of them, the other is from a log it holds.
    boots = ad.parse_logs([("loop.log", rom + close + rom + close),
                           ("ble-log-1.txt", ok_low),
                           ("capture.log", f"{rom}PARQUET BEGIN boot={e}\n" + ok_low + close)])
    require([x["source"] for x in boots] == ["loop.log", "loop.log, ble-log-1.txt, capture.log"],
            f"only one boot of a log is taken in, {boots}")


def test_clean_single_boot(work: Path) -> None:
    """One boot discharging to cutoff, then a recharge boot hours later, with nothing to flag."""
    root = work / "clean"
    dated = root / "output" / THING / "year=2026" / "month=10" / "day=21"
    boot_e, boot_f = (0x0E, 0x0E), (0x0F, 0x0F)
    first, mv, t = 1, 3620, T0
    for count in (20, 90, 90, 90, 12):  # short first file, power-critical close of 12 rows
        columns = rows(boot_e, first, count, utc_start=t, mv_start=mv, state=state_for)
        write(dated / f"data_{first:04d}_{hex_boot(boot_e)}_{first}-{first + count - 1}-0.parquet", columns)
        first, mv, t = first + count, mv - count, t + count * 10_000_000
    # Critical from mv 3350, row 271, to the last row 302, 31 steps of 10 s.
    # The recharge boot on USB, its first file short as always.
    later = t + 4 * 3_600_000_000
    write(dated / f"data_9999_{hex_boot(boot_f)}_1-20-0.parquet",
          rows(boot_f, 1, 20, utc_start=later, mv_start=4300, state=lambda _: 1))
    summary = ad.summarize_rows(ad.read_run(root), small_rows=30, gap_s=30.0)
    require(summary["small_files"] == 3 and summary["loop_small_files"] == 0, f"{summary['boots']}")
    require(summary["states"]["critical_to_last_row_s"] == 310.0, "the recharge boot must not stretch it, "
            f"got {summary['states']}")
    require(summary["states"]["last_mv"] == 3319, f"{summary['states']}")
    require(summary["gaps"]["count"] == 1 and summary["gaps"]["before_critical"] == 0, f"{summary['gaps']}")
    problems = ad.findings(summary, None)
    require(problems == [], f"a clean run has no findings, got {problems}")

    # The same boot cut off 90 s after Critical fails the 182 s rule.
    short = work / "short"
    write(short / "data.parquet", rows(boot_e, 1, 60, utc_start=T0, mv_start=3400, state=state_for))
    summary = ad.summarize_rows(ad.read_run(short), small_rows=30, gap_s=30.0)
    require(summary["states"]["critical_to_last_row_s"] == 90.0, f"{summary['states']}")
    require(any("under 182 s" in p for p in ad.findings(summary, None)), f"{ad.findings(summary, None)}")


def test_loop_boots(work: Path) -> None:
    """Which boots count as reboot loop boots on the Parquet side."""
    root = work / "loop"
    dated = root / "output" / THING / "year=2026" / "month=10" / "day=22"
    early, crit, synced, unsynced = (0x10, 0x10), (0x11, 0x11), (0x12, 0x12), (0x13, 0x13)
    # A watchdog reset at Low before Critical, small but not a loop boot.
    write(dated / "early.parquet", rows(early, 1, 10, utc_start=T0, mv_start=3460, state=state_for))
    # The boot that went critical starts at Critical. Its short first file and
    # its close file are not loop files.
    write(dated / "crit-1.parquet", rows(crit, 1, 25, utc_start=T0 + 100_000_000, mv_start=3350, state=state_for))
    write(dated / "crit-2.parquet", rows(crit, 26, 10, utc_start=T0 + 350_000_000, mv_start=3325, state=state_for))
    # Two loop boots, one with host time right after the cut, one without.
    write(dated / "synced.parquet", rows(synced, 1, 4, utc_start=T0 + 450_000_000, mv_start=3300, state=state_for))
    write(root / "output" / THING / "unsynced" / "u.parquet",
          rows(unsynced, 1, 3, utc_start=None, mv_start=3290, state=state_for))
    summary = ad.summarize_rows(ad.read_run(root), small_rows=30, gap_s=30.0)
    loop = {b["boot"]: b["loop"] for b in summary["boots"]}
    require(loop == {hex_boot(early): False, hex_boot(crit): False, hex_boot(synced): True,
                     hex_boot(unsynced): True}, f"{loop}")
    require(summary["small_files"] == 5 and summary["loop_small_files"] == 2, f"{summary['boots']}")
    # The next boot follows within the step, the segment still ends at its boot.
    require(summary["states"]["critical_to_last_row_s"] == 340.0, f"{summary['states']}")
    require(any("2 small Parquet files from reboot loop boots" in p for p in ad.findings(summary, None)),
            f"{ad.findings(summary, None)}")


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
    # Critical at row 91, the 120 s gap after row 131 ends the segment.
    require(states["critical_to_last_row_s"] == 400.0, f"{states}")
    require(summary["gaps"]["before_critical"] == 1, f"the boot D to A gap only, {summary['gaps']}")
    boots = {b["boot"]: b for b in summary["boots"]}
    require(boots[hex_boot(BOOT_A)]["files"] == 2 and boots[hex_boot(BOOT_A)]["small_files"] == 0, f"{boots}")
    require(boots[hex_boot(BOOT_B)]["small_files"] == 1 and boots[hex_boot(BOOT_B)]["unplaced_rows"] == 5, "B")
    require([b["loop"] for b in summary["boots"]].count(True) == 2, f"B and C are loop boots, {boots}")
    require(not boots[hex_boot(BOOT_A)]["loop"] and not boots[hex_boot(BOOT_D)]["loop"], f"{boots}")
    require(summary["small_files"] == 2, f"two small reboot loop files, got {summary['small_files']}")
    require(summary["loop_small_files"] == 2, f"{summary['loop_small_files']}")
    problems = ad.findings(summary, None)
    require(any("before the first critical row" in p for p in problems), f"{problems}")
    require(not any("under 182 s" in p for p in problems), f"{problems}")
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
        test_overlapping_polls(work)
        test_clean_single_boot(work)
        test_loop_boots(work)
    require("serial" not in sys.modules, "no serial module was loaded")
    print("PASS discharge analysis on a synthetic run and logs, no port opened")


if __name__ == "__main__":
    main()
