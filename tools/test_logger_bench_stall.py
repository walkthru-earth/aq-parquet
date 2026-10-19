"""Bench stall command, parser bounds, deadline and the default-off build guard."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from host_cxx import CXX_STD

GUARD = "CONFIG_AQ_LOGGER_BENCH_STALL"
OVERLAY = "tools/idf-bench-stall-sdkconfig.defaults"


def guarded_lines(source: str) -> list[tuple[str, bool]]:
    """Each line with whether it sits inside `#if CONFIG_AQ_LOGGER_BENCH_STALL`."""
    stack: list[bool] = []
    out = []
    for line in source.splitlines():
        directive = line.strip()
        if directive.startswith("#if"):
            stack.append(directive == f"#if {GUARD}")
        elif directive.startswith("#endif"):
            assert stack, "unbalanced #endif in aq_logger.cpp"
            stack.pop()
        elif directive.startswith(("#else", "#elif")):
            assert not (stack and stack[-1]), f"{GUARD} blocks must not have an #else branch"
        out.append((line, any(stack)))
    assert not stack, "unbalanced #if in aq_logger.cpp"
    return out


def check_wiring(root: Path) -> None:
    """The command exists only in images built with the bench overlay."""
    logger = root / "firmware/common/logger"
    kconfig = (logger / "Kconfig").read_text()
    entry = re.search(r"config AQ_LOGGER_BENCH_STALL\n(.*?)(?=\nconfig |\nendmenu)", kconfig, re.S)
    assert entry, "Kconfig lost AQ_LOGGER_BENCH_STALL"
    assert re.search(r"^\s+default n$", entry.group(1), re.M), "AQ_LOGGER_BENCH_STALL must default to n"

    lines = guarded_lines((logger / "src/aq_logger.cpp").read_text())
    bench = [(line, inside) for line, inside in lines
             if "aq_logger_bench_stall.h" in line or "bench::" in line or "bench_stall" in line]
    assert bench, "aq_logger.cpp no longer uses the bench stall helpers"
    outside = [line.strip() for line, inside in bench if not inside]
    assert not outside, f"bench stall code outside #if {GUARD}: {outside}"
    code = "\n".join(line for line, inside in lines if inside)
    for call in ("bench::parse_stall_seconds(", "bench::stall_continues(", "bench::is_stall_command("):
        assert call in code, f"aq_logger.cpp must call {call} under {GUARD}"
    # The stall must not stamp, or the watchdog never sees it.
    assert "stamp_heartbeat" not in code, "the bench stall must not stamp the heartbeat"

    # No shipped defaults turn it on, only the bench overlay.
    shipped = [root / "tools/idf-sdkconfig.defaults", root / "tools/idf-connectivity-sdkconfig.defaults",
               *root.glob("firmware/**/sdkconfig.defaults")]
    for path in shipped:
        if "/build/" in str(path) or "managed_components" in str(path):
            continue
        assert GUARD not in path.read_text(), f"{path.relative_to(root)} must not set {GUARD}"
    overlay = (root / OVERLAY).read_text().splitlines()
    assert f"{GUARD}=y" in overlay, f"{OVERLAY} must set {GUARD}=y"
    settings = [line for line in overlay if line.strip() and not line.startswith("#")]
    assert settings == [f"{GUARD}=y"], f"{OVERLAY} must only turn on the bench stall"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    logger = root / "firmware/common/logger"
    check_wiring(root)
    with tempfile.TemporaryDirectory(prefix="aq-bench-stall-") as directory:
        executable = Path(directory) / "bench-stall-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                   "-I", str(logger / "src"),
                   str(logger / "tests/test_bench_stall.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("Bench stall: seconds parser bounds and malformed input, forever and returning deadline, "
          "default-off Kconfig, guarded wiring and the bench-only overlay passed")


if __name__ == "__main__":
    main()
