"""Exercise three independent LAN collectors against one real sensor.

Read-only: never configures, flushes, deletes or rewrites the sensor archive.
Keep the token in an ignored private file; only nonsecret results are printed.
Run through pixi with --host aq-xxxx.local --token-file <private-path> --out <artifacts-path>.
This qualifies host TCP clients, not Android/iOS radio or background behavior.
"""
from __future__ import annotations

import argparse
import asyncio
import binascii
import hashlib
import json
from pathlib import Path
import time

from ble_sync import LanLink, ProtocolError, Session
from parquet_device import output_destination, validate_file


async def run(args: argparse.Namespace) -> dict:
    token = bytes.fromhex(args.token_file.read_text().strip())
    links = [LanLink(args.host, token, timeout=30) for _ in range(3)]
    sessions = [Session(link) for link in links]
    challenger = LanLink(args.host, token, timeout=10)
    try:
        for link, session in zip(links, sessions):
            await link.connect()
            await session.start()
        infos = await asyncio.gather(*(session.info() for session in sessions))
        if any(info != infos[0] for info in infos):
            raise ProtocolError("collectors disagree on sensor identity")
        try:
            await challenger.connect()
        except ProtocolError as error:
            if "refused code=7" not in str(error):
                raise
        else:
            raise ProtocolError("fourth collector was accepted by a full pool")
        listed, _ = await sessions[0].list_files()
        candidates = [(name, size) for name, size in listed.items() if 12 <= size <= 32 * 1024 * 1024]
        if not candidates:
            raise ProtocolError("no bounded finalized file available for concurrency check")
        name, _ = max(candidates, key=lambda item: item[1])
        started = time.monotonic()
        downloads = await asyncio.gather(*(session.fetch(name, 4096) for session in sessions))
        elapsed = time.monotonic() - started
        payload = downloads[0][0]
        if any(data != payload for data, _ in downloads):
            raise ProtocolError("concurrent downloads differ")
        # Drop one client with other handles still open, then keep collecting.
        opened = await asyncio.gather(*(session.open(name) for session in sessions))
        await links[0].disconnect()
        retained = await asyncio.gather(*(
            sessions[i].read_window(opened[i][0], 0, min(4096, opened[i][1])) for i in (1, 2)
        ))
        if any(window != payload[:len(window)] for window in retained):
            raise ProtocolError("surviving client lost its file after another disconnected")
        for i in (1, 2):
            await sessions[i].close(opened[i][0])
        destination = output_destination(args.out, name)
        if destination.exists():
            if destination.read_bytes() != payload:
                raise ProtocolError("refusing to replace different local archive bytes")
        else:
            with destination.open("xb") as file:
                file.write(payload)
        inspected = validate_file(destination)
        return {
            "firmware": infos[0].get("fw"), "station": infos[0].get("station"),
            "clients": 3, "fourth_busy": True, "disconnect_isolated": True,
            "name": name, "bytes": len(payload), "crc32": f"{binascii.crc32(payload) & 0xffffffff:08x}",
            "sha256": hashlib.sha256(payload).hexdigest(), "seconds": round(elapsed, 3),
            "transfers": [details for _, details in downloads],
            "rows": inspected["rows"], "readers_match": inspected["readers_match"],
        }
    finally:
        await challenger.disconnect()
        for link in links:
            await link.disconnect()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--token-file", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(asyncio.run(run(args)), indent=2))


if __name__ == "__main__":
    main()
