"""Export all currently listed finalized files without resetting or flushing.

Use an artifacts/ directory OUTSIDE firmware build/; compilation can clean build/.
Legacy names are exposed only by firmware supporting the legacy-parquet prefix.
"""
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import time

import parquet_device as device


def fetch_with_retry(port, name: str, expected_bytes: int, retries: int) -> bytes:
    """One `parquet get` per attempt. The board's own PARQUET ROW log lines share
    the serial link with the hex transfer, and a rare corrupted or truncated line
    fails the strict parser; the transfer is idempotent, so simply ask again."""
    failure = None
    for attempt in range(1, retries + 1):
        device.send_command(port, "parquet get " + name)
        try:
            payload = device.receive_file(device.lines_until(port, time.monotonic() + 60), name)
        except (ValueError, TimeoutError) as error:
            failure = error
            print(f"RETRY attempt={attempt} name={name} reason={error}", flush=True)
            # Let the device finish emitting the failed transfer before re-requesting.
            for _ in device.lines_until(port, time.monotonic() + 2):
                pass
            continue
        if len(payload) != expected_bytes:
            raise ValueError("inventory size mismatch: " + name)
        return payload
    raise RuntimeError(f"transfer failed after {retries} attempts: {name}: {failure}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--retries", type=int, default=3,
                        help="per-file transfer attempts before the export fails (default 3)")
    args = parser.parse_args()
    if "build" in args.out.parts:
        parser.error("use artifacts/, not a build directory that firmware tools may clean")
    if args.out.is_symlink():
        parser.error("output directory must not be a symbolic link")
    if (args.out / "manifest.json").exists():
        parser.error("choose a new export directory; manifest already exists")
    records = {}
    pattern = re.compile(r"PARQUET FILE name=(\S+) bytes=(\d+)\Z")
    with device.open_port(args.port, 115200) as port:
        for sweep in range(3):
            listed = {}
            device.send_command(port, "parquet list")
            for line in device.lines_until(port, time.monotonic() + 30):
                if line.startswith("PARQUET ERROR"):
                    raise RuntimeError(line)
                if match := pattern.fullmatch(line):
                    listed[device.safe_name(match[1])] = int(match[2])
                if line.startswith("PARQUET PARTIAL"):
                    print(line, flush=True)
                if line == "PARQUET LIST END":
                    break
            else:
                raise TimeoutError("incomplete inventory")
            missing = sorted(set(listed) - set(records))
            if not missing:
                break
            for name in missing:
                relative = name if name.startswith("legacy-parquet/") else "output/" + name
                existing = args.out / relative
                if existing.is_file() and not existing.is_symlink() and existing.stat().st_size == listed[name]:
                    # Resume: a previous interrupted run already published this file.
                    # save_verified re-validates the bytes and refuses a differing copy.
                    payload = existing.read_bytes()
                    source = "resumed-local-verified"
                else:
                    payload = fetch_with_retry(port, name, listed[name], args.retries)
                    source = "live-device-readback"
                summary = device.save_verified(payload, relative, args.out)
                records[name] = {"path": relative, "source": source,
                                 "bytes": len(payload), "rows": summary["rows"],
                                 "crc32": summary["crc32"],
                                 "sha256": hashlib.sha256(payload).hexdigest(),
                                 "readers_match": summary["readers_match"]}
                print(json.dumps(records[name]), flush=True)
        else:
            raise RuntimeError("inventory kept changing; partial export retained, retry with a new directory")
    report = {"completed_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
              "port": args.port, "inventory_rechecked": True,
              "reset_or_flush_requested": False, "device_deletions": False,
              "files": list(records.values()), "total_files": len(records),
              "total_rows": sum(item["rows"] for item in records.values())}
    args.out.mkdir(parents=True, exist_ok=True)
    with (args.out / "manifest.json").open("x") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(f"PASS {len(records)} files / {report['total_rows']} rows -> {args.out}", flush=True)


if __name__ == "__main__":
    main()
