"""Check a board's rendered LIVE key map against frozen fixtures keyed by digest.

The phone picks its LIVE profile by dictionary SHA-256, so one digest must
always mean one LIVE key set. A key change without a digest change fails here,
and so does a new digest without a frozen fixture entry. Legacy digests that
already shipped two key sets are kept as `collision` entries and may never be
the current build.

Each build also records which dictionary field every key carries. A LIVE key
never changes meaning (docs/shared/ble-sync-protocol.md#live-key-stability), so
across every digest of every board a key maps to one field. The only exceptions
are the builds that sent the IMU die temperature as `t` before that rule.
"""

from __future__ import annotations

import copy
import json
import re
from pathlib import Path

FIXTURES = Path(__file__).resolve().parent / "fixtures/live_keys"
PROTOCOL = Path(__file__).resolve().parents[1] / "docs/shared/ble-sync-protocol.md"
BOARDS = ("cores3", "waveshare")
# The LIVE `utc` source column was renamed for TIMESTAMP(MICROS, UTC) in e166978.
# The wire value stayed nanoseconds, so both names are one field.
FIELD_ALIASES = {"event_time_utc_ns": "event_time_utc_us"}
# Builds that sent imu_temperature_c as `t` before the LIVE key stability rule.
# They are listed by name in ble-sync-protocol.md. No later build may join them.
PRE_RULE_EXCEPTIONS = {
    ("cores3", "20f850852413abb1165f3f7ca95b4831a0e90a34ae7c7fcb41c44b108d3513c3", "d38a971", "t"),
    ("cores3", "f35d73981ca65d9de587a4ec7988ccea19e71dce988c69eb4bb99713f2352bb3", "95ef33c", "t"),
    ("cores3", "22291d3aab63365404394f30c1549fb18bde0699df9b2de863de70bb3f4edf65", "e166978", "t"),
}
PRE_RULE_FIELD = "imu_temperature_c"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def unique_pairs(items: list[tuple[str, object]]) -> dict[str, object]:
    """JSON object hook that refuses a duplicate name instead of keeping the last one."""
    names = [name for name, _ in items]
    require(len(names) == len(set(names)), f"duplicate JSON name in {names}")
    return dict(items)


def load(board: str) -> dict:
    fixture = json.loads((FIXTURES / f"{board}.json").read_text(), object_pairs_hook=unique_pairs)
    require(fixture["board"] == board, "fixture board")
    return fixture


def build_keys(build: dict) -> list[str]:
    """LIVE keys of one build in emit order, from its ordered key to field map."""
    return list(build["fields"])


def rendered_keys(payload: str) -> list[str]:
    """Keys of one LIVE JSON object in emit order. Duplicates fail."""
    pairs = json.loads(payload, object_pairs_hook=lambda items: items)
    keys = [key for key, _ in pairs]
    require(len(keys) == len(set(keys)), "duplicate LIVE key")
    return keys


def check(board: str, digest: str, firmware: str, payload: str) -> list[str]:
    digests = load(board)["digests"]
    for known, entry in digests.items():
        builds = entry["builds"]
        require(len(known) == 64 and builds, f"fixture entry {known[:8]}")
        sets = {frozenset(build_keys(build)) for build in builds}
        # A digest names one key set. Only a flagged legacy collision may break that.
        require(len(sets) == 1 or entry.get("collision") is True,
                f"{board} digest {known[:8]} lists different LIVE key sets without a collision flag")
    entry = digests.get(digest)
    require(entry is not None,
            f"{board} digest {digest[:8]} has no frozen LIVE fixture in tools/fixtures/live_keys/{board}.json")
    require(entry.get("collision") is not True,
            f"{board} digest {digest[:8]} is a legacy collision, ship a new dictionary digest")
    current = entry["builds"][-1]
    keys = rendered_keys(payload)
    require(current["firmware"] == firmware, f"{board} fixture firmware {current['firmware']} != {firmware}")
    require(keys == build_keys(current),
            f"{board} LIVE keys changed under digest {digest[:8]}: {keys} != {build_keys(current)}")
    require(payload == current["payload"], f"{board} LIVE sample payload changed under digest {digest[:8]}")
    return keys


def render(pairs: list[tuple[str, object]]) -> str:
    """Rebuild one compact LIVE JSON object from ordered key and value pairs."""
    return "{" + ",".join(f"{json.dumps(key)}:{json.dumps(value)}" for key, value in pairs) + "}"


def assert_rejects_drift(board: str, digest: str, firmware: str, payload: str) -> None:
    """The check itself must refuse a renamed, dropped or reordered key and a legacy digest.

    Cases come from the rendered keys, so they stay meaningful after any legitimate rename.
    """
    fixture = load(board)
    pairs = json.loads(payload, object_pairs_hook=lambda items: items)
    require(len(pairs) >= 3, f"{board} LIVE payload too short for drift cases")
    require(render(pairs) == payload, f"{board} LIVE payload does not round trip through render")
    keys = [key for key, _ in pairs]
    renamed = [(keys[0] + "_renamed", pairs[0][1])] + pairs[1:]
    dropped = pairs[:1] + pairs[2:]
    swapped = pairs[:1] + [pairs[2], pairs[1]] + pairs[3:]
    cases = [(digest, render(case)) for case in (renamed, dropped, swapped)]
    cases += [(known, payload) for known, entry in fixture["digests"].items() if entry.get("collision")]
    for case_digest, case_payload in cases:
        require(case_payload != payload or case_digest != digest, "drift case is not a change")
        try:
            check(board, case_digest, firmware, case_payload)
        except AssertionError:
            continue
        raise AssertionError(f"{board} LIVE fixture check accepted drift under {case_digest[:8]}")


def key_fields(fixtures: dict[str, dict] | None = None) -> dict[str, str]:
    """Each LIVE key maps to one dictionary field across every digest of every board.

    Only PRE_RULE_EXCEPTIONS may differ, and each of them must still be in a fixture.
    Returns the key to field map with FIELD_ALIASES folded in.
    """
    fixtures = fixtures if fixtures is not None else {board: load(board) for board in BOARDS}
    seen: dict[str, dict[str, list[str]]] = {}
    used = set()
    for board, fixture in fixtures.items():
        for digest, entry in fixture["digests"].items():
            for build in entry["builds"]:
                where = f"{board} {digest[:8]} {build.get('commit', build['firmware'])}"
                for key, field in build["fields"].items():
                    require(isinstance(field, str) and re.fullmatch(r"[a-z][a-z0-9_]*", field) is not None,
                            f"{where} LIVE key {key} names no dictionary field")
                    exception = (board, digest, build.get("commit"), key)
                    if exception in PRE_RULE_EXCEPTIONS:
                        require(field == PRE_RULE_FIELD, f"{where} pre-rule {key} is {field}, not {PRE_RULE_FIELD}")
                        used.add(exception)
                        continue
                    seen.setdefault(key, {}).setdefault(FIELD_ALIASES.get(field, field), []).append(where)
    require(used == PRE_RULE_EXCEPTIONS,
            f"pre-rule LIVE exception missing from the fixtures: {sorted(PRE_RULE_EXCEPTIONS - used)}")
    for key, by_field in seen.items():
        require(len(by_field) == 1, f"LIVE key {key} maps to more than one dictionary field: {by_field}")
    return {key: next(iter(by_field)) for key, by_field in seen.items()}


def check_fields(board: str, digest: str, dictionary_fields: list[dict]) -> None:
    """The current build's key to field map is the one the compiled dictionary declares."""
    current = load(board)["digests"][digest]["builds"][-1]
    expected = {field["live"]: field["name"] for field in dictionary_fields if field["live"]}
    require(current["fields"] == expected and build_keys(current) == list(expected),
            f"{board} fixture key to field map under digest {digest[:8]} differs from the dictionary")


def documented_keys(text: str) -> set[str]:
    """Keys named in the first column of the LIVE table in ble-sync-protocol.md."""
    start = text.find("### `live` (read, notify)")
    end = text.find("#### LIVE key stability", start)
    require(0 <= start < end, "LIVE section not found in ble-sync-protocol.md")
    keys: set[str] = set()
    for line in text[start:end].splitlines():
        if line.startswith("| `"):
            keys.update(re.findall(r"`([a-z0-9_]+)`", line.split("|")[1]))
    return keys


def check_documented(keys: list[str], text: str) -> None:
    missing = [key for key in keys if key not in documented_keys(text)]
    require(not missing, f"LIVE keys missing from the LIVE table in docs/shared/ble-sync-protocol.md: {missing}")


def check_key_meaning(board: str, digest: str, dictionary_fields: list[dict]) -> None:
    """Run the key to field and documentation checks, and prove each one rejects a mutation."""
    key_fields()
    check_fields(board, digest, dictionary_fields)
    keys = [field["live"] for field in dictionary_fields if field["live"]]
    text = PROTOCOL.read_text()
    check_documented(keys, text)
    fixtures = {name: load(name) for name in BOARDS}
    # A current key reused for another key's field, and a pre-rule build relabelled away.
    reused = copy.deepcopy(fixtures)
    fields = reused[board]["digests"][digest]["builds"][-1]["fields"]
    fields[keys[0]] = fields[keys[1]]
    stale = copy.deepcopy(fixtures)
    old_board, old_digest, old_commit, old_key = sorted(PRE_RULE_EXCEPTIONS)[0]
    for build in stale[old_board]["digests"][old_digest]["builds"]:
        if build.get("commit") == old_commit:
            build["fields"][old_key] = "ambient_temperature_c"
    for case in (reused, stale):
        try:
            key_fields(case)
        except AssertionError:
            continue
        raise AssertionError(f"{board} LIVE key to field check accepted a reused or stale mapping")
    undocumented = "\n".join(line for line in text.splitlines()
                             if not (line.startswith("| `") and f"`{keys[0]}`" in line.split("|")[1]))
    try:
        check_documented(keys, undocumented)
    except AssertionError:
        return
    raise AssertionError(f"{board} LIVE documentation check accepted a table without {keys[0]}")
