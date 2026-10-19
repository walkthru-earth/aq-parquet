#!/usr/bin/env python3
"""Compare two aq-build-sha256.json manifests of one board built from two checkout paths.

tools/check_repro_paths.sh builds the images and calls this. Every file in the
manifest must hash the same, except the compiler database and the project
description, which record absolute paths by design. For a file that differs,
the report counts the differing bytes and says whether either checkout path
appears in it, which points at a source that misses aq_map_source_paths().
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

# check_idf_target.py hashes these too, but they list absolute source and
# build paths, so they always differ between checkouts.
PATH_BOUND = frozenset({"compile_commands.json", "project_description.json"})
MANIFEST = "aq-build-sha256.json"


def compare(first: dict, second: dict) -> list[str]:
    """Return the problems that make two manifests disagree, empty when they match."""
    problems = []
    for key in ("board", "variant", "sdk_commit"):
        if first.get(key) != second.get(key):
            problems.append(f"{key} differs: {first.get(key)!r} and {second.get(key)!r}")
    files_a, files_b = first.get("files"), second.get("files")
    if not isinstance(files_a, dict) or not isinstance(files_b, dict) or not files_a:
        return problems + ["manifest has no file hashes"]
    if set(files_a) != set(files_b):
        problems.append(f"file lists differ: {sorted(set(files_a) ^ set(files_b))}")
    compared = sorted((set(files_a) & set(files_b)) - PATH_BOUND)
    if not compared:
        problems.append("no path independent files to compare")
    problems += [f"{name} differs" for name in compared if files_a[name] != files_b[name]]
    return problems


def describe(name: str, builds: list[Path], roots: list[Path]) -> str:
    """Byte level detail for one differing file, to find what leaked."""
    try:
        data = [(build / name).read_bytes() for build in builds]
    except OSError as error:
        return f"  {name}: cannot read ({error})"
    a, b = data
    differing = sum(x != y for x, y in zip(a, b)) + abs(len(a) - len(b))
    first = next((index for index, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
    leaks = [f"checkout {index + 1} path in build {build_index + 1}"
             for index, root in enumerate(roots)
             for build_index, blob in enumerate(data)
             if str(root).encode() in blob]
    leak = ", ".join(leaks) if leaks else "no checkout path found"
    return (f"  {name}: sizes {len(a)} and {len(b)}, {differing} bytes differ, "
            f"first at offset {first:#x}, {leak}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("first_root", type=Path)
    parser.add_argument("first_build", type=Path)
    parser.add_argument("second_root", type=Path)
    parser.add_argument("second_build", type=Path)
    args = parser.parse_args()
    builds = [args.first_build, args.second_build]
    try:
        manifests = [json.loads((build / MANIFEST).read_text()) for build in builds]
    except (OSError, ValueError) as error:
        raise SystemExit(f"repro check failed: cannot read {MANIFEST}: {error}") from error
    problems = compare(*manifests)
    if problems:
        print("Images built from two checkout paths differ")
        for problem in problems:
            print("  " + problem)
            if problem.endswith(" differs") and problem.split()[0] in manifests[0].get("files", {}):
                print(describe(problem.split()[0], builds, [args.first_root, args.second_root]))
        raise SystemExit(1)
    files = sorted(set(manifests[0]["files"]) - PATH_BOUND)
    print(f"{manifests[0]['board']}/{manifests[0]['variant']} built from two checkout paths "
          f"matches in {len(files)} files: {', '.join(files)}")


if __name__ == "__main__":
    main()
