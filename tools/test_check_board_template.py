#!/usr/bin/env python3
"""Mutation tests of tools/check_board_template.py on both boards' headers."""

from __future__ import annotations

from pathlib import Path
import shutil
import tempfile

import check_board_template as gate

ROOT = Path(__file__).resolve().parents[1]
BOARDS = [ROOT / "firmware/esp-idf-cores3/bringup", ROOT / "firmware/esp-idf-waveshare-sim7670g/logger"]


def rejects(action, message: str) -> None:
    try:
        action()
    except SystemExit:
        return
    raise AssertionError(message)


def main() -> None:
    for board in BOARDS:
        raw = gate.check_headers(board)
        gate.check_image(raw, b"\xe9head" + raw + b"\0tail")
        rejects(lambda: gate.check_image(raw, b"head" + raw[:-1] + b"\0"), "truncated template")
        rejects(lambda: gate.check_image(raw, b"head" + raw + b"tail"), "template without its NUL")
        rejects(lambda: gate.check_image(raw, (raw + b"\0") * 2), "template twice")
        with tempfile.TemporaryDirectory() as scratch:
            copy = Path(scratch)
            for name in ("telemetry_template.h", "telemetry_descriptor_digest.h"):
                shutil.copy(board / name, copy / name)
            template = copy / "telemetry_template.h"
            text = template.read_text()
            template.write_text(text.replace("OpenSensorTemplate", "OpenSensorTemplatE", 1))
            rejects(lambda: gate.check_headers(copy), "edited template with the old desc")
            template.write_text(text)
            digest = copy / "telemetry_descriptor_digest.h"
            desc = gate.descriptor_digest(copy)
            flipped = desc[:-1] + ("0" if desc[-1] != "0" else "1")
            digest.write_text(digest.read_text().replace(desc, flipped))
            rejects(lambda: gate.check_headers(copy), "edited desc")
        print(f"board {board.parent.name}, {len(raw)} bytes")
    print("PASS board template gate rejects stale headers and images without the exact bytes")


if __name__ == "__main__":
    main()
