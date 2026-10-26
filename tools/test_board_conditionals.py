"""Host tests for check_board_conditionals.py and the way tools/lint.sh reads its exit code."""

from __future__ import annotations

import contextlib
import io
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile

import check_board_conditionals as bc

FIRES = [
    "#if 0\n#elif defined(CONFIG_OS_BOARD_CORES3)\n#endif\n",
    "#if !defined(WAVESHARE_X)\n#endif\n",
    "#  ifdef m5stack_core\n#  endif\n",
    "#ifndef OS_CORE_S3_PINS\n#endif\n",
    "#if CONFIG_IDF_TARGET == \"esp32s3\"\n#endif\n",
    "#if 0\n#elif CONFIG_OS_SIM7670G_UART\n#endif\n",
    "#if defined(A) || \\\n    CONFIG_IDF_TARGET_ESP32S3\n#endif\n",
    "#if defined(A) /* a block comment\n   over two lines */ || defined(M5_CORE_X)\n#endif\n",
]
QUIET = [
    "#if CONFIG_IDF_TARGET_ARCH_XTENSA\n#endif\n",
    "#if CONFIG_SOC_UART_NUM > 2\n#endif\n",
    "#if CONFIG_IDF_TARGET_LINUX\n#endif\n",
    "#ifdef CONFIG_OS_SCORE_S\n#endif\n",
    "#ifndef OS_SHARED_H\n#define OS_SHARED_H\n#endif\n",
    "#if CONFIG_OS_LOGGER_WORKER_WDT // not for CORES3\n#endif // CORES3\n",
    "#define CORES3 1\nint waveshare = 0;\n",
    "#if A // a comment that ends in a backslash \\\n   || WAVESHARE\n#endif\n",
    "const char *r = R\"(\n#if CORES3\n)\";\n",
]


def check(root: Path, text: str | bytes) -> list[str]:
    path = root / "common" / "x.cpp"
    path.write_bytes(text if isinstance(text, bytes) else text.encode())
    return bc.findings([root / "common"])


def check_findings(root: Path) -> None:
    (root / "common" / "vendor" / "lib").mkdir(parents=True)
    for text in FIRES:
        assert len(check(root, text)) == 1, text
    for text in QUIET:
        assert check(root, text) == [], text
    # CRLF line ends, also on a continuation, read as LF.
    assert len(check(root, b"#if defined(A) || \\\r\n    defined(OS_WAVESHARE)\r\n#endif\r\n")) == 1
    # The finding names the line the directive starts on, after a block
    # comment and a splice that each took up a line before it.
    found = check(root, "/* one\n two */\nint a = 1; \\\n\n#elif CORES3\n")
    assert found == [f"{root / 'common' / 'x.cpp'}:5: #elif CORES3"], found
    # Third-party vendor code and other file types are skipped.
    check(root, "")
    (root / "common" / "vendor" / "lib" / "y.h").write_text("#if CONFIG_IDF_TARGET_ESP32S3\n#endif\n")
    (root / "common" / "notes.txt").write_text("#if CORES3\n")
    assert bc.findings([root / "common"]) == []
    (root / "common" / "notes.txt").unlink()


def check_main(root: Path) -> None:
    def run(args: list[str]) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = bc.main(args)
        return code, out.getvalue(), err.getvalue()

    check(root, "")
    assert run([str(root / "common")])[0] == 0
    check(root, "#if WAVESHARE\n#endif\n")
    code, out, err = run([str(root / "common")])
    assert code == 1 and out.strip().endswith("x.cpp:1: #if WAVESHARE") and "board-name" in err, (out, err)
    # A missing directory or an unknown flag is an error, never a pass.
    code, _, err = run([str(root / "missing")])
    assert code == 2 and "is not a directory" in err, err
    assert run(["--all"])[0] == 2
    # The committed shared firmware is clean.
    assert run([])[0] == 0


def check_lint_exit_codes(root: Path) -> None:
    """lint.sh passes only on exit 0 from the check. A fake python returns a
    chosen code for the check and runs the real one for everything else, and a
    fake cppcheck passes, so only the board check decides."""
    bin_dir = root / "bin"
    bin_dir.mkdir()
    fake_python = bin_dir / "python"
    fake_python.write_text("#!/usr/bin/env bash\n"
                           'if [[ "$1" == tools/check_board_conditionals.py ]]; then exit "${FAKE_BOARD_RC}"; fi\n'
                           f'exec "{sys.executable}" "$@"\n')
    fake_cppcheck = bin_dir / "cppcheck"
    fake_cppcheck.write_text("#!/usr/bin/env bash\nexit 0\n")
    for path in (fake_python, fake_cppcheck):
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
    lint = Path(bc.ROOT) / "tools" / "lint.sh"
    for rc, expected in ((0, 0), (1, 1), (2, 1), (127, 1)):
        env = dict(os.environ, PATH=f"{bin_dir}{os.pathsep}{os.environ['PATH']}", FAKE_BOARD_RC=str(rc))
        result = subprocess.run(["bash", str(lint)], env=env, capture_output=True, text=True, check=False)
        assert result.returncode == expected, (rc, result.returncode, result.stderr)
        if rc not in (0, 1):
            assert f"board conditional check failed to run (exit {rc})" in result.stderr, result.stderr


def main() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        check_findings(root)
        check_main(root)
        check_lint_exit_codes(root)
    print("check_board_conditionals tests passed")


if __name__ == "__main__":
    main()
