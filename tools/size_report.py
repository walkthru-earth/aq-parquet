"""Render an `idf.py size --format json2` report as a Markdown table.

CI writes the table to the job summary. With --baseline, each row also shows
the change against an earlier report, usually the latest green main build.

Usage:
  python tools/size_report.py --label cores3/bringup size.json
  python tools/size_report.py --label cores3/bringup size.json --baseline main.json
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def load(path: Path) -> dict[str, int]:
    report = json.loads(path.read_text())
    rows = {"Total image": int(report["total_size"])}
    for region in report["layout"]:
        rows[region["name"]] = int(region["used"])
    return rows


def delta(current: int, previous: int | None) -> str:
    if previous is None:
        return "new"
    change = current - previous
    if change == 0:
        return "0"
    percent = f" ({change / previous:+.2%})" if previous else ""
    return f"{change:+,}{percent}"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("report", type=Path)
    parser.add_argument("--label", required=True)
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    current = load(args.report)
    baseline = load(args.baseline) if args.baseline else None

    lines = [f"### {args.label} image size", ""]
    if baseline is None:
        lines += ["No main baseline was available, so only absolute sizes are shown.", ""]
        lines += ["| Region | Used bytes |", "| --- | ---: |"]
        lines += [f"| {name} | {used:,} |" for name, used in current.items()]
    else:
        lines += ["Change is relative to the latest successful main build.", ""]
        lines += ["| Region | Used bytes | Change vs main |", "| --- | ---: | ---: |"]
        lines += [f"| {name} | {used:,} | {delta(used, baseline.get(name))} |" for name, used in current.items()]
    print("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
