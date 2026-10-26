"""Board template (#85) headers, digest and checks shared by both contract tests.

The host fixture prints the template that tools/board_template.h builds from
the board tables, and the bytes the firmware embeds from telemetry_template.h.
check() fails when the checked-in headers are stale, and write() regenerates
them. desc is the SHA-256 of the exact embedded bytes, never a re-serialized
copy. validate() runs the pinned stac-opensensor v0.1.0 schemas and the
profile rules a schema cannot express.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re

from jsonschema import Draft7Validator, FormatChecker
from referencing import Registry, Resource

SCHEMAS = Path(__file__).resolve().parent / "stac_opensensor/v0.1.0"
SPEC = "https://github.com/walkthru-earth/stac-opensensor (a017edf)"
CHUNK = 74


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def c_literal(raw: bytes) -> str:
    text = raw.decode("ascii")
    require(all(0x20 <= ord(c) <= 0x7E for c in text), "template is printable ASCII")
    # Pack escaped characters up to CHUNK columns, never splitting an escape,
    # so each line stays within 80 columns.
    lines, piece = [], ""
    for c in text:
        escaped = "\\" + c if c in '"\\' else c
        if len(piece) + len(escaped) > CHUNK:
            lines.append(f'    "{piece}"')
            piece = ""
        piece += escaped
    lines.append(f'    "{piece}"')
    return "\n".join(lines)


def template_header(raw: bytes, regenerate: str) -> str:
    return f"""#pragma once

// GENERATED, do not edit. Regenerate with
// {regenerate}
// The stac-opensensor v0.1.0 board template (#85), built by
// tools/board_template.h from telemetry_fields.inc, telemetry_semantics.inc,
// telemetry_classes.inc and the board constants in telemetry_contract.h.
// Minified printable ASCII. telemetry_descriptor_digest.h holds the SHA-256
// of exactly these bytes, without the terminating NUL.

namespace telemetry {{
namespace contract {{
// clang-format off
inline constexpr char kTemplateJson[] =
{c_literal(raw)};
// clang-format on
}} // namespace contract
}} // namespace telemetry
"""


def digest_header(raw: bytes, regenerate: str) -> str:
    return f"""#pragma once

namespace telemetry {{
namespace contract {{
// GENERATED, do not edit. Regenerate with
// {regenerate}
// desc, the SHA-256 of kTemplateJson in telemetry_template.h without its NUL.
// Checked by tools/build-idf-board.sh and the host contract test.
constexpr const char *kDescriptorSha256 =
    "{hashlib.sha256(raw).hexdigest()}";
}} // namespace contract
}} // namespace telemetry
"""


def write(firmware: Path, generated: bytes, regenerate: str) -> None:
    (firmware / "telemetry_template.h").write_text(template_header(generated, regenerate))
    (firmware / "telemetry_descriptor_digest.h").write_text(digest_header(generated, regenerate))


def check(firmware: Path, generated: bytes, embedded: bytes, desc: str, regenerate: str) -> str:
    """Fail on a stale header, return desc."""
    stale = f"stale board template, run {regenerate}"
    require((firmware / "telemetry_template.h").read_text() == template_header(generated, regenerate), stale)
    require((firmware / "telemetry_descriptor_digest.h").read_text() == digest_header(generated, regenerate), stale)
    require(embedded == generated, "embedded kTemplateJson differs from the generated template")
    digest = hashlib.sha256(embedded).hexdigest()
    require(desc == digest, "kDescriptorSha256 is not the SHA-256 of kTemplateJson")
    require(json.dumps(json.loads(embedded), separators=(",", ":"), ensure_ascii=True).encode() == embedded,
            "template is minified JSON with no duplicate key")
    return digest


def validator() -> Draft7Validator:
    resources = []
    for name in ("template.json", "profile.json", "schema.json"):
        contents = json.loads((SCHEMAS / name).read_text())
        resources.append((contents["$id"].rstrip("#"), Resource.from_contents(contents)))
    registry = Registry().with_resources(resources)
    schema = json.loads((SCHEMAS / "template.json").read_text())
    return Draft7Validator(schema, registry=registry, format_checker=FormatChecker())


def validate(template: dict, dictionary: dict, dictionary_uri: str) -> None:
    errors = sorted(validator().iter_errors(template), key=lambda e: list(e.path))
    require(not errors, "template schema: " + "; ".join(
        f"{'/'.join(map(str, e.path))}: {e.message}" for e in errors[:5]))
    require("uri" in FormatChecker().checkers, "format uri is enforced (rfc3986-validator)")
    fields = dictionary["fields"]
    firmware = template["firmware"]
    require(template["id"] == firmware["schema"] == dictionary["schema"], "id is the firmware schema name")
    require(firmware["dict"] == dictionary["sha256"], "firmware.dict is the dictionary digest")
    links = [link for link in template["links"] if link["rel"] == "describedby"]
    require(len(links) == 1 and links[0]["file:checksum"] == "1220" + dictionary["sha256"],
            "describedby checksum is 1220 plus dict")
    # Same repository path as the footer dictionary_uri, served raw so the
    # bytes at the link are the bytes hashed.
    blob = re.fullmatch(r"https://github\.com/([^/]+/[^/]+)/blob/(.+)", dictionary_uri)
    require(blob is not None and links[0]["href"] ==
            f"https://raw.githubusercontent.com/{blob[1]}/{blob[2]}", "describedby href is the raw dictionary")
    columns = template["item_assets"]["data"]["table:columns"]
    require([c["name"] for c in columns] == [f["name"] for f in fields], "one column per FIELD, in order")
    types = {1: "int32", 2: "int64", 4: "float"}
    for column, field in zip(columns, fields, strict=True):
        require(column["type"] == types[field["type"]], f"type: {field['name']}")
        require(column["opensensor:field_id"] == field["field_id"], f"field_id: {field['name']}")
        require(column["opensensor:procedure"] == field["procedure"], f"procedure: {field['name']}")
        require(column["opensensor:property"] == field["property"], f"property: {field['name']}")
        require(firmware["unit_labels"].get(field["name"], column["unit"]) == field["unit"],
                f"unit label: {field['name']}")
        require(field["name"] not in firmware["unit_labels"] or column["unit"] != field["unit"],
                f"unit label only where it differs: {field['name']}")
        gate = column.get("opensensor:valid_when")
        require(gate is None or gate["column"] in {f["name"] for f in fields}, f"gate column: {field['name']}")
        derived = column.get("opensensor:derived")
        require((derived is not None) == (column["roles"] == ["modelResult"]), f"derived: {field['name']}")
        if column["unit"] in ("{code}", "{boolean}"):
            classes = column.get("classification:classes", [])
            require([c["value"] for c in classes] == sorted({c["value"] for c in classes}),
                    f"class values ascending and unique: {field['name']}")
    require(firmware["live_keys"] == {f["name"]: f["live"] for f in fields if f["live"]},
            "live_keys follow the dictionary")
    require(list(firmware["live_keys"]) == [f["name"] for f in fields if f["live"]], "live_keys in field order")
    require(template["instruments"] == list(dict.fromkeys(template["instruments"])), "instruments unique")
    procedures = {f["procedure"] for f in fields}
    require(set(template["instruments"]) <= procedures, "instruments are FIELD procedures")


def run(executable: Path, firmware: Path, dictionary: dict, board_sources: list[Path], write_out: bool,
        regenerate: str) -> str | None:
    """Regenerate with write_out, else check and validate. Return desc."""
    import subprocess

    # sample_interval_s is the aqlogger::Config default. A board that sets its
    # own interval must pass it to the generator first.
    for source in board_sources:
        require(".sample_interval_ms" not in source.read_text(),
                f"{source.name} overrides the sample interval the template assumes")
    generated = subprocess.check_output([str(executable), "template"])
    if write_out:
        write(firmware, generated, regenerate)
        print(f"WROTE {firmware.name} board template, {len(generated)} bytes, rerun without --write-template",
              flush=True)
        return None
    embedded = subprocess.check_output([str(executable), "embedded"])
    desc = check(firmware, generated, embedded, dictionary["descriptor_sha256"], regenerate)
    validate(json.loads(embedded), dictionary, dictionary["uri"])
    print(f"PASS board template {len(embedded)} bytes, valid v0.1.0, desc {desc}", flush=True)
    return desc
