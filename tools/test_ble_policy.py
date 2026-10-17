"""Check BLE notify credits, stall clock, advert window, interval mode, link limits and link auth gates.

Host checks of the pure link policy only; no radio, host stack or central behavior
is established.
Run: pixi run ble-policy-test
"""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile
from host_cxx import CXX_STD


def gate_body(source: str, name: str) -> str:
    """The body of the ble_sync.cpp function `name`, by brace matching."""
    start = source.index("{", source.index(f" {name}("))
    depth = 0
    for index in range(start, len(source)):
        depth += {"{": 1, "}": -1}.get(source[index], 0)
        if depth == 0:
            return source[start:index + 1]
    raise AssertionError(f"unbalanced {name}")


def check_gate_wiring(source: str) -> None:
    """Issue #51. Every response gate asks may_send_frame(), which falls back
    to NimBLE's live state. Refusing on the cached flag alone fails here."""
    for name in ("notify_on_host", "drain_responses", "enqueue_response"):
        body = gate_body(source, name)
        assert "may_send_frame(" in body, f"{name} must gate with may_send_frame"
        assert "authenticated.load()" not in body, f"{name} must not gate on the cached flag"
    body = gate_body(source, "may_send_frame")
    assert "may_send(authenticated.load()" in body and "secure(handle)" in body, body


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    connectivity = root / "firmware/common/connectivity/src"
    check_gate_wiring((connectivity / "ble_sync.cpp").read_text())
    with tempfile.TemporaryDirectory(prefix="aq-ble-policy-test-") as directory:
        executable = Path(directory) / "ble-policy-test"
        command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror",
                   "-I", str(connectivity),
                   "-I", str(root / "firmware/common/runtime/src"),
                   str(root / "tools/test_ble_policy.cpp"),
                   str(connectivity / "ble_policy.cpp"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("BLE policy: credits Ok/Full/Closed, refused/failed, stale generation, "
          "15 s stall clock, cancel, response ring order and wrap, reopen epoch "
          "and resubscribe, EALREADY stall close, boot flag baseline, advert "
          "window, interval mode, link limits, stale auth flag served, insecure link "
          "ERROR 14 fallback, no auth code 7, gate table and gate wiring passed")


if __name__ == "__main__":
    main()
