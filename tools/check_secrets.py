"""Fail when git tracks a local secret (issue #45, app signing key).

The app signing key lives only in secrets/ on the owner machine, see
docs/shared/development.md#local-app-signing. That directory is ignored, but an
ignore rule does not stop `git add -f`. This check reads the staged blobs in
the git index, not the working tree, so a key that was staged and then deleted
or replaced on disk still fails. It fails when

- any tracked path is under secrets/, or
- a tracked file holds a PEM private key header, for any key type.

It prints the offending path only, never file contents. Symlink and gitlink
entries are skipped by their index mode.

Exit 0 when clean, 1 with one line per finding, 2 when git cannot list the
files. tools/lint.sh fails on anything but 0.

Usage:
  python tools/check_secrets.py [--root DIR]   # default the repository root
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SECRETS_DIR = "secrets"
# Split so this file does not match its own pattern. It covers RSA, EC,
# ENCRYPTED, OPENSSH and plain PKCS#8 private keys.
PRIVATE_KEY_RE = re.compile(b"-----BEGIN (?:[A-Z0-9]+ )*" + b"PRIVATE" + b" KEY-----")


def index_entries(root: Path) -> list[tuple[str, str, str]]:
    """(mode, blob id, path) for every entry in the git index of root.
    Raises OSError or CalledProcessError."""
    result = subprocess.run(["git", "-C", str(root), "ls-files", "--stage", "-z"], capture_output=True, check=True)
    entries = []
    for record in result.stdout.decode("utf-8", "surrogateescape").split("\0"):
        if record:
            meta, _, name = record.partition("\t")
            mode, blob, _stage = meta.split(" ")
            entries.append((mode, blob, name))
    return entries


def read_blobs(root: Path, blobs: list[str]) -> dict[str, bytes]:
    """Blob contents by id from the object store, in one git process."""
    if not blobs:
        return {}
    request = "".join(f"{blob}\n" for blob in blobs).encode()
    out = subprocess.run(["git", "-C", str(root), "cat-file", "--batch"], input=request, capture_output=True,
                         check=True).stdout
    contents, pos = {}, 0
    for blob in blobs:
        end = out.index(b"\n", pos)
        header = out[pos:end].split(b" ")
        if len(header) != 3:
            raise subprocess.CalledProcessError(1, ["git", "cat-file"], stderr=f"blob {blob} missing".encode())
        size = int(header[2])
        contents[blob] = out[end + 1:end + 1 + size]
        pos = end + 1 + size + 1
    return contents


def findings(root: Path) -> list[str]:
    found = []
    regular = []
    for mode, blob, name in index_entries(root):
        if name == SECRETS_DIR or name.startswith(SECRETS_DIR + "/"):
            found.append(f"{name}: tracked under {SECRETS_DIR}/")
        # Regular files only. A symlink entry (120000) holds a target path and
        # a gitlink (160000) is another repository.
        elif mode in ("100644", "100755"):
            regular.append((blob, name))
    contents = read_blobs(root, sorted({blob for blob, _ in regular}))
    for blob, name in regular:
        if PRIVATE_KEY_RE.search(contents[blob]):
            found.append(f"{name}: holds a PEM private key header")
    # A conflicted path has one entry per stage. Report it once.
    return list(dict.fromkeys(found))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=ROOT, help="git checkout to check")
    try:
        args = parser.parse_args(argv)
    except SystemExit as error:
        return 0 if error.code == 0 else 2
    try:
        found = findings(args.root)
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"check_secrets: cannot list tracked files in {args.root}, {error}", file=sys.stderr)
        return 2
    for line in found:
        print(line)
    if found:
        print(f"check_secrets: {len(found)} tracked secret finding(s). Untrack them with git rm --cached, "
              "and rotate any key that was ever pushed.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
