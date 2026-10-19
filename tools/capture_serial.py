#!/usr/bin/env python3
"""Capture serial output from the board for a bounded time, then stop.

Non-interactive on purpose, so a diagnostic dump can be recorded and read back
without a human sitting on a terminal. Writes to stdout and optionally a file.

Usage
    pixi run capture --port /dev/cu.usbmodem101 --seconds 25
    pixi run capture --port /dev/cu.usbmodem101 --seconds 25 --out logs/boot.txt
    pixi run capture --port /dev/cu.usbmodem101 --until "DIAGNOSTIC COMPLETE"
    pixi run capture --port /dev/cu.wchusbserial5B901533371 --seconds 900 \
        --send-on "PARQUET ROW" "parquet bench-stall 0" --out artifacts/stall.log

Without --send-on nothing is written to the device except an optional reset
pulse, so this is safe to run against a board you care about.

--send-on PATTERN COMMAND waits on the same open port until PATTERN appears in
the output and then writes COMMAND and a newline, once. Repeat it to send
several commands in order, each one waiting for its own pattern after the
previous send. Opening the Waveshare CH343 console resets the board, so a
second open to send a command would reboot it. Commands that answer with
secrets (owner-pin, wifi-profile, config-hex) are refused, since everything
the board prints lands in the capture. Commands that destroy data
(erase-archive) are refused too, so a typo in a cue cannot wipe a card. A cue
command must be printable ASCII, since the firmware drops any other byte and
would run what is left.

Exit codes are 0 for a capture with every cue sent, 1 when the port cannot
open, 2 for a refused cue or an empty capture, 3 when a cue never fired and
130 when stopped with Ctrl-C. An interrupted capture still writes --out.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path
from typing import Iterable

try:
    import serial
except ImportError:
    print("pyserial is missing, run this through pixi", file=sys.stderr)
    raise SystemExit(1)


# Serial commands whose replies carry the device PIN, a Wi-Fi PSK or a decoded
# credential. A capture keeps everything the board prints, so never send them.
SECRET_COMMANDS = ("parquet owner-pin", "parquet wifi-profile", "parquet config-hex")

# Serial commands that destroy owner data. Never sent on a cue, whatever follows
# the command word, since one typo in a long bench run could wipe the archive.
DESTRUCTIVE_COMMANDS = ("parquet erase-archive",)

INTERRUPTED_EXIT = 130


def refusal(command: str) -> str | None:
    """Why a cue command is refused, or None. Case and spacing do not get past it."""
    words = " ".join(command.lower().split())
    for refused in SECRET_COMMANDS:
        if words == refused or words.startswith(refused + " "):
            return f"refusing to send {refused!r}, its reply is a secret"
    for refused in DESTRUCTIVE_COMMANDS:
        if words == refused or words.startswith(refused + " "):
            return f"refusing to send {refused!r}, it destroys data"
    return None


Cue = tuple[bytes, str]


class CueSender:
    """Sends each command once, after its pattern appears in the output.

    Cues run in order. Each pattern must appear after the point where the
    previous command was sent, so an earlier line never fires a later cue.
    """

    def __init__(self, cues: Iterable[tuple[str, str]]) -> None:
        self.pending: list[Cue] = [(pattern.encode(), command) for pattern, command in cues]
        for pattern, command in self.pending:
            if not pattern:
                raise ValueError("--send-on needs a non-empty pattern")
            if not command.strip() or "\n" in command or "\r" in command:
                raise ValueError("--send-on needs a single-line command")
            # The firmware console silently drops bytes outside 32 to 126, so
            # "erase-\tarchive" would reach it as erase-archive. Refuse them first.
            if not all(32 <= ord(char) <= 126 for char in command):
                # The text is not echoed, it may carry a credential.
                raise ValueError("refusing to send the cue command, it must be printable ASCII (32 to 126)")
            reason = refusal(command)
            if reason:
                raise ValueError(reason)
        self.start = 0

    def due(self, collected: bytes | bytearray) -> list[Cue]:
        """The next cue once its pattern is in collected, at most one per call."""
        fired: list[Cue] = []
        while self.pending:
            pattern = self.pending[0][0]
            found = collected.find(pattern, self.start)
            if found < 0:
                # Keep a tail so a pattern split across two reads still matches.
                self.start = max(self.start, len(collected) - len(pattern) + 1)
                break
            fired.append(self.pending.pop(0))
            # The next pattern must arrive after this send.
            self.start = len(collected)
        return fired

    def requeue(self, unsent: list[Cue], collected: bytes | bytearray) -> None:
        """Puts cues whose write failed back in front. Each waits for its pattern again."""
        self.pending[:0] = unsent
        self.start = len(collected)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True, help="serial device, for example /dev/cu.usbmodem101")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=20.0, help="hard upper bound on capture time")
    parser.add_argument("--until", default=None, help="stop early once this text appears")
    parser.add_argument("--out", default=None, help="also write the capture to this file")
    parser.add_argument("--reset", action="store_true", help="pulse DTR/RTS first to reboot the board")
    parser.add_argument("--send-on", nargs=2, action="append", default=[], metavar=("PATTERN", "COMMAND"),
                        help="once PATTERN appears, send COMMAND and a newline on the same port. Repeatable, "
                             "in order")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        cues = CueSender(args.send_on)
    except ValueError as exc:
        print(f"capture: {exc}", file=sys.stderr)
        return 2

    try:
        port = serial.Serial(args.port, args.baud, timeout=0.2)
    except Exception as exc:
        print(f"could not open {args.port}, {exc}", file=sys.stderr)
        return 1

    collected = bytearray()
    reconnecting = False
    interrupted = False
    try:
        if args.reset:
            # Standard esp32 auto-reset pulse. Leaves the chip running the app.
            port.setDTR(False)
            port.setRTS(True)
            time.sleep(0.2)
            port.setRTS(False)
            time.sleep(0.2)
        port.reset_input_buffer()

        deadline = time.monotonic() + args.seconds
        # The bound is always enforced, so a silent board cannot hang this.
        while time.monotonic() < deadline:
            try:
                chunk = port.read(4096)
            except (OSError, serial.SerialException):
                port.close()
                reconnecting = True
                while time.monotonic() < deadline:
                    try:
                        port = serial.Serial(args.port, args.baud, timeout=0.2)
                        print("\n[capture] serial device reconnected", file=sys.stderr)
                        break
                    except (OSError, serial.SerialException):
                        time.sleep(0.05)
                else:
                    break
                continue
            if chunk:
                collected += chunk
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()
                fired = cues.due(collected)
                for index, (_, command) in enumerate(fired):
                    try:
                        port.write(command.encode() + b"\n")
                        port.flush()
                    except (OSError, serial.SerialException) as exc:
                        # The read path reconnects. The cue waits for its pattern again.
                        cues.requeue(fired[index:], collected)
                        print(f"\n[capture] could not send {command!r}, {exc}", file=sys.stderr)
                        break
                    print(f"\n[capture] sent {command!r}", file=sys.stderr)
                if args.until and args.until.encode() in collected:
                    print(f"\n[capture] matched {args.until!r}, stopping early", file=sys.stderr)
                    break
    except KeyboardInterrupt:
        # A long bench capture stopped by hand still keeps what it read.
        interrupted = True
        print("\n[capture] interrupted, keeping what was read", file=sys.stderr)
    finally:
        port.close()

    text = collected.decode("utf-8", "replace")
    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text)
        print(f"\n[capture] {len(collected)} bytes to {out}", file=sys.stderr)

    if interrupted:
        return INTERRUPTED_EXIT
    if not collected:
        reconnect_note = " after USB re-enumeration" if reconnecting else ""
        print(
            f"\n[capture] nothing received{reconnect_note}. The board may be in the ROM download "
            "bootloader, which prints nothing, or the app may not be running.",
            file=sys.stderr,
        )
        return 2
    if cues.pending:
        waiting = ", ".join(repr(pattern.decode()) for pattern, _ in cues.pending)
        print(f"\n[capture] never saw {waiting}, those commands were not sent", file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
