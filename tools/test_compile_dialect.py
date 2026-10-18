"""Host tests for the fixture dialect check, `check_idf_target.py --compile-dialect`.

The SDK fixtures (logger compile test, connectivity and PSRAM probe) have no
board target, so they run only the per-file -std check on their compiler
database. This test feeds that check crafted databases and makes sure every
fixture entry point calls it. It needs no SDK.
"""

from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools/check_idf_target.py"
PINNED = re.search(r"-std=gnu\+\+\d+", (ROOT / "firmware/cmake/aq_cxx_standard.cmake").read_text())[0]
# The ESP-IDF 6.1 default. Spelled in two parts because check_cxx_dialect_sources
# rejects a literal C++ -std in a tools/test_*.py.
SDK_DEFAULT = "-std=gnu++" + "26"
REPO_CPP = ROOT / "tools/fixtures/connectivity_compile/main.cpp"
REPO_C = ROOT / "firmware/common/vendor/lz4/lz4.c"
# Every SDK fixture build must run the check on its own build directory.
FIXTURE_ENTRY_POINTS = (
    ("tools/test_logger_build.sh", 'fixture="${repo_root}/firmware/common/logger/tests/compile"',
     '--compile-dialect "${fixture}/build"'),
    ("tools/build_connectivity_fixture.sh", 'fixture="${repo_root}/tools/fixtures/connectivity_compile"',
     '--compile-dialect "${fixture}/build"'),
    ("pixi.toml", "waveshare-psram-build = ",
     "--compile-dialect tools/fixtures/waveshare_psram_probe/build"),
)


def entry(source: Path, *flags: str) -> dict[str, str]:
    return {"directory": "/tmp", "file": str(source),
            "command": " ".join(["xtensa-esp32s3-elf-g++", *flags, "-c", str(source)])}


def run(build: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(SCRIPT), "--compile-dialect", str(build)],
                          capture_output=True, text=True, timeout=60, check=False)


def main() -> None:
    for relative, anchor, call in FIXTURE_ENTRY_POINTS:
        text = (ROOT / relative).read_text()
        assert anchor in text, f"{relative} no longer has {anchor}"
        # For pixi.toml only the PSRAM task line counts.
        lines = [line for line in text.splitlines() if anchor in line or relative != "pixi.toml"]
        assert any("tools/check_idf_target.py" in line and call in line for line in lines), \
            f"{relative} must run check_idf_target.py {call}"

    cases = {
        "pinned": ([entry(REPO_CPP, SDK_DEFAULT, PINNED)], True),
        "pinned with a C file": ([entry(REPO_CPP, PINNED), entry(REPO_C, "-std=gnu17")], True),
        "SDK default wins": ([entry(REPO_CPP, PINNED, SDK_DEFAULT)], False),
        "no std": ([entry(REPO_CPP, "-Og")], False),
        "C file with a C++ std": ([entry(REPO_CPP, PINNED), entry(REPO_C, PINNED)], False),
        "only third party": ([entry(Path("/opt/esp/idf/components/log/log.cpp"), SDK_DEFAULT)], False),
        "empty database": ([], False),
    }
    with tempfile.TemporaryDirectory() as raw:
        for name, (commands, ok) in cases.items():
            build = Path(raw) / name.replace(" ", "-")
            build.mkdir()
            (build / "compile_commands.json").write_text(json.dumps(commands))
            result = run(build)
            if ok:
                assert result.returncode == 0, f"{name}: {result.stderr}"
                assert PINNED in result.stdout, name
            else:
                assert result.returncode != 0, f"{name}: {result.stdout}"
                assert "native target rejected" in result.stderr, f"{name}: {result.stderr}"
        missing = Path(raw) / "missing"
        missing.mkdir()
        result = run(missing)
        assert result.returncode != 0 and "compile_commands.json" in result.stderr, result.stderr

    print("check_idf_target.py --compile-dialect host tests passed")


if __name__ == "__main__":
    main()
