"""Check a board's rendered LIVE key map against frozen fixtures keyed by digest.

The phone picks its LIVE profile by dictionary SHA-256, so one digest must
always mean one LIVE key set. A key change without a digest change fails here,
and so does a new digest without a frozen fixture entry. Legacy digests that
already shipped two key sets are kept as `collision` entries and may never be
the current build.
"""

from __future__ import annotations

import json
from pathlib import Path

FIXTURES = Path(__file__).resolve().parent / "fixtures/live_keys"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def rendered_keys(payload: str) -> list[str]:
    """Keys of one LIVE JSON object in emit order. Duplicates fail."""
    pairs = json.loads(payload, object_pairs_hook=lambda items: items)
    keys = [key for key, _ in pairs]
    require(len(keys) == len(set(keys)), "duplicate LIVE key")
    return keys


def check(board: str, digest: str, firmware: str, payload: str) -> list[str]:
    fixture = json.loads((FIXTURES / f"{board}.json").read_text())
    require(fixture["board"] == board, "fixture board")
    digests = fixture["digests"]
    for known, entry in digests.items():
        builds = entry["builds"]
        require(len(known) == 64 and builds, f"fixture entry {known[:8]}")
        sets = {frozenset(build["keys"]) for build in builds}
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
    require(keys == current["keys"],
            f"{board} LIVE keys changed under digest {digest[:8]}: {keys} != {current['keys']}")
    require(payload == current["payload"], f"{board} LIVE sample payload changed under digest {digest[:8]}")
    return keys


def render(pairs: list[tuple[str, object]]) -> str:
    """Rebuild one compact LIVE JSON object from ordered key and value pairs."""
    return "{" + ",".join(f"{json.dumps(key)}:{json.dumps(value)}" for key, value in pairs) + "}"


def assert_rejects_drift(board: str, digest: str, firmware: str, payload: str) -> None:
    """The check itself must refuse a renamed, dropped or reordered key and a legacy digest.

    Cases come from the rendered keys, so they stay meaningful after any legitimate rename.
    """
    fixture = json.loads((FIXTURES / f"{board}.json").read_text())
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
