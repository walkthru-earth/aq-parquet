"""Sign app images with the local app signing key (issue #45, owner machine only).

The key lives in secrets/ on the owner machine and is never a CI secret. See
docs/shared/development.md#local-app-signing for the one-time setup.
secrets/.env names the key

  AQ_APP_SIGNING_KEY=<file name of the PEM, relative to secrets/>
  AQ_APP_SIGNING_SCHEME=rsa3072   # optional, the only scheme ESP32-S3 has

Each unsigned app .bin is signed with `espsecure sign-data --version 2` into
the output directory as <stem>.signed.bin, then checked with
`espsecure verify-signature --version 2`. SHA256SUMS in the output directory
lists every signed image, in sha256sum format.

It refuses to run when secrets/.env is missing, when anything under secrets/
is tracked by git, when the key or secrets/.env is open to group or other,
when the key is not an RSA-3072 PEM private key inside secrets/, or when the
output directory is inside secrets/. It never prints the key, the .env values
or any file content. The key is named only as secrets/<name>.

Firmware does not check app signatures yet. That waits on #40.

Usage:
  pixi run release-sign --out <dir> <app.bin> [<app.bin> ...]
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
KEY_VAR = "AQ_APP_SIGNING_KEY"
SCHEME_VAR = "AQ_APP_SIGNING_SCHEME"
# ESP32-S3 Secure Boot v2 verifies RSA-3072 only. ECDSA v2 is for other chips.
SCHEMES = {"rsa3072": 3072}
DEFAULT_SCHEME = "rsa3072"
ENV_NAME_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
SUMS = "SHA256SUMS"
ESPSECURE = [sys.executable, "-m", "espsecure"]


class Refused(Exception):
    """A precondition failed. The message never holds a secret value."""


def parse_env(text: str) -> dict[str, str]:
    """Minimal .env reader. KEY=VALUE lines, optional `export `, # comments,
    optional matching quotes. A bad line is named by number, never echoed."""
    values = {}
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[len("export "):].lstrip()
        name, sep, value = line.partition("=")
        name = name.strip()
        if not sep or not ENV_NAME_RE.fullmatch(name):
            raise Refused(f"secrets/.env line {number} is not NAME=VALUE")
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        elif " #" in value:
            value = value.split(" #", 1)[0].rstrip()
        values[name] = value
    return values


def git_tracked(repo: Path, rel: str) -> list[str]:
    """Tracked paths at or under rel in repo. Raises Refused when git fails."""
    try:
        result = subprocess.run(["git", "-C", str(repo), "ls-files", "-z", "--", rel],
                                capture_output=True, check=True)
    except (OSError, subprocess.CalledProcessError) as error:
        raise Refused(f"cannot ask git what is tracked in {repo}") from error
    return [name for name in result.stdout.decode("utf-8", "surrogateescape").split("\0") if name]


def private_mode(path: Path, label: str) -> None:
    mode = path.stat().st_mode
    if mode & (stat.S_IRWXG | stat.S_IRWXO):
        raise Refused(f"{label} is open to group or other (mode {stat.S_IMODE(mode):03o}), "
                      f"run chmod {'700' if stat.S_ISDIR(mode) else '600'} {label}")


def check_key(key: Path, label: str, bits: int) -> None:
    """The key must be an unencrypted RSA PEM private key of the scheme size."""
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import rsa

    try:
        loaded = serialization.load_pem_private_key(key.read_bytes(), password=None)
    except TypeError:
        raise Refused(f"{label} is password protected, espsecure needs an unencrypted PEM") from None
    except ValueError:
        raise Refused(f"{label} is not a PEM private key") from None
    if not isinstance(loaded, rsa.RSAPrivateKey) or loaded.key_size != bits:
        raise Refused(f"{label} is not an RSA-{bits} private key")


def load_config(repo: Path) -> tuple[Path, str, str]:
    """Check secrets/ and return the key path, its printable label and the scheme."""
    secrets = repo / "secrets"
    env_file = secrets / ".env"
    if not env_file.is_file():
        raise Refused("secrets/.env is missing, see docs/shared/development.md#local-app-signing")
    tracked = git_tracked(repo, "secrets")
    if tracked:
        raise Refused(f"git tracks {len(tracked)} path(s) under secrets/, untrack them and rotate the key")
    private_mode(secrets, "secrets")
    private_mode(env_file, "secrets/.env")
    values = parse_env(env_file.read_text())
    name = values.get(KEY_VAR, "")
    if not name:
        raise Refused(f"secrets/.env does not set {KEY_VAR}")
    scheme = values.get(SCHEME_VAR, "") or DEFAULT_SCHEME
    if scheme not in SCHEMES:
        raise Refused(f"{SCHEME_VAR} must be one of {', '.join(sorted(SCHEMES))} for ESP32-S3")
    secrets_real = secrets.resolve()
    key = (secrets / name).resolve()
    if not key.is_relative_to(secrets_real) or key == secrets_real:
        raise Refused(f"{KEY_VAR} must name a file inside secrets/")
    label = f"secrets/{key.relative_to(secrets_real).as_posix()}"
    if not key.is_file():
        raise Refused(f"{label} does not exist")
    private_mode(key, label)
    check_key(key, label, SCHEMES[scheme])
    return key, label, scheme


def espsecure(args: list[str], key: Path, label: str) -> None:
    """Run espsecure quietly. On failure show its output with the key path
    replaced by its label. espsecure never prints key material."""
    result = subprocess.run(ESPSECURE + args, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        output = (result.stdout + result.stderr).replace(str(key), label).strip()
        raise Refused(f"espsecure {args[0]} failed (exit {result.returncode})\n{output}")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def sign(repo: Path, out: Path, images: list[Path]) -> list[str]:
    secrets_real = (repo / "secrets").resolve()
    out_real = out.resolve()
    if out_real.is_relative_to(secrets_real):
        raise Refused("the output directory must not be inside secrets/")
    names = {}
    for image in images:
        if not image.is_file():
            raise Refused(f"{image} is not a file")
        if image.resolve().is_relative_to(secrets_real):
            raise Refused(f"input {image.name} is inside secrets/, sign a copy from the build directory")
        signed = f"{image.stem}.signed.bin"
        if signed in names:
            raise Refused(f"{image} and {names[signed]} would both write {signed}")
        names[signed] = image
    key, label, scheme = load_config(repo)
    out_real.mkdir(parents=True, exist_ok=True)
    print(f"signing {len(images)} image(s) with {label} ({scheme}, Secure Boot v2)")
    lines = []
    for signed, image in names.items():
        target = out_real / signed
        target.unlink(missing_ok=True)
        try:
            espsecure(["sign-data", "--version", "2", "--keyfile", str(key), "--output", str(target),
                       str(image)], key, label)
            espsecure(["verify-signature", "--version", "2", "--keyfile", str(key), str(target)], key, label)
        except Refused:
            target.unlink(missing_ok=True)
            raise
        lines.append(f"{sha256(target)}  {signed}")
        print(f"signed and verified {target}")
    sums = out_real / SUMS
    sums.write_text("".join(f"{line}\n" for line in sorted(lines, key=lambda line: line.split("  ", 1)[1])))
    print(f"wrote {sums}")
    return lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, required=True, help="directory for the signed images and SHA256SUMS")
    parser.add_argument("--repo", type=Path, default=ROOT, help=argparse.SUPPRESS)
    parser.add_argument("images", type=Path, nargs="+", help="unsigned app .bin files")
    args = parser.parse_args(argv)
    try:
        sign(args.repo, args.out, args.images)
    except Refused as error:
        print(f"release-sign: refused, {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
