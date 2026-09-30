"""Offline publication tests for backup-flash.sh; no serial device is opened."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


BOARD = "waveshare-sim7670g-v2"
STAMP = "20260930T123456Z"


def make_image(path: Path, marker: bytes) -> None:
    with path.open("wb") as stream:
        stream.write(marker)
        stream.truncate(16_777_216)


def run_case(script: Path, fake_bin: Path, mode: str, source: Path) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment.update(
        PATH=f"{fake_bin}{os.pathsep}{environment['PATH']}",
        AQ_FAKE_ESPTOOL_MODE=mode,
        AQ_FAKE_SOURCE=str(source),
    )
    return subprocess.run(
        [str(script), "--board", BOARD, "--port", "/dev/fake-checked-port"],
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )


def main() -> None:
    source_script = Path(__file__).with_name("backup-flash.sh")
    with tempfile.TemporaryDirectory(prefix="aq-backup-test-") as directory:
        root = Path(directory)
        tools = root / "tools"
        tools.mkdir()
        script = tools / "backup-flash.sh"
        shutil.copy2(source_script, script)
        fake_bin = root / "fake-bin"
        fake_bin.mkdir()
        (fake_bin / "date").write_text(f"#!/bin/sh\nprintf '%s\\n' {STAMP}\n")
        (fake_bin / "python").write_text(
            "#!/bin/sh\n"
            "for arg do output=$arg; done\n"
            "case \"$AQ_FAKE_ESPTOOL_MODE\" in\n"
            "  fail) printf partial > \"$output\"; exit 8 ;;\n"
            "  short) printf short > \"$output\"; exit 0 ;;\n"
            "  complete) cp \"$AQ_FAKE_SOURCE\" \"$output\" ;;\n"
            "  *) exit 9 ;;\n"
            "esac\n"
        )
        for name in ("date", "python"):
            (fake_bin / name).chmod(0o755)

        first = root / "first.bin"
        second = root / "second.bin"
        make_image(first, b"original")
        make_image(second, b"changed")
        backup = root / "backup"
        published = backup / f"{BOARD}-flash-{STAMP}.bin"

        for mode in ("fail", "short"):
            result = run_case(script, fake_bin, mode, first)
            assert result.returncode != 0, result
            assert not published.exists(), result
            assert not list(backup.glob(".*")), list(backup.iterdir())

        result = run_case(script, fake_bin, "complete", first)
        assert result.returncode == 0, result
        assert published.stat().st_size == 16_777_216
        original_hash = hashlib.sha256(first.read_bytes()).hexdigest()
        assert hashlib.sha256(published.read_bytes()).hexdigest() == original_hash
        assert original_hash in result.stdout and "backup verified" in result.stdout
        assert not list(backup.glob(".*")), list(backup.iterdir())

        collision = run_case(script, fake_bin, "complete", second)
        assert collision.returncode != 0, collision
        assert "already exists" in collision.stderr, collision
        assert hashlib.sha256(published.read_bytes()).hexdigest() == original_hash
        assert not list(backup.glob(".*")), list(backup.iterdir())
    print("PASS backup publication: failed, short, verified and same-name collision readbacks")


if __name__ == "__main__":
    main()
