#!/usr/bin/env python3
"""Gate the embedded board template (#85) the way the dict digest is gated.

Before a build, desc in telemetry_descriptor_digest.h must be the SHA-256 of
the kTemplateJson bytes in telemetry_template.h. After a build, the app image
must hold exactly those bytes followed by their NUL, so the bytes in .rodata
are the bytes desc names. The host contract tests prove the headers match the
board tables, this script proves the image matches the headers.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"board template: {message}")


def template_bytes(firmware: Path) -> bytes:
    text = (firmware / "telemetry_template.h").read_text()
    body = re.search(r"inline constexpr char kTemplateJson\[\] =\n(.*?);\n", text, re.S)
    require(body is not None, "kTemplateJson not found")
    chunks = re.findall(r'^    "((?:[^"\\]|\\["\\])*)"$', body[1], re.M)
    require(len(chunks) == len(body[1].splitlines()), "kTemplateJson is not one literal per line")
    raw = "".join(re.sub(r'\\(["\\])', r"\1", chunk) for chunk in chunks).encode("ascii")
    require(raw and all(0x20 <= b <= 0x7E for b in raw), "kTemplateJson is not printable ASCII")
    return raw


def descriptor_digest(firmware: Path) -> str:
    found = re.findall(r'"([0-9a-f]{64})"', (firmware / "telemetry_descriptor_digest.h").read_text())
    require(len(found) == 1, "telemetry_descriptor_digest.h holds one SHA-256")
    return found[0]


def check_headers(firmware: Path) -> bytes:
    raw = template_bytes(firmware)
    desc = descriptor_digest(firmware)
    require(hashlib.sha256(raw).hexdigest() == desc, "desc is not the SHA-256 of kTemplateJson")
    json.loads(raw)
    return raw


def check_image(raw: bytes, image: bytes) -> None:
    require(image.count(raw + b"\0") == 1, "app image does not hold kTemplateJson and its NUL exactly once")


def app_image(build: Path) -> Path:
    description = json.loads((build / "project_description.json").read_text())
    return build / description["app_bin"]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", type=Path, help="board directory with telemetry_template.h")
    parser.add_argument("--build", type=Path, help="ESP-IDF build directory to check the app image of")
    args = parser.parse_args()
    raw = check_headers(args.firmware)
    desc = hashlib.sha256(raw).hexdigest()
    if args.build is not None:
        image = app_image(args.build)
        check_image(raw, image.read_bytes())
        print(f"PASS {image.name} embeds the board template, {len(raw)} bytes, desc {desc}")
    else:
        print(f"PASS {args.firmware.name} board template headers, {len(raw)} bytes, desc {desc}")


if __name__ == "__main__":
    main()
