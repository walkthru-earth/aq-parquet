#!/usr/bin/env python3
"""Copy Wi-Fi settings between two owner-controlled AQLogger USB consoles.

Only SSID, PSK and Wi-Fi/LAN enable flags are copied. Pairing PINs, bearer
tokens and station identities remain per device. Credentials are held in
memory only: never printed, logged, saved or placed in command-line arguments.
"""
from __future__ import annotations

import argparse
import re
import time

import parquet_device as device


def read_profile(port_name: str, timeout: float) -> bytes:
    with device.open_port(port_name, 115200) as port:
        device.send_command(port, "parquet wifi-profile")
        for line in device.lines_until(port, time.monotonic() + timeout):
            match = re.fullmatch(
                r"AQ WIFI_PROFILE ssid=([0-9a-f]*) psk=([0-9a-f]*) "
                r"wifi_on=([01]) lan_on=([01])", line)
            if not match:
                continue
            try:
                ssid = bytes.fromhex(match[1])
                psk = bytes.fromhex(match[2])
            except ValueError:
                raise SystemExit("Malformed source Wi-Fi profile") from None
            if len(ssid) > 32 or len(psk) > 63 or any(
                    char in ssid + psk for char in (b"\0", b"\r", b"\n")):
                raise SystemExit("Source profile exceeds supported credential bounds")
            return (b"wifi.ssid=" + ssid + b"\nwifi.psk=" + psk +
                    b"\nwifi.on=" + match[3].encode() +
                    b"\nlan.on=" + match[4].encode())
    raise SystemExit("No source Wi-Fi profile before the deadline")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-port", required=True)
    parser.add_argument("--port", required=True, help="destination logger")
    parser.add_argument("--timeout", type=device.positive_seconds, default=10.0)
    args = parser.parse_args()
    if args.source_port == args.port:
        raise SystemExit("Source and destination must be different devices")
    profile = read_profile(args.source_port, args.timeout)
    with device.open_port(args.port, 115200) as port:
        device.send_command(port, "parquet config-hex " + profile.hex())
        for line in device.lines_until(port, time.monotonic() + args.timeout):
            if line.startswith("AQ CONFIG ok=1"):
                print("Copied Wi-Fi settings over USB; per-device identities and secrets retained.")
                return
            if line.startswith("AQ CONFIG ok=0"):
                raise SystemExit("Destination rejected the Wi-Fi settings")
    raise SystemExit("No destination acknowledgement before the deadline")


if __name__ == "__main__":
    main()
