"""The `opensensor` Parquet footer (#88) checks shared by the footer tests.

Every telemetry file carries exactly one key-value entry, `opensensor`, whose
value is compact UTF-8 JSON valid against the pinned stac-opensensor
json-schema/footer.json v0.1.0 (a56dfb0) in tools/stac_opensensor/v0.1.0. The
schema sets no additionalProperties, so problems() also holds every object to
the schema's members in the schema's order, with at least the members of the
pinned example footers/cores3-telemetry-v5.json. dict and desc come from the
caller, read from the board digest headers, never hardcoded here.
mutation_proof() shows each rule fails on a broken footer.
"""

from __future__ import annotations

import copy
import json
from pathlib import Path
import re

from jsonschema import Draft7Validator, FormatChecker
import pyarrow.parquet as pq

PINNED = Path(__file__).resolve().parent / "stac_opensensor"
SCHEMA = PINNED / "v0.1.0/footer.json"
EXAMPLE = PINNED / "footers/cores3-telemetry-v5.json"
SPEC = "https://github.com/walkthru-earth/stac-opensensor (a56dfb0)"
KEY = "opensensor"
# telemetry::Writer::finish caps one value at 4096 bytes, NUL not counted.
MAX_JSON_BYTES = 4096


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def schema() -> dict:
    return json.loads(SCHEMA.read_text())


def example() -> dict:
    return json.loads(EXAMPLE.read_text())


def validator() -> Draft7Validator:
    require("uri" in FormatChecker().checkers, "format uri is enforced (rfc3986-validator)")
    return Draft7Validator(schema(), format_checker=FormatChecker())


def digest_of(header: Path, name: str) -> str:
    """The 64 hex digits of `constexpr const char *<name> =` in a header."""
    match = re.search(name + r'\s*=\s*"([0-9a-f]{64})"', header.read_text())
    require(match is not None, f"{name} in {header}")
    return match[1]


def order_problems(document: dict, spec: dict, sample: dict, where: str = "") -> list[str]:
    """Members are a subset of the schema's, a superset of the example's and
    in schema order, at every level."""
    allowed = list(spec["properties"])
    keys = list(document)
    problems = []
    extra = [k for k in keys if k not in allowed]
    if extra:
        problems.append(f"{where or '/'}: members not in the schema {extra}")
    missing = [k for k in sample if k not in keys]
    if missing:
        problems.append(f"{where or '/'}: members of the example missing {missing}")
    known = [k for k in keys if k in allowed]
    if known != [k for k in allowed if k in keys]:
        problems.append(f"{where or '/'}: members out of schema order {keys}")
    for key in known:
        child = spec["properties"][key]
        if isinstance(document[key], dict) and "properties" in child:
            problems += order_problems(document[key], child, sample.get(key, {}), f"{where}/{key}")
    return problems


def problems(text: str, *, dict_sha: str, desc: str) -> list[str]:
    """Every reason the footer JSON breaks the contract, empty when it holds."""
    try:
        document = json.loads(text)
    except ValueError as error:
        return [f"not JSON: {error}"]
    found = [f"schema {'/'.join(map(str, e.path)) or '/'}: {e.message}"
             for e in sorted(validator().iter_errors(document), key=lambda e: list(e.path))]
    if not isinstance(document, dict):
        return found or ["footer is not an object"]
    found += order_problems(document, schema(), example())
    if json.dumps(document, separators=(",", ":"), ensure_ascii=False) != text:
        found.append("not compact JSON, or a duplicate member")
    if len(text.encode()) > MAX_JSON_BYTES:
        found.append(f"{len(text.encode())} bytes is over the writer cap {MAX_JSON_BYTES}")
    if document.get("dictionary", {}).get("sha256") != dict_sha:
        found.append("dictionary.sha256 is not dict")
    if document.get("template", {}).get("sha256") != desc:
        found.append("template.sha256 is not desc")
    return found


def kv_problems(kv: dict[str, str], *, dict_sha: str, desc: str) -> list[str]:
    """The whole key-value metadata, exactly one key named opensensor."""
    if list(kv) != [KEY]:
        return [f"footer keys {sorted(kv)}, want exactly [{KEY!r}]"]
    return problems(kv[KEY], dict_sha=dict_sha, desc=desc)


def read_kv(path: Path) -> dict[str, str]:
    raw = pq.ParquetFile(path).metadata.metadata or {}
    return {k.decode(): v.decode() for k, v in raw.items()}


def check_file(path: Path, *, dict_sha: str, desc: str) -> dict:
    """Validate a real file's footer and return the parsed JSON."""
    kv = read_kv(path)
    found = kv_problems(kv, dict_sha=dict_sha, desc=desc)
    require(not found, f"{path.name} footer: " + "; ".join(found))
    return json.loads(kv[KEY])


def mutation_proof(text: str, *, dict_sha: str, desc: str) -> list[str]:
    """Break a valid footer one rule at a time and require each to fail."""
    require(not kv_problems({KEY: text}, dict_sha=dict_sha, desc=desc), "baseline footer is valid")
    good = json.loads(text)

    def mutated(change) -> str:
        document = copy.deepcopy(good)
        change(document)
        return json.dumps(document, separators=(",", ":"), ensure_ascii=False)

    def string_number(d: dict) -> None:
        d["acquisition"]["sample_interval_ms"] = str(d["acquisition"]["sample_interval_ms"])

    def wrong_desc(d: dict) -> None:
        d["template"]["sha256"] = "0" * 64

    def missing(d: dict) -> None:
        del d["firmware"]["boot_id"]

    def long_rotation(d: dict) -> None:
        d["file"]["rotation_interval_s"] = 3601

    def extra_member(d: dict) -> None:
        d["station"]["country_iso3166_1_alpha2"] = "EG"

    cases = {
        "stray second footer key": {KEY: text, "schema_version": good["firmware"]["schema"]},
        "number written as a string": {KEY: mutated(string_number)},
        "wrong template.sha256": {KEY: mutated(wrong_desc)},
        "missing required field": {KEY: mutated(missing)},
        "rotation_interval_s over 3600": {KEY: mutated(long_rotation)},
        "unspecified extra JSON member": {KEY: mutated(extra_member)},
    }
    results = []
    for name, kv in cases.items():
        found = kv_problems(kv, dict_sha=dict_sha, desc=desc)
        require(bool(found), f"mutation not caught: {name}")
        results.append(f"{name}: fails ({found[0]})")
    return results


def board_digests(firmware: Path) -> tuple[str, str]:
    """dict and desc of a board, read from its generated digest headers."""
    return (digest_of(firmware / "telemetry_dictionary_digest.h", "kDictionarySha256"),
            digest_of(firmware / "telemetry_descriptor_digest.h", "kDescriptorSha256"))


def check_document(document: dict, dictionary: dict, templates: str, *, compressed: bool,
                   rotation: int, row_groups: int) -> None:
    """The values a board writes, against its compiled dictionary. `templates`
    is the board folder under docs/shared/templates."""
    require(document["firmware"]["version"] == dictionary["firmware"], "firmware.version")
    require(document["firmware"]["schema"] == dictionary["schema"], "firmware.schema")
    require(document["dictionary"] == {"sha256": dictionary["sha256"], "version": dictionary["dictionary"],
                                       "uri": dictionary["uri"]}, "dictionary group")
    # Both URIs are raw at a full commit (#89). pinned-uris-test proves the
    # commit is on HEAD and its bytes hash to dict and desc.
    require(re.fullmatch(r"https://raw\.githubusercontent\.com/walkthru-earth/aq-parquet/[0-9a-f]{40}/.+",
                         document["dictionary"]["uri"]) is not None, "dictionary.uri is raw at a full commit")
    require(re.fullmatch(r"https://raw\.githubusercontent\.com/walkthru-earth/aq-parquet/[0-9a-f]{40}/"
                         + re.escape(f"docs/shared/templates/{templates}/{dictionary['sha256']}.json"),
                         document["template"]["href"]) is not None,
            "template.href is the published template of dict at a full commit")
    require(document["template"]["sha256"] == dictionary["descriptor_sha256"], "template.sha256 is desc")
    require(document["station"]["deployment_id"] == document["acquisition"]["calibration_id"] == "unknown",
            "no fabricated deployment or calibration")
    require(document["acquisition"]["sample_interval_ms"] == 10000, "sample interval")
    require(document["acquisition"]["time_semantics"] != "", "time semantics")
    require(json.loads(document["acquisition"]["config"])["sample_interval_ms"] == 10000,
            "acquisition.config is the configuration JSON as a string")
    require(document["file"] == {"rotation_interval_s": rotation, "row_groups": row_groups,
                                 "row_group_rows_max": 90,
                                 "compression": "LZ4_RAW" if compressed else "UNCOMPRESSED",
                                 "purpose": "telemetry"}, "file group")


def check_board(executable: Path, firmware: Path, dictionary: dict, templates: str) -> str:
    """The fixture's `footer` mode prints what the firmware builds for a full
    one hour LZ4 file. Validate it, prove each mutation fails and return it."""
    import subprocess

    dict_sha, desc = board_digests(firmware)
    require((dict_sha, desc) == (dictionary["sha256"], dictionary["descriptor_sha256"]),
            "compiled digests equal the digest headers")
    text = subprocess.check_output([str(executable), "footer"], text=True)
    found = kv_problems({KEY: text}, dict_sha=dict_sha, desc=desc)
    require(not found, "board footer: " + "; ".join(found))
    check_document(json.loads(text), dictionary, templates, compressed=True, rotation=3600, row_groups=4)
    # template.href names the file #86 publishes, whose SHA-256 is desc.
    import hashlib

    published = (Path(__file__).resolve().parents[1] / "docs/shared/templates" / templates /
                 f"{dict_sha}.json")
    require(published.is_file() and hashlib.sha256(published.read_bytes()).hexdigest() == desc,
            f"template.href target {published.name} is published with SHA-256 desc")
    for line in mutation_proof(text, dict_sha=dict_sha, desc=desc):
        print(f"PASS mutation {line}", flush=True)
    print(f"PASS {dictionary['schema']} opensensor footer {len(text.encode())} bytes, "
          f"valid against footer.json {SPEC}", flush=True)
    return text
