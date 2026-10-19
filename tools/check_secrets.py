"""Fail when git tracks a local secret (issue #45, app signing key).

The app signing key lives only in secrets/ on the owner machine, see
docs/shared/development.md#local-app-signing. That directory is ignored, but an
ignore rule does not stop `git add -f`. This check reads the git index and
fails when

- any tracked path is under secrets/, or
- a tracked file holds a PEM private key header, for any key type.

It prints the offending path only, never file contents. Symlinks and gitlinks
are skipped, so the check never reads outside the checkout.

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


def tracked_paths(root: Path) -> list[str]:
    """Paths in the git index of root. Raises OSError or CalledProcessError."""
    result = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], capture_output=True, check=True)
    return [name for name in result.stdout.decode("utf-8", "surrogateescape").split("\0") if name]


def findings(root: Path) -> list[str]:
    found = []
    for name in tracked_paths(root):
        if name == SECRETS_DIR or name.startswith(SECRETS_DIR + "/"):
            found.append(f"{name}: tracked under {SECRETS_DIR}/")
            continue
        path = root / name
        # A deleted but still staged file, a symlink or a submodule is not read.
        if path.is_symlink() or not path.is_file():
            continue
        try:
            data = path.read_bytes()
        except OSError as error:
            found.append(f"{name}: cannot read ({error.strerror})")
            continue
        if PRIVATE_KEY_RE.search(data):
            found.append(f"{name}: holds a PEM private key header")
    return found


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
