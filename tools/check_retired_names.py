"""Fail on a retired AQ wire, NVS, Kconfig or serial name anywhere in the repo (#93).

The 2026-10-26 rename moved every name the phone, the host tools or a stored
blob depends on to its OpenSensor form, with no backward compatibility. This
check keeps the old names from coming back in code, tools, docs or CI.

Matched names (old form, see docs/shared/mobile-migration.md for the new one)
  mDNS service           the old _xxsync service type
  LAN handshake          the old four byte magic
  GATT base UUID         the old base prefix
  GATT service name      the old service display name
  NVS namespaces         the old config and coredump namespaces
  Kconfig symbols        the old logger and runtime symbols, with or without CONFIG_
  serial tags            a line tag at a line start or after a quote or backtick

History is exempt. The dated bench records, the migration log, the #93 audit
and the sdkconfig.rename maps name the old forms on purpose.

Exit 0 when clean, 1 with one path:line per finding, 2 when the repo cannot be
listed. tools/lint.sh fails on anything but 0.

Usage:
  python tools/check_retired_names.py [PATH ...]   # default every tracked file
"""

from __future__ import annotations

from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
# Each old name is split so this file does not match its own patterns.
OLD = "A" + "Q"
RETIRED = (
    ("mDNS service", re.compile("_" + "aq" + "sync")),
    ("LAN handshake", re.compile(OLD + "S1")),
    ("GATT base UUID", re.compile("c0a5" + "e9f0", re.I)),
    ("GATT service name", re.compile(OLD + " Sync")),
    ("NVS namespace", re.compile(r"\b" + "aq" + r"(cfg|dump)\b")),
    ("Kconfig symbol", re.compile(r"\b(CONFIG_)?" + OLD + r"_(LOGGER_WORKER_WDT|LOGGER_BENCH_STALL|LOG_PERSIST_|PM_DFS_)")),
    ("serial tag", re.compile("(^|[\"'`])" + OLD + r" [A-Z][A-Z_]+")),
)
# History that names the old forms on purpose.
EXEMPT = {
    "docs/shared/mobile-migration.md",
    "docs/boards/m5stack-cores3/bench-verified.md",
    "docs/boards/waveshare-esp32-s3-sim7670g/bench-verified.md",
    "docs/shared/stac-contract/firmware-audit.md",
    "docs/shared/stac-contract/firmware-audit-review.md",
    "firmware/common/logger/sdkconfig.rename",
    "firmware/common/runtime/sdkconfig.rename",
}
SKIP_SUFFIXES = {".lock", ".png", ".jpg", ".jpeg", ".gif", ".bin", ".elf", ".parquet", ".pdf", ".zip"}


def tracked(root: Path) -> list[str] | None:
    """Every tracked path, relative to root, or None when git cannot list them."""
    try:
        result = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], capture_output=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return [name for name in result.stdout.decode().split("\0") if name]


def scan_text(text: str) -> list[tuple[int, str]]:
    """Line number and kind of every retired name in one file's text."""
    findings = []
    for number, line in enumerate(text.splitlines(), 1):
        for kind, pattern in RETIRED:
            if pattern.search(line):
                findings.append((number, kind))
    return findings


def findings(root: Path, names: list[str]) -> list[str]:
    out = []
    for name in sorted(names):
        if name in EXEMPT or Path(name).suffix in SKIP_SUFFIXES:
            continue
        path = root / name
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        out += [f"{name}:{number}: retired {kind}" for number, kind in scan_text(text)]
    return out


def main(argv: list[str]) -> int:
    names = argv or tracked(ROOT)
    if names is None:
        print("!!! retired name check cannot list tracked files", file=sys.stderr)
        return 2
    found = findings(ROOT, names)
    for finding in found:
        print(f"!!! {finding}, use the OpenSensor name (docs/shared/mobile-migration.md)")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
