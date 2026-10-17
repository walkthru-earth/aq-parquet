"""Render an `idf.py size --format json2` report as a Markdown table.

CI writes the table to the job summary. With --baseline, each row also shows
the change against an earlier report. CI uses the build of the pull request
base commit when it has one, else the newest green main build, and names it
with --baseline-from.

Usage:
  python tools/size_report.py --label cores3/bringup size.json
  python tools/size_report.py --label cores3/bringup size.json --baseline main.json \
      --baseline-from "pull request base 0fffb2702d75, run 123"
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
    parser.add_argument("--baseline-from", default="an earlier build",
                        help="which build the baseline came from, shown above the table")
    args = parser.parse_args()
    current = load(args.report)
    baseline = load(args.baseline) if args.baseline else None

    lines = [f"### {args.label} image size", ""]
    if baseline is None:
        lines += ["No baseline build was available, so only absolute sizes are shown.", ""]
        lines += ["| Region | Used bytes |", "| --- | ---: |"]
        lines += [f"| {name} | {used:,} |" for name, used in current.items()]
    else:
        lines += [f"Change is relative to the {args.baseline_from}.", ""]
        lines += ["| Region | Used bytes | Change |", "| --- | ---: | ---: |"]
        lines += [f"| {name} | {used:,} | {delta(used, baseline.get(name))} |" for name, used in current.items()]
    print("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
