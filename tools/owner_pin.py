#!/usr/bin/env python3
"""Read a headless logger's fixed pairing PIN over its physical serial port.

The PIN is printed only on explicit owner request. Never save it into captures,
repository files or cloud logs. This does not change settings or pairing mode.
"""
from __future__ import annotations

import argparse
import re
import time

import parquet_device as device


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--timeout", type=device.positive_seconds, default=8.0)
    args = parser.parse_args()
    with device.open_port(args.port, 115200) as port:
        device.send_command(port, "parquet owner-pin")
        for line in device.lines_until(port, time.monotonic() + args.timeout):
            match = re.fullmatch(r"AQ OWNER_PIN (\d{6})", line)
            if match:
                print(match.group(1))
                return
            if line.startswith("AQ OWNER_PIN ERROR"):
                raise SystemExit("This device uses its pairing display rather than a fixed PIN.")
    raise SystemExit("No owner PIN response before the deadline; check the logger and port.")


if __name__ == "__main__":
    main()
