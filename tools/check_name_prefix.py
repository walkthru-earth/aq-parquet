"""Fail on a product BLE name prefix literal in shared code or host tools (#90).

The BLE name prefix is a board constant passed through ble::Identity, so
firmware/common, the host tools and their tests must not carry the old
product prefix as a literal. A board passes its own prefix from its own
directory. AQS1 and _aqsync belong to #93 and are not matched.

Only source files are scanned (C, C++, Python and shell), not docs, which
may name the old prefix when they describe the migration.

Exit 0 when clean, 1 with one path:line per finding, 2 when a directory is
missing. tools/lint.sh fails on anything but 0.

Usage:
  python tools/check_name_prefix.py [DIR ...]   # default firmware/common tools
"""

from __future__ import annotations

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DIRS = ("firmware/common", "tools")
SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".ino", ".py", ".sh"}
SKIP_PARTS = {"build", "__pycache__", "managed_components", ".pixi"}
# Split so this file does not match its own pattern.
PREFIX_RE = re.compile("A" + "Q-")


def scan(directory: Path) -> list[str]:
    findings = []
    for path in sorted(directory.rglob("*")):
        if path.suffix not in SUFFIXES or not path.is_file():
            continue
        if SKIP_PARTS.intersection(path.relative_to(directory).parts):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for number, line in enumerate(text.splitlines(), 1):
            if PREFIX_RE.search(line):
                findings.append(f"{path.relative_to(ROOT)}:{number}")
    return findings


def main(argv: list[str]) -> int:
    directories = [ROOT / d for d in (argv or DEFAULT_DIRS)]
    missing = [str(d) for d in directories if not d.is_dir()]
    if missing:
        print(f"!!! name prefix check cannot read {', '.join(missing)}", file=sys.stderr)
        return 2
    findings = [f for d in directories for f in scan(d)]
    for finding in findings:
        print(f"!!! product BLE name prefix literal at {finding}, pass it from the board")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
