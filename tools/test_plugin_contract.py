#!/usr/bin/env python3
"""Gate and mutation tests of the plugin contract (#17).

The checked-in manifests, locks and board files must pass
tools/plugin_contract.py check(), including the base branch lock. Each rule
is then proved on a scratch copy of the files the gate reads, with the base
lock injected. The gate must reject each mutation with its own message, and
must accept a clean lock history.
"""

from __future__ import annotations

import contextlib
import copy
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

import plugin_contract as gate

ROOT = gate.ROOT
CORES3 = Path("firmware/esp-idf-cores3/bringup")
WAVESHARE = Path("firmware/esp-idf-waveshare-sim7670g/logger")
PMSA003 = gate.COMMON_MODULES / "pmsa003/module.json"
IMU = CORES3 / "modules/m5_imu/module.json"
IDENTITY = WAVESHARE / "modules/waveshare/module.json"
BOARD_FILES = ("board.json", gate.LOCK, "telemetry_fields.inc", "telemetry_semantics.inc", "telemetry_classes.inc")
# An older CoreS3 dict the migration notes name. The history tests use it as an earlier release.
OLDER_DICT = "1180e7273bf1dd4cce94ba02b84af61bdddeeb11dc7ddcef61967aca7031c472"


def no_base(_: Path) -> None:
    return None


def rejects(action, expected: str, mutation: str) -> None:
    try:
        action()
    except AssertionError as error:
        if expected not in str(error):
            raise AssertionError(f"{mutation} failed for another reason, {error}") from error
        return
    raise AssertionError(f"{mutation} passed the gate")


def scratch_copy(scratch: Path) -> Path:
    out = scratch / "repo"
    for directory in (gate.PLUGINS, gate.STAC, gate.TEMPLATES, gate.COMMON_MODULES):
        shutil.copytree(ROOT / directory, out / directory)
    for path in (gate.MIGRATION, gate.WRITER, gate.SYSTEM_MODULE):
        (out / path).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(ROOT / path, out / path)
    for board in (CORES3, WAVESHARE):
        (out / board).mkdir(parents=True, exist_ok=True)
        for name in BOARD_FILES:
            shutil.copy(ROOT / board / name, out / board / name)
        shutil.copytree(ROOT / board / "modules", out / board / "modules")
    return out


def read(path: Path) -> dict:
    return json.loads(path.read_text())


def edit_json(relative: Path, change):
    def apply(root: Path) -> None:
        document = read(root / relative)
        change(document)
        (root / relative).write_text(json.dumps(document, indent=2, ensure_ascii=False) + "\n")
    return apply


def replace_text(relative: Path, old: str, new: str):
    def apply(root: Path) -> None:
        text = (root / relative).read_text()
        gate.require(old in text, f"mutation anchor {old!r} missing in {relative}")
        (root / relative).write_text(text.replace(old, new, 1))
    return apply


def mutate(scratch: Path, edit, expected: str, mutation: str, base=no_base) -> None:
    root = scratch_copy(scratch)
    try:
        edit(root)
        rejects(lambda: gate.check(root, base), expected, mutation)
    finally:
        shutil.rmtree(root)
    print(f"PASS rejects {mutation}", flush=True)


def column(document: dict, segment: str, name: str) -> dict:
    return next(c for c in document["segments"][segment] if c["name"] == name)


def older_release(lock: dict) -> dict:
    """A shipped release that is a strict prefix of today's CoreS3 release."""
    newest = lock["releases"][-1]
    segments = copy.deepcopy(newest["segments"][:-1])
    return {"dict": OLDER_DICT, "rows": sum(s["rows"] for s in segments), "segments": segments}


def with_history(change=None):
    def apply(root: Path) -> None:
        def edit(lock: dict) -> None:
            lock["releases"].insert(0, older_release(lock))
            if change:
                change(lock)
        edit_json(CORES3 / gate.LOCK, edit)(root)
    return apply


def cores3_base(document: dict):
    return lambda path: document if path.parent.name == "bringup" else None


def main() -> None:
    results, examples = gate.check()
    for r in results:
        print(f"PASS {r.board} {r.columns} columns in {r.segments} segments, dict {r.dict}", flush=True)
    for line in examples:
        print(f"PASS example {line}", flush=True)

    board = CORES3 / "board.json"
    lock = CORES3 / gate.LOCK
    today = read(ROOT / lock)

    with tempfile.TemporaryDirectory(prefix="plugin-contract-") as temporary:
        scratch = Path(temporary)
        root = scratch_copy(scratch)
        gate.check(root, no_base)
        gate.check(root, cores3_base(today))
        shutil.rmtree(root)
        print("PASS the scratch copy passes, so each rejection below comes from its mutation", flush=True)

        # The manifests reproduce the board files byte for byte.
        mutate(scratch, edit_json(IMU, lambda d: column(d, "core", "accel_x_g")["firmware"].update(decimals=4)),
               "FIELD row", "a decimals change in a manifest")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pm1_cf1_ug_m3")["firmware"].update(
            live_key="x1")), "FIELD row", "a LIVE key change in a manifest")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_age_ms")["firmware"].update(
            validity="always")), "FIELD row", "a validity change in a manifest")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pm1_cf1_ug_m3").update(
            {"opensensor:plausible_range": [0, 999]})), "SEM row", "a plausible range change")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pm1_cf1_ug_m3").update(
            {"opensensor:aggregate": "maximum"})), "SEM row", "an aggregate change")
        mutate(scratch, edit_json(PMSA003, lambda d: d.update(model="PMS5003")), "SEM row",
               "a module model change")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_status")["classification:classes"][1]
                                  .update(name="warm")), "pms_status", "a class name change")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_status")["classification:classes"][0]
                                  .update(value=99)), "class values ascending", "a class value out of order")
        mutate(scratch, edit_json(PMSA003, lambda d: d.update(vendor='Plan"tower')), "needs escaping",
               "a string the .inc cannot spell")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pm1_cf1_ug_m3").update(
            {"opensensor:valid_when": {"column": "pms_status", "finite": True}})), "finite gate must name",
               "a finite gate on another column")
        mutate(scratch, edit_json(board, lambda d: d["segments"].insert(2, d["segments"].pop(3))),
               "FIELD row", "a segment reorder")
        mutate(scratch, edit_json(board, lambda d: d["segments"].pop()), "manifest columns",
               "a removed last segment")
        mutate(scratch, edit_json(board, lambda d: d.update(title="Other")), "title differs", "a title change")
        mutate(scratch, edit_json(board, lambda d: d.update(sample_interval_s=5)), "sample_interval_s differs", "a sample interval change")
        mutate(scratch, edit_json(WAVESHARE / "board.json", lambda d: d.pop("class_subsets")),
               "clock_status", "a dropped class subset")
        mutate(scratch, edit_json(WAVESHARE / "board.json", lambda d: d.update(
            class_subsets={"clock_status": ["none", "gps"]})), "class_subsets names a class",
               "a class subset naming an unknown class")

        # Schema rules.
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_age_ms").update(
            {"opensensor:field_id": 12})), "schema", "a field_id in a manifest")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_age_ms").update(type="int64")),
               "schema", "a template type in a manifest")
        mutate(scratch, edit_json(PMSA003, lambda d: d.update(id="PMS-A003")), "schema", "a bad module id")
        mutate(scratch, edit_json(IMU, lambda d: column(d, "core", "accel_x_g")["firmware"].update(decimals=10)),
               "schema", "decimals over 9")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_age_ms").update(colour="red")),
               "schema", "an unknown column member")
        mutate(scratch, edit_json(PMSA003, lambda d: d.pop("procedure")), "schema", "a module without procedure")
        mutate(scratch, edit_json(board, lambda d: d.update(board="M5 CoreS3")), "schema", "a bad board slug")
        mutate(scratch, edit_json(lock, lambda d: d["releases"][0].update(dict="nope")), "schema",
               "a lock dict that is not a digest")

        # Cross rules.
        mutate(scratch, edit_json(board, lambda d: d["segments"].append({"module": "nope", "segment": "core"})),
               "unknown module", "an unknown module")
        mutate(scratch, edit_json(board, lambda d: d["segments"].append({"module": "pmsa003", "segment": "nope"})),
               "unknown segment", "an unknown segment")
        mutate(scratch, edit_json(board, lambda d: d["segments"].append({"module": "system", "segment": "head"})),
               "segment system.head repeats", "a repeated segment")
        mutate(scratch, edit_json(board, lambda d: d["local_modules"].append("../../../common/x/module.json")),
               "does not match", "a local module outside the board app")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_age_ms").update(name="pms_frames")),
               "column name repeats", "a duplicate column in a module")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_frames")["firmware"].update(
            live_key="bat")), "LIVE key repeats", "a LIVE key repeat on one board")
        mutate(scratch, edit_json(IDENTITY, lambda d: column(d, "pms_identity", "sensor_serial_code")["firmware"]
                                  .update(live_key="imu_t")), "LIVE key imu_t", "a LIVE key naming two columns")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pms_age_ms")["firmware"].update(
            decimals=2)), "decimals on a non Float", "decimals on an integer column")
        mutate(scratch, edit_json(PMSA003, lambda d: column(d, "core", "pm1_cf1_ug_m3").update(
            {"opensensor:valid_when": {"column": "nope", "equals": 2}})), "valid_when names an unknown",
               "a valid_when on an unknown column")
        mutate(scratch, replace_text(gate.WRITER, "kMaxColumns = 96;", "kMaxColumns = 80;"), "over kMaxColumns",
               "the column cap")
        mutate(scratch, lambda r: (r / board).unlink(), "has a board.json", "a board app without board.json")

        # The lock.
        mutate(scratch, replace_text(CORES3 / "telemetry_fields.inc", "// FIELD(", "//  FIELD("),
               "telemetry_fields.inc is", "a comment only edit of telemetry_fields.inc")
        mutate(scratch, lambda r: (r / lock).unlink(), "missing shipped.lock", "a missing lock")
        mutate(scratch, edit_json(lock, lambda d: d["releases"][-1]["segments"][1].update(sha256="0" * 64)),
               "segments differ from the board manifest", "an edited segment hash")
        mutate(scratch, edit_json(lock, lambda d: d["releases"][-1]["segments"].pop()), "do not add up",
               "a removed lock segment")
        mutate(scratch, edit_json(lock, lambda d: d["releases"][-1]["segments"].insert(
            0, d["releases"][-1]["segments"].pop(1))), "not contiguous", "a reordered lock segment")
        mutate(scratch, with_history(lambda d: d["releases"][0].update(dict="1" * 64)), "never names dict",
               "a release the migration notes never name")

        root = scratch_copy(scratch)
        with_history()(root)
        results, _ = gate.check(root, cores3_base({"board": "m5stack-cores3", "releases": [older_release(today)]}))
        gate.require(results[0].dict == today["releases"][-1]["dict"], "the newest release is today's")
        shutil.rmtree(root)
        print("PASS accepts a release that appends to an older one", flush=True)

        def reorder_older(lock: dict) -> None:
            older = lock["releases"][0]["segments"]
            older[1], older[2] = older[2], older[1]
            older[1]["first"] = 1 + older[0]["rows"]
            older[2]["first"] = older[1]["first"] + older[1]["rows"]

        mutate(scratch, with_history(reorder_older), "reorders, removes or edits",
               "a newer release that reorders a shipped segment")
        mutate(scratch, with_history(lambda d: d["releases"][0]["segments"][2].update(sha256="2" * 64)),
               "reorders, removes or edits", "a newer release that edits a shipped segment")
        # A baseline release may edit a shipped segment only with its migration entry.
        heading = "Test baseline (plugin contract test)"

        def baseline(lock: dict) -> None:
            lock["releases"][0]["segments"][2].update(sha256="2" * 64)
            lock["releases"][1]["baseline"] = {"reason": "an owner approved edit", "migration": heading}

        def note(body: str):
            def apply(root: Path) -> None:
                path = root / gate.MIGRATION
                text = path.read_text()
                first = text.index("\n## ") + 1
                path.write_text(text[:first] + f"## {heading}\n\n{body}\n\n" + text[first:])
            return apply

        def both(*edits):
            return lambda root: [edit(root) for edit in edits]

        mutate(scratch, with_history(baseline), "names no", "a baseline release with no migration entry")
        mutate(scratch, both(with_history(baseline), note("No dict named here.")), "never names its dict",
               "a baseline whose migration entry never names its dict")
        mutate(scratch, with_history(lambda d: d["releases"][0].update(
            baseline={"reason": "x", "migration": heading})), "cannot be a baseline", "a first release baseline")
        root = scratch_copy(scratch)
        both(with_history(baseline), note(f"New dict `{today['releases'][-1]['dict']}`."))(root)
        gate.check(root, no_base)
        shutil.rmtree(root)
        print("PASS accepts a baseline release that edits a shipped segment", flush=True)
        mutate(scratch, with_history(lambda d: d["releases"].append(copy.deepcopy(d["releases"][0]))),
               "a dict repeats", "a dict that repeats")
        mutate(scratch, lambda r: None, "lost a release present on the base branch", "a release dropped from the base",
               base=cores3_base({"board": "m5stack-cores3", "releases": [older_release(today)] + today["releases"]}))
        edited = copy.deepcopy(today)
        edited["releases"][0]["segments"][0]["sha256"] = "3" * 64
        mutate(scratch, lambda r: None, "differs from the base branch", "a lock entry edited against the base",
               base=cores3_base(edited))
        mutate(scratch, lambda r: None, "names another board", "a base lock of another board",
               base=cores3_base({**today, "board": "other"}))

        # The base loader fails closed when the ref is missing.
        os.environ["PLUGIN_CONTRACT_BASE"] = "refs/heads/plugin-contract-no-such-ref"
        try:
            rejects(lambda: gate.git_base_lock(ROOT)(ROOT / lock), "is missing", "a missing base ref")
        finally:
            del os.environ["PLUGIN_CONTRACT_BASE"]
        print("PASS rejects a missing base ref", flush=True)

        # A lock absent at the base, as after a board directory move, is new
        # and the loader names it in a notice rather than skipping silently.
        first = subprocess.run(["git", "rev-list", "--max-parents=0", "HEAD"], cwd=ROOT, capture_output=True,
                               text=True, check=True).stdout.split()[0]
        os.environ["PLUGIN_CONTRACT_BASE"] = first
        notice = io.StringIO()
        try:
            with contextlib.redirect_stderr(notice):
                found = gate.git_base_lock(ROOT)(ROOT / lock)
        finally:
            del os.environ["PLUGIN_CONTRACT_BASE"]
        gate.require(found is None, "a lock absent at the base reads as new")
        gate.require(f"NOTICE {lock.as_posix()} is not on {first}" in notice.getvalue(),
                     f"no notice for a lock absent at the base, {notice.getvalue()!r}")
        print("PASS notices a lock absent at the base", flush=True)


if __name__ == "__main__":
    main()
