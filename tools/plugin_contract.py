#!/usr/bin/env python3
"""Plugin contract gate (#17), docs/shared/plugins/README.md.

Module and board manifests describe each board's columns as ordered module
segments. This gate validates every manifest against the plugin v0.1.0
schemas, which resolve offline against the vendored stac-opensensor v0.1.0
schemas. It then expands each board's segments and requires (Option A) that

- the rendered FIELD rows equal the board telemetry_fields.inc FIELD rows in
  order, byte for byte,
- the rendered SEM rows equal the telemetry_semantics.inc SEM rows,
- the class names and nodata flags equal the telemetry_classes.inc rows,
- each rendered template column, title, keywords, instruments, LIVE keys and
  unit labels equal the published board template at today's dict,
- the board shipped.lock matches the file and the manifests, is append only
  against its own history and against the base branch lock, and names only
  releases that docs/shared/mobile-migration.md names.

The board .inc files stay frozen and hand written. Nothing here writes them.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Callable

from jsonschema import Draft7Validator, FormatChecker
from referencing import Registry, Resource

ROOT = Path(__file__).resolve().parents[1]
PLUGINS = Path("docs/shared/plugins")
SCHEMAS = PLUGINS / "v0.1.0"
EXAMPLES = PLUGINS / "examples"
STAC = Path("tools/stac_opensensor/v0.1.0")
TEMPLATES = Path("docs/shared/templates")
MIGRATION = Path("docs/shared/mobile-migration.md")
WRITER = Path("firmware/common/src/parquet_writer.h")
SYSTEM_MODULE = Path("firmware/common/logger/system.module.json")
COMMON_MODULES = Path("firmware/common/sensors/modules")
BOARD_GLOB = "firmware/esp-idf-*/*/board.json"
FIELDS_GLOB = "firmware/esp-idf-*/*/telemetry_fields.inc"
LOCK = "shipped.lock"

# Template column types, as tools/board_template.py maps them.
PHYSICAL_TYPES = {"Int32": "int32", "Int64": "int64", "Float": "float"}
LOGICAL_TYPES = {"uint8_code": "uint8", "wkb_point_crs84": "geometry"}
CLASS_ROW = re.compile(r'CLASS\(\s*(\w+)\s*,\s*([^,]+?)\s*,\s*"([^"]*)"\s*,\s*(true|false)\s*\)')

BaseLock = Callable[[Path], "dict | None"]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def load_json(path: Path) -> dict:
    def no_duplicates(pairs: list) -> dict:
        keys = [key for key, _ in pairs]
        require(len(keys) == len(set(keys)), f"{path}: duplicate key")
        return dict(pairs)

    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=no_duplicates)
    except json.JSONDecodeError as error:
        raise AssertionError(f"{path}: not JSON, {error}") from error


def validators(root: Path) -> dict[str, Draft7Validator]:
    resources = []
    for directory in (root / STAC, root / SCHEMAS):
        for path in sorted(directory.glob("*.json")):
            contents = json.loads(path.read_text())
            resources.append((contents["$id"].rstrip("#"), Resource.from_contents(contents)))
    registry = Registry().with_resources(resources)
    require("uri" in FormatChecker().checkers, "format uri is enforced (rfc3986-validator)")
    out = {}
    for kind in ("module", "board", "lock"):
        schema = json.loads((root / SCHEMAS / f"{kind}.json").read_text())
        Draft7Validator.check_schema(schema)
        out[kind] = Draft7Validator(schema, registry=registry, format_checker=FormatChecker())
    return out


def validate(validator: Draft7Validator, document: dict, where: str) -> None:
    errors = sorted(validator.iter_errors(document), key=lambda e: list(e.absolute_path))
    require(not errors, f"{where} schema: " + "; ".join(
        f"{'/'.join(map(str, e.absolute_path)) or '(root)'}: {e.message}" for e in errors[:5]))


# Rendering. Every value must round trip into the exact .inc spelling, so a
# value the renderer cannot spell fails here and never reaches a board file.


def number(value: float | int, where: str) -> str:
    require(isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value),
            f"{where}: not a finite number")
    if isinstance(value, float) and value.is_integer():
        value = int(value)
    text = str(value)
    require(float(text) == value, f"{where}: {value!r} does not round trip")
    return text


def literal(text: str, where: str) -> str:
    require('"' not in text and "\\" not in text and "\n" not in text, f"{where}: {text!r} needs escaping")
    return f'"{text}"'


@dataclass(frozen=True)
class Column:
    """One expanded board column, a manifest column with its module defaults resolved."""

    module: str
    segment: str
    spec: dict  # the manifest column
    procedure: str
    vendor: str
    model: str

    @property
    def name(self) -> str:
        return self.spec["name"]

    @property
    def firmware(self) -> dict:
        return self.spec["firmware"]


def field_row(column: Column) -> str:
    fw = column.firmware
    where = column.name
    return (f"FIELD({column.name}, {fw['type']}, {column.procedure}, {literal(fw['unit'], where)}, "
            f"{fw['validity']}, {literal(fw['live_key'], where)}, {fw['decimals']})")


def gate_text(column: Column) -> str:
    gate = column.spec.get("opensensor:valid_when")
    if gate is None:
        return ""
    if gate.get("finite") is True:
        # SEM spells a finite gate only on the column itself.
        require(gate["column"] == column.name, f"{column.name}: a finite gate must name the column itself")
        return "finite"
    value = gate["equals"]
    require(not isinstance(value, (str, bool)), f"{column.name}: SEM gates compare numbers only")
    return f"{gate['column']} == {number(value, column.name + ' valid_when')}"


def sem_row(column: Column) -> str:
    spec = column.spec
    where = column.name
    low, high = ("", "")
    if "opensensor:plausible_range" in spec:
        low, high = (number(v, where + " plausible_range") for v in spec["opensensor:plausible_range"])
    parts = [
        column.name,
        literal(spec.get("unit", ""), where),
        spec["roles"][0],
        literal(spec.get("cf:standard_name", ""), where),
        spec["opensensor:aggregate"],
        low,
        high,
        number(spec.get("opensensor:settle_s", 0), where + " settle_s"),
        literal(gate_text(column), where),
        literal(spec["opensensor:property"], where),
        literal(column.vendor, where),
        literal(column.model, where),
    ]
    return "SEM(" + ", ".join(parts) + ")"


def classes(column: Column, subsets: dict) -> list[dict]:
    listed = column.spec.get("classification:classes", [])
    keep = subsets.get(column.name)
    if keep is None:
        return listed
    names = [c["name"] for c in listed]
    require(set(keep) <= set(names), f"{column.name}: class_subsets names a class the module does not list")
    return [c for c in listed if c["name"] in keep]


def template_column(column: Column, field_id: int, subsets: dict) -> dict:
    spec = column.spec
    fw = column.firmware
    kind = LOGICAL_TYPES.get(fw["unit"]) or PHYSICAL_TYPES.get(fw["type"])
    require(kind is not None, f"{column.name}: {fw['type']} needs a logical unit")
    out = {"name": column.name, "type": kind}
    for key in ("unit", "roles", "cf:standard_name"):
        if key in spec:
            out[key] = spec[key]
    kept = classes(column, subsets)
    if kept:
        out["classification:classes"] = kept
    out["opensensor:property"] = spec["opensensor:property"]
    out["opensensor:procedure"] = column.procedure
    out["opensensor:field_id"] = field_id
    for key in ("opensensor:valid_when", "opensensor:settle_s", "opensensor:derived", "opensensor:aggregate",
                "opensensor:plausible_range"):
        if key in spec and not (key == "opensensor:settle_s" and spec[key] == 0):
            out[key] = spec[key]
    return out


def instrument(column: Column) -> bool:
    """tools/board_template.h instrument(), a procedure that names a fitted sensor."""
    return bool(column.model) and column.spec["roles"][0] != "referenceInformation" and \
        column.firmware["validity"] != "unavailable"


# Loading.


@dataclass
class Module:
    path: Path
    manifest: dict

    @property
    def id(self) -> str:
        return self.manifest["id"]


@dataclass
class Board:
    path: Path  # board.json
    manifest: dict
    modules: dict[str, Module] = field(default_factory=dict)  # local modules by id

    @property
    def directory(self) -> Path:
        return self.path.parent


def load_module(root: Path, path: Path, checks: dict) -> Module:
    manifest = load_json(path)
    validate(checks["module"], manifest, str(path.relative_to(root)))
    module = Module(path, manifest)
    names = [c["name"] for cols in manifest["segments"].values() for c in cols]
    require(len(names) == len(set(names)), f"{module.id}: a column name repeats inside the module")
    status = manifest.get("status")
    if status is not None:
        cols = [c for cols in manifest["segments"].values() for c in cols if c["name"] == status["column"]]
        require(len(cols) == 1, f"{module.id}: status column {status['column']} is not a module column")
        require(bool(cols[0].get("classification:classes")), f"{module.id}: status column has no classes")
    for cols in manifest["segments"].values():
        for c in cols:
            listed = c.get("classification:classes", [])
            values = [k["value"] for k in listed]
            require(values == sorted(set(values)), f"{c['name']}: class values ascending and unique")
            require(len({k["name"] for k in listed}) == len(listed), f"{c['name']}: class names unique")
    return module


def common_modules(root: Path, checks: dict) -> dict[str, Module]:
    paths = [root / SYSTEM_MODULE, *sorted((root / COMMON_MODULES).glob("*/module.json"))]
    out: dict[str, Module] = {}
    for path in paths:
        module = load_module(root, path, checks)
        require(module.id not in out, f"module id {module.id} repeats")
        if path != root / SYSTEM_MODULE:
            require(path.parent.name == module.id, f"{path.relative_to(root)}: directory is not the module id")
        else:
            require(module.id == "system", "system.module.json has id system")
        out[module.id] = module
    return out


def load_board(root: Path, path: Path, checks: dict, common: dict[str, Module]) -> Board:
    manifest = load_json(path)
    validate(checks["board"], manifest, str(path.relative_to(root)))
    board = Board(path, manifest)
    for relative in manifest.get("local_modules", []):
        module_path = (path.parent / relative).resolve()
        require(module_path.is_relative_to(path.parent.resolve()), f"{relative}: outside the board app")
        require(module_path.is_file(), f"{path.relative_to(root)}: missing local module {relative}")
        module = load_module(root, module_path, checks)
        require(module.id not in common and module.id not in board.modules,
                f"local module id {module.id} repeats a module id")
        require(module_path.parent.name == module.id, f"{relative}: directory is not the module id")
        board.modules[module.id] = module
    return board


def expand(board: Board, common: dict[str, Module]) -> list[Column]:
    seen = set()
    out: list[Column] = []
    for entry in board.manifest["segments"]:
        key = (entry["module"], entry["segment"])
        require(key not in seen, f"{board.manifest['board']}: segment {'.'.join(key)} repeats")
        seen.add(key)
        module = board.modules.get(entry["module"]) or common.get(entry["module"])
        require(module is not None, f"{board.manifest['board']}: unknown module {entry['module']}")
        segment = module.manifest["segments"].get(entry["segment"])
        require(segment is not None, f"{board.manifest['board']}: unknown segment {'.'.join(key)}")
        for spec in segment:
            fw = spec["firmware"]
            out.append(Column(module.id, entry["segment"], spec,
                              spec.get("opensensor:procedure", module.manifest["procedure"]),
                              fw.get("vendor", module.manifest.get("vendor", "")),
                              fw.get("model", module.manifest.get("model", ""))))
    return out


def max_columns(root: Path) -> int:
    found = re.search(r"\bkMaxColumns\s*=\s*(\d+)\s*;", (root / WRITER).read_text())
    require(found is not None, f"{WRITER}: kMaxColumns not found")
    return int(found.group(1))


def cross_rules(label: str, columns: list[Column], board: Board, cap: int) -> None:
    names = [c.name for c in columns]
    require(len(names) == len(set(names)), f"{label}: a column name repeats on the board")
    require(len(columns) <= cap, f"{label}: {len(columns)} columns, over kMaxColumns {cap}")
    properties = [c.spec["opensensor:property"] for c in columns]
    require(len(properties) == len(set(properties)), f"{label}: an opensensor:property repeats on the board")
    live = [c.firmware["live_key"] for c in columns if c.firmware["live_key"]]
    require(len(live) == len(set(live)), f"{label}: a LIVE key repeats on the board")
    known = set(names)
    for c in columns:
        gate = c.spec.get("opensensor:valid_when")
        require(gate is None or gate["column"] in known, f"{c.name}: valid_when names an unknown column")
        derived = c.spec.get("opensensor:derived")
        require(derived is None or set(derived["from"]) <= known, f"{c.name}: derived names an unknown column")
        require((derived is not None) == (c.spec["roles"] == ["modelResult"]), f"{c.name}: derived and modelResult")
        require(c.firmware["type"] == "Float" or c.firmware["decimals"] == 0, f"{c.name}: decimals on a non Float")
    for name in board.manifest.get("class_subsets", {}):
        require(name in known, f"{label}: class_subsets names unknown column {name}")


def live_keys_across(boards: dict[str, list[Column]]) -> None:
    owner: dict[str, tuple[str, str]] = {}
    for label, columns in boards.items():
        for c in columns:
            key = c.firmware["live_key"]
            if key:
                first = owner.setdefault(key, (c.name, label))
                require(first[0] == c.name, f"LIVE key {key} is {first[0]} on {first[1]} and {c.name} on {label}")


# Board files.


def inc_rows(text: str, macro: str) -> list[str]:
    rows = [line for line in text.splitlines() if line.startswith(macro + "(")]
    stray = [line for line in text.splitlines() if line.lstrip().startswith(macro + "(") and line not in rows]
    require(not stray, f"{macro} row not at column 0: {stray[:1]}")
    return rows


def class_rows(text: str) -> list[tuple[str, str, bool]]:
    code = re.sub(r"//[^\n]*", "", text)
    rows = [(m.group(1), m.group(3), m.group(4) == "true") for m in CLASS_ROW.finditer(code)]
    require(len(rows) == code.count("CLASS("), "a CLASS row did not parse")
    return rows


def segment_hash(rows: list[str]) -> str:
    return hashlib.sha256(("\n".join(rows) + "\n").encode()).hexdigest()


def lock_segments(columns: list[Column], rows: list[str]) -> list[dict]:
    out: list[dict] = []
    for index, column in enumerate(columns):
        if out and (out[-1]["module"], out[-1]["segment"]) == (column.module, column.segment):
            out[-1]["rows"] += 1
        else:
            out.append({"module": column.module, "segment": column.segment, "first": index + 1, "rows": 1})
    for entry in out:
        entry["sha256"] = segment_hash(rows[entry["first"] - 1:entry["first"] - 1 + entry["rows"]])
    return out


def git_base_lock(root: Path) -> BaseLock:
    """Read <path> at origin/<PR base or main>. Fail closed when the ref is missing.

    On a push to main GITHUB_BASE_REF is empty, so the base is the pushed
    commit and this compares the lock to itself. That is by design. Branch
    protection forces a pull request, where the base check is real, and the
    other lock rules still run on push.
    """
    ref = os.environ.get("PLUGIN_CONTRACT_BASE") or f"origin/{os.environ.get('GITHUB_BASE_REF') or 'main'}"

    def load(path: Path) -> dict | None:
        relative = path.resolve().relative_to(root.resolve()).as_posix()
        known = subprocess.run(["git", "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}"], cwd=root,
                               capture_output=True, text=True)
        require(known.returncode == 0, f"base ref {ref} is missing, fetch it or set PLUGIN_CONTRACT_BASE")
        shown = subprocess.run(["git", "show", f"{ref}:{relative}"], cwd=root, capture_output=True, text=True)
        if shown.returncode != 0:
            listed = subprocess.run(["git", "ls-tree", "--name-only", ref, "--", relative], cwd=root,
                                    capture_output=True, text=True)
            require(listed.returncode == 0 and not listed.stdout.strip(), f"cannot read {relative} at {ref}")
            # The lock is new on this branch, or its board directory moved. Say so,
            # so a move never passes unseen.
            print(f"NOTICE {relative} is not on {ref}, treating it as a new lock", file=sys.stderr, flush=True)
            return None
        return json.loads(shown.stdout)

    return load


def check_lock(root: Path, board: Board, raw: bytes, columns: list[Column], rows: list[str], checks: dict,
               base_lock: BaseLock) -> dict:
    path = board.directory / LOCK
    label = board.manifest["board"]
    require(path.is_file(), f"{label}: missing {LOCK}")
    lock = load_json(path)
    validate(checks["lock"], lock, str(path.relative_to(root)))
    require(lock["board"] == label, f"{LOCK}: board is {lock['board']}, not {label}")
    releases = lock["releases"]
    dicts = [r["dict"] for r in releases]
    require(len(dicts) == len(set(dicts)), f"{label} {LOCK}: a dict repeats")
    for release in releases:
        segments = release["segments"]
        first = 1
        for entry in segments:
            require(entry["first"] == first, f"{label} {LOCK}: segments of {release['dict'][:12]} are not contiguous")
            first += entry["rows"]
        require(first - 1 == release["rows"], f"{label} {LOCK}: rows of {release['dict'][:12]} do not add up")
    for older, newer in zip(releases, releases[1:]):
        require(newer["segments"][:len(older["segments"])] == older["segments"],
                f"{label} {LOCK}: release {newer['dict'][:12]} reorders, removes or edits a shipped segment")
        require(newer["rows"] >= older["rows"], f"{label} {LOCK}: release {newer['dict'][:12]} drops rows")
    newest = releases[-1]
    digest = hashlib.sha256(raw).hexdigest()
    require(newest["dict"] == digest, f"{label}: telemetry_fields.inc is {digest}, the newest {LOCK} dict is "
                                      f"{newest['dict']}")
    require(newest["rows"] == len(columns), f"{label}: newest {LOCK} rows differ from the manifests")
    require(newest["segments"] == lock_segments(columns, rows),
            f"{label}: newest {LOCK} segments differ from the board manifest")
    notes = (root / MIGRATION).read_text()
    for release in releases:
        require(release["dict"] in notes, f"{label}: {MIGRATION} never names dict {release['dict']}")
    base = base_lock(path)
    if base is not None:
        require(base.get("board") == label, f"{label}: base {LOCK} names another board")
        earlier = base.get("releases", [])
        require(len(earlier) <= len(releases), f"{label}: {LOCK} lost a release present on the base branch")
        for index, entry in enumerate(earlier):
            require(json.dumps(releases[index], sort_keys=True) == json.dumps(entry, sort_keys=True),
                    f"{label}: {LOCK} release {index + 1} differs from the base branch")
    return newest


def check_board(root: Path, board: Board, columns: list[Column], checks: dict, base_lock: BaseLock) -> dict:
    label = board.manifest["board"]
    directory = board.directory
    raw = (directory / "telemetry_fields.inc").read_bytes()
    have = inc_rows(raw.decode("utf-8"), "FIELD")
    want = [field_row(c) for c in columns]
    for index, (a, b) in enumerate(zip(want, have)):
        require(a == b, f"{label} FIELD row {index + 1} differs\n manifest {a}\n board    {b}")
    require(len(want) == len(have), f"{label}: {len(want)} manifest columns, {len(have)} FIELD rows")

    have_sem = inc_rows((directory / "telemetry_semantics.inc").read_text(), "SEM")
    want_sem = [sem_row(c) for c in columns]
    for index, (a, b) in enumerate(zip(want_sem, have_sem)):
        require(a == b, f"{label} SEM row {index + 1} differs\n manifest {a}\n board    {b}")
    require(len(want_sem) == len(have_sem), f"{label}: {len(want_sem)} manifest columns, {len(have_sem)} SEM rows")

    subsets = board.manifest.get("class_subsets", {})
    want_classes = [(c.name, k["name"], k.get("nodata", False)) for c in columns for k in classes(c, subsets)]
    have_classes = class_rows((directory / "telemetry_classes.inc").read_text())
    # The .inc groups rows by column in value order, but the column groups
    # need not follow field order, so compare per column.
    def grouped(rows: list[tuple[str, str, bool]]) -> dict[str, list[tuple[str, bool]]]:
        out: dict[str, list[tuple[str, bool]]] = {}
        for column, name, nodata in rows:
            require(column not in out or next(reversed(out)) == column,
                    f"{label}: CLASS rows of {column} are not grouped")
            out.setdefault(column, []).append((name, nodata))
        return out

    want_groups, have_groups = grouped(want_classes), grouped(have_classes)
    for column in sorted(set(want_groups) | set(have_groups)):
        require(want_groups.get(column) == have_groups.get(column),
                f"{label}: {column} classes differ, manifest {want_groups.get(column)} "
                f"telemetry_classes.inc {have_groups.get(column)}")

    # The lock first, so a dict move names the lock before the missing template.
    newest = check_lock(root, board, raw, columns, have, checks, base_lock)
    digest = newest["dict"]
    published = root / TEMPLATES / label / f"{digest}.json"
    require(published.is_file(), f"{label}: no published template for dict {digest}")
    template = load_json(published)
    manifest = board.manifest
    require(template["id"] == manifest["schema"] == template["firmware"]["schema"], f"{label}: schema differs")
    require(template["opensensor:sample_interval_s"] == manifest["sample_interval_s"],
            f"{label}: sample_interval_s differs from the template")
    require(template["title"] == manifest.get("title"), f"{label}: title differs from the template")
    require(template["keywords"] == manifest.get("keywords"), f"{label}: keywords differ from the template")
    instruments = list(dict.fromkeys(c.procedure for c in columns if instrument(c)))
    require(template["instruments"] == instruments, f"{label}: instruments differ, {instruments}")
    firmware = template["firmware"]
    live = {c.name: c.firmware["live_key"] for c in columns if c.firmware["live_key"]}
    require(firmware["live_keys"] == live and list(firmware["live_keys"]) == list(live),
            f"{label}: LIVE keys differ from the template")
    labels = {c.name: c.firmware["unit"] for c in columns if c.firmware["unit"] != c.spec.get("unit", "")}
    require(firmware["unit_labels"] == labels, f"{label}: unit labels differ from the template")
    published_columns = template["item_assets"]["data"]["table:columns"]
    require(len(published_columns) == len(columns), f"{label}: template column count differs")
    for index, (column, got) in enumerate(zip(columns, published_columns, strict=True)):
        expected = template_column(column, index + 1, subsets)
        if expected != got:
            require(False, f"{label} template column {index + 1} {column.name} differs, {diff(expected, got)}")
    return newest


def diff(expected: dict, got: dict) -> list[str]:
    keys = sorted(set(expected) | set(got))
    return [f"{k}: manifest {expected.get(k)!r} template {got.get(k)!r}" for k in keys if expected.get(k) != got.get(k)]


def check_examples(root: Path, checks: dict, common: dict[str, Module], cap: int) -> list[str]:
    directory = root / EXAMPLES
    modules = dict(common)
    for path in sorted(directory.glob("*.module.json")):
        module = load_module(root, path, checks)
        require(module.id not in modules, f"example module id {module.id} repeats a module id")
        modules[module.id] = module
    boards = sorted(directory.glob("*.board.json"))
    require(boards, f"{EXAMPLES}: no example board")
    out = []
    for path in boards:
        manifest = load_json(path)
        validate(checks["board"], manifest, str(path.relative_to(root)))
        board = Board(path, manifest)
        columns = expand(board, modules)
        cross_rules(manifest["board"], columns, board, cap)
        for c in columns:
            field_row(c)
            sem_row(c)
        out.append(f"{manifest['board']} {len(columns)} columns")
    return out


@dataclass(frozen=True)
class Result:
    board: str
    columns: int
    segments: int
    dict: str


def check(root: Path = ROOT, base_lock: BaseLock | None = None) -> tuple[list[Result], list[str]]:
    root = root.resolve()
    checks = validators(root)
    if base_lock is None:
        base_lock = git_base_lock(root)
    common = common_modules(root, checks)
    cap = max_columns(root)
    board_paths = sorted(root.glob(BOARD_GLOB))
    with_fields = {p.parent for p in root.glob(FIELDS_GLOB)}
    require(with_fields == {p.parent for p in board_paths},
            "every board app with telemetry_fields.inc has a board.json and the reverse, "
            f"{sorted(str(p.relative_to(root)) for p in with_fields ^ {p.parent for p in board_paths})}")
    boards = [load_board(root, path, checks, common) for path in board_paths]
    slugs = [b.manifest["board"] for b in boards]
    require(len(slugs) == len(set(slugs)), "a board slug repeats")
    expanded = {}
    for board in boards:
        columns = expand(board, common)
        cross_rules(board.manifest["board"], columns, board, cap)
        expanded[board.manifest["board"]] = columns
    live_keys_across(expanded)
    results = []
    for board in boards:
        columns = expanded[board.manifest["board"]]
        newest = check_board(root, board, columns, checks, base_lock)
        results.append(Result(board.manifest["board"], len(columns), len(newest["segments"]), newest["dict"]))
    examples = check_examples(root, checks, common, cap)
    return results, examples


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    results, examples = check(args.root)
    for r in results:
        print(f"PASS {r.board} {r.columns} columns in {r.segments} segments reproduce FIELD, SEM, CLASS and the "
              f"template, dict {r.dict}", flush=True)
    for line in examples:
        print(f"PASS example {line}", flush=True)


if __name__ == "__main__":
    main()
