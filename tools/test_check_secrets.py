"""Host tests for check_secrets.py and the way tools/lint.sh reads its exit code.

Every repository here is a throwaway git index in a temp dir. The real
secrets/ directory is never touched.
"""

from __future__ import annotations

import contextlib
import io
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile

import check_secrets as cs

# Built from parts so this file does not trip the check it tests.
HEADERS = [f"-----BEGIN {kind}PRIVATE KEY-----" for kind in ("", "RSA ", "EC ", "ENCRYPTED ", "OPENSSH ")]
QUIET = [
    "-----BEGIN PUBLIC KEY-----\n",
    "-----BEGIN CERTIFICATE-----\n",
    "PRIVATE KEY without the dashes\n",
    "-----BEGIN (?:[A-Z0-9]+ )*PRIVATE KEY-----\n",
]


def git(repo: Path, *args: str) -> None:
    subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True)


def run(args: list[str]) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = cs.main(args)
    return code, out.getvalue(), err.getvalue()


def fresh_repo(root: Path, name: str) -> Path:
    repo = root / name
    repo.mkdir()
    git(repo, "init", "-q")
    (repo / "README.md").write_text("clean\n")
    git(repo, "add", "README.md")
    return repo


def check_findings(root: Path) -> None:
    repo = fresh_repo(root, "findings")
    assert cs.findings(repo) == []
    assert run(["--root", str(repo)])[0] == 0

    for index, header in enumerate(HEADERS):
        name = f"leak{index}.txt"
        body = f"x\n{header}\nSECRETBODY{index}\n"
        (repo / name).write_text(body)
        git(repo, "add", name)
        code, out, err = run(["--root", str(repo)])
        assert code == 1 and f"{name}: holds a PEM private key header" in out, (header, out)
        # Paths only. The file body never reaches the output.
        assert "SECRETBODY" not in out + err and "BEGIN" not in out + err, (out, err)
        git(repo, "rm", "-q", "--cached", name)
        (repo / name).unlink()

    for index, body in enumerate(QUIET):
        name = f"quiet{index}.txt"
        (repo / name).write_text(body)
        git(repo, "add", name)
    assert cs.findings(repo) == [], cs.findings(repo)

    # A header in a binary file still fires.
    (repo / "blob.bin").write_bytes(b"\x00\xff" + HEADERS[1].encode() + b"\x00")
    git(repo, "add", "blob.bin")
    assert cs.findings(repo) == ["blob.bin: holds a PEM private key header"]
    git(repo, "rm", "-q", "--cached", "blob.bin")

    # Anything tracked under secrets/ fires, even a harmless file, and even
    # when it was force added past the ignore rule.
    (repo / ".gitignore").write_text("/secrets/\n")
    (repo / "secrets").mkdir()
    (repo / "secrets" / "notes.txt").write_text("nothing\n")
    git(repo, "add", "-f", "secrets/notes.txt")
    assert cs.findings(repo) == ["secrets/notes.txt: tracked under secrets/"]
    git(repo, "rm", "-q", "--cached", "secrets/notes.txt")
    assert cs.findings(repo) == []
    # An ignored, untracked secrets/ is fine. A nested dir named secrets is not
    # the owner directory.
    (repo / "docs" / "secrets").mkdir(parents=True)
    (repo / "docs" / "secrets" / "a.md").write_text("doc\n")
    git(repo, "add", "docs/secrets/a.md")
    assert cs.findings(repo) == []

    # The staged blob is what gets committed. A key staged and then deleted or
    # replaced on disk still fires, and a clean file staged and then given a
    # key on disk does not.
    (repo / "gone.txt").write_text(HEADERS[0])
    git(repo, "add", "gone.txt")
    (repo / "gone.txt").unlink()
    assert cs.findings(repo) == ["gone.txt: holds a PEM private key header"]
    (repo / "gone.txt").write_text("clean now\n")
    assert cs.findings(repo) == ["gone.txt: holds a PEM private key header"]
    git(repo, "add", "gone.txt")
    assert cs.findings(repo) == []
    (repo / "later.txt").write_text("clean\n")
    git(repo, "add", "later.txt")
    (repo / "later.txt").write_text(HEADERS[0])
    assert cs.findings(repo) == []
    # An executable file is read too.
    (repo / "run.sh").write_text(HEADERS[2])
    (repo / "run.sh").chmod(0o755)
    git(repo, "add", "run.sh")
    assert cs.findings(repo) == ["run.sh: holds a PEM private key header"]
    git(repo, "rm", "-q", "--cached", "run.sh")
    # A symlink entry is never followed, even to a key.
    outside = root / "outside.pem"
    outside.write_text(HEADERS[0])
    (repo / "link.pem").symlink_to(outside)
    git(repo, "add", "link.pem")
    assert cs.findings(repo) == []


def check_errors(root: Path) -> None:
    plain = root / "not-a-repo"
    plain.mkdir()
    code, _, err = run(["--root", str(plain)])
    assert code == 2 and "cannot list tracked files" in err, err
    assert run(["--bogus"])[0] == 2
    # The committed tree is clean.
    assert run([])[0] == 0


def check_lint_exit_codes(root: Path) -> None:
    """lint.sh passes only on exit 0 from the check. A fake python returns a
    chosen code for the check and runs the real one for everything else, and a
    fake cppcheck passes, so only the secrets check decides."""
    bin_dir = root / "bin"
    bin_dir.mkdir()
    fake_python = bin_dir / "python"
    fake_python.write_text("#!/usr/bin/env bash\n"
                           'if [[ "$1" == tools/check_secrets.py ]]; then exit "${FAKE_SECRETS_RC}"; fi\n'
                           f'exec "{sys.executable}" "$@"\n')
    fake_cppcheck = bin_dir / "cppcheck"
    fake_cppcheck.write_text("#!/usr/bin/env bash\nexit 0\n")
    for path in (fake_python, fake_cppcheck):
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
    lint = Path(cs.ROOT) / "tools" / "lint.sh"
    for rc, expected in ((0, 0), (1, 1), (2, 1), (127, 1)):
        env = dict(os.environ, PATH=f"{bin_dir}{os.pathsep}{os.environ['PATH']}", FAKE_SECRETS_RC=str(rc))
        result = subprocess.run(["bash", str(lint)], env=env, capture_output=True, text=True, check=False)
        assert result.returncode == expected, (rc, result.returncode, result.stderr)
        if rc not in (0, 1):
            assert f"tracked secrets check failed to run (exit {rc})" in result.stderr, result.stderr


def main() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        check_findings(root)
        check_errors(root)
        check_lint_exit_codes(root)
    print("check_secrets tests passed")


if __name__ == "__main__":
    main()
