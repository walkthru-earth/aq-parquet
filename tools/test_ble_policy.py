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
    body = gate_body(source, "notify_on_host")
    assert "may_send_frame(" in body, "notify_on_host must gate with may_send_frame"
    assert "authenticated.load()" not in body, "notify_on_host must not gate on the cached flag"
    for name in ("drain_responses", "enqueue_response"):
        body = gate_body(source, name)
        assert "response_gate(" in body, f"{name} must gate with response_gate"
        assert "authenticated.load()" not in body, f"{name} must not gate on the cached flag"
    body = gate_body(source, "response_gate")
    assert "may_send_frame(" in body and "subscribed[Response]" in body, body
    assert "BLE REFUSE reason=" in body and "refusal_is_fault(" in body, body
    body = gate_body(source, "may_send_frame")
    assert "may_send(authenticated.load()" in body and "secure(handle)" in body, body


def case_body(source: str, event: str) -> str:
    """The gap_event case for `event`, up to the next case label."""
    start = source.index(f"case {event}:")
    return source[start:source.index("  case BLE_GAP_EVENT_", start + 1)]


def check_early_subscribe_wiring(source: str) -> None:
    """Issue #51, bonded reconnect. NimBLE can restore a bonded CCCD before it
    reports CONNECT. SUBSCRIBE must hold it and CONNECT must apply it after it
    clears the old link's state, through the same path as a live write."""
    body = case_body(source, "BLE_GAP_EVENT_SUBSCRIBE")
    assert "route_subscribe(" in body and "early_cccd.hold(" in body, body
    assert "apply_subscribe(" in body, body
    body = case_body(source, "BLE_GAP_EVENT_CONNECT")
    assert "early_cccd.replay(" in body, "CONNECT must apply held subscriptions"
    fill = body.index("std::fill(std::begin(subscribed)")
    opened = body.index("responses.open(")
    replay = body.index("early_cccd.replay(")
    assert fill < opened < replay and "apply_subscribe(" in body[replay:], body
    assert body.count("early_cccd.clear()") == 2, "failed and refused CONNECT clear it"
    assert "early_cccd.clear()" in case_body(source, "BLE_GAP_EVENT_DISCONNECT")
    body = gate_body(source, "apply_subscribe")
    assert "response_subscribe(" in body and "subscribed[which] = notify" in body, body


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    connectivity = root / "firmware/common/connectivity/src"
    transport = (connectivity / "ble_sync.cpp").read_text()
    check_gate_wiring(transport)
    check_early_subscribe_wiring(transport)
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
          "ERROR 14 fallback, no auth code 7, subscribe restored before or with "
          "CONNECT, logged gate refusals, gate table and gate and subscribe "
          "wiring passed")


if __name__ == "__main__":
    main()
