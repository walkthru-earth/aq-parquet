"""Exercise independent LAN collectors against one real sensor.

Read-only: never configures, flushes, deletes or rewrites the sensor archive.
Keep the token in an ignored private file; only nonsecret results are printed.
Run through pixi with --host aq-xxxx.local --token-file <private-path> --out <artifacts-path>.
This qualifies host TCP clients, not Android/iOS radio or background behavior.
--throttle-seconds stops reading on the first collector after the concurrency
check, pipelines READs to it so its device ring fills, and requires that the
other collectors still complete LIST and READ while the throttled one ends with
BYE slow-peer (protocol 3, issue #2).
"""
from __future__ import annotations

import argparse
import asyncio
import binascii
import hashlib
import json
from pathlib import Path
import time

import struct

from ble_sync import OP_READ, LanLink, LinkClosedByDevice, ProtocolError, Session

SLOW_PEER = 5
from parquet_device import output_destination, validate_file


async def run(args: argparse.Namespace) -> dict:
    token = bytes.fromhex(args.token_file.read_text().strip())
    links = [LanLink(args.host, token, timeout=30) for _ in range(args.clients)]
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
            raise ProtocolError("additional collector was accepted; the three-client pool was not full")
        configuration = await sessions[0].get_config()
        if configuration["lan"]["clients"] != 3:
            raise ProtocolError("expected three active sensor clients")
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
            sessions[i].read_window(opened[i][0], 0, min(4096, opened[i][1])) for i in range(1, args.clients)
        ))
        if any(window != payload[:len(window)] for window in retained):
            raise ProtocolError("surviving client lost its file after another disconnected")
        for i in range(1, args.clients):
            await sessions[i].close(opened[i][0])
        destination = output_destination(args.out, name)
        if destination.exists():
            if destination.read_bytes() != payload:
                raise ProtocolError("refusing to replace different local archive bytes")
        else:
            with destination.open("xb") as file:
                file.write(payload)
        inspected = validate_file(destination)
        throttle = await throttle_check(args, links, sessions, name, payload) if args.throttle_seconds else None
        return {
            "firmware": infos[0].get("fw"), "station": infos[0].get("station"),
            "host_clients": args.clients, "sensor_clients": 3, "fourth_busy": True, "disconnect_isolated": True,
            "name": name, "bytes": len(payload), "crc32": f"{binascii.crc32(payload) & 0xffffffff:08x}",
            "sha256": hashlib.sha256(payload).hexdigest(), "seconds": round(elapsed, 3),
            "transfers": [details for _, details in downloads],
            "rows": inspected["rows"], "readers_match": inspected["readers_match"],
            "throttle": throttle,
        }
    finally:
        await challenger.disconnect()
        for link in links:
            await link.disconnect()


async def throttle_check(args, links, sessions, name: str, payload: bytes) -> dict:
    """One collector stops reading; the others must finish, the stuck one gets BYE slow-peer."""
    slow_link = LanLink(args.host, links[1].token, timeout=30)
    slow = Session(slow_link)
    await slow_link.connect()
    await slow.start()
    handle, size, _ = await slow.open(name)
    slow_link.writer.transport.pause_reading()
    window = min(16384, size)
    for _ in range(args.throttle_reads):
        await slow.request(struct.pack("<BHII", OP_READ, handle, 0, window))
    started = time.monotonic()
    fast = sessions[1:]
    for session in fast:
        session.frame_timeout = 45.0
    listed = await asyncio.gather(*(session.list_files() for session in fast))
    downloads = await asyncio.gather(*(session.fetch(name, 4096) for session in fast))
    fast_seconds = time.monotonic() - started
    if any(name not in files for files, _ in listed) or any(data != payload for data, _ in downloads):
        raise ProtocolError("a fast collector lost LIST or READ while one peer was throttled")
    await asyncio.sleep(max(0.0, args.throttle_seconds - (time.monotonic() - started)))
    slow_link.writer.transport.resume_reading()
    deadline = time.monotonic() + 10
    while slow_link.is_connected and time.monotonic() < deadline:
        await asyncio.sleep(0.2)
    error = slow_link.close_error
    if not isinstance(error, LinkClosedByDevice) or error.reason_code != SLOW_PEER:
        raise ProtocolError(f"throttled collector did not end with BYE slow-peer: {error!r}")
    return {"fast_seconds": round(fast_seconds, 3), "pipelined_reads": args.throttle_reads,
            "bye": {k: error.bye[k] for k in ("reason", "err", "queued_frames")}}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clients", type=int, choices=(2, 3), default=3,
                        help="host collectors; use 2 when a phone already occupies the third slot")
    parser.add_argument("--host", required=True)
    parser.add_argument("--throttle-seconds", type=float, default=0.0,
                        help="pause reading on an extra collector this long (use > 15 to reach slow-peer)")
    parser.add_argument("--throttle-reads", type=int, default=64,
                        help="16 KiB READs pipelined to the throttled collector")
    parser.add_argument("--token-file", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(asyncio.run(run(args)), indent=2))


if __name__ == "__main__":
    main()
