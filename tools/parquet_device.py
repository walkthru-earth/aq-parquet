#!/usr/bin/env python3
"""Bounded CoreS3 Parquet serial capture, commands, extraction and host validation.

Examples (run through pixi):
  python tools/parquet_device.py capture --port /dev/cu.usbmodem101 --seconds 620 --until-ready
  python tools/parquet_device.py command --port /dev/cu.usbmodem101 'parquet list'
  python tools/parquet_device.py fetch --port /dev/cu.usbmodem101 boot-0.parquet --out /tmp/readback
  python tools/parquet_device.py bench --port /dev/cu.usbmodem101 --seconds 960 --until-ready --out /tmp/readback --log /tmp/rotation.log
  python tools/parquet_device.py bench --port /dev/cu.usbmodem101 --seconds 35 --flush-after-capture --out /tmp/readback
  python tools/parquet_device.py sync-time --port /dev/cu.usbmodem101
  python tools/parquet_device.py bench --port /dev/cu.usbmodem101 --sync-time --until-ready --out /tmp/readback
  python tools/parquet_device.py inspect /tmp/readback/boot-0.parquet

Opening a port does not intentionally reset the board. Files are published only
after transfer integrity and independent-reader validation; existing files are
accepted only when their contents match exactly.
"""

from __future__ import annotations

import argparse
import binascii
import json
import math
import os
from pathlib import Path
import re
import sys
import tempfile
import time


MAX_LINE_BYTES = 1024 * 1024
MAX_FILE_BYTES = 32 * 1024 * 1024
SAFE_COMPONENT = re.compile(r"[A-Za-z0-9][A-Za-z0-9_=.-]*\Z")
BEGIN = re.compile(r"PARQUET DATA BEGIN name=(\S+) bytes=(\d+)\Z")
DATA = re.compile(r"PARQUET DATA offset=(\d+) hex=([0-9a-fA-F]+)\Z")
END = re.compile(r"PARQUET DATA END name=(\S+) bytes=(\d+) crc32=([0-9a-fA-F]{8})\Z")
READY = re.compile(r"PARQUET READY name=(\S+)(?: |$)")
TIME_RESPONSE = re.compile(r"PARQUET TIME epoch_s=(\d+) source=host(?: monotonic_us=\d+)?\Z")


def positive_seconds(value: str) -> float:
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("time limit must be finite and positive")
    return number


def safe_name(name: str) -> str:
    """Allow flat legacy names or bounded relative Hive paths beneath /output."""
    parts = name.split("/")
    if (
        len(name) > 384
        or len(parts) > 8
        or not name.endswith(".parquet")
        or any(len(part) > 128 or ".." in part or not SAFE_COMPONENT.fullmatch(part) for part in parts)
    ):
        raise ValueError("expected a relative .parquet path without traversal, empty components or unsafe characters (maximum 384 bytes, 8 components)")
    return name


def output_destination(directory: Path, name: str) -> Path:
    """Create only ordinary directories inside the explicitly selected root."""
    safe_name(name)
    if directory.is_symlink():
        raise ValueError("refusing a symbolic-link output directory")
    directory.mkdir(parents=True, exist_ok=True)
    current = directory
    parts = name.split("/")
    for part in parts[:-1]:
        current = current / part
        if current.is_symlink():
            raise ValueError(f"refusing symbolic-link output ancestry: {current}")
        current.mkdir(exist_ok=True)
        if not current.is_dir():
            raise ValueError(f"output ancestry is not a directory: {current}")
    destination = current / parts[-1]
    if destination.is_symlink():
        raise ValueError("refusing a symbolic-link destination")
    return destination


def open_port(name: str, baud: int):
    import serial

    # On our macOS/CoreS3 native-USB bench, False/False reset the running board
    # on every open. True/True plus clearing HUPCL preserved its boot identity.
    # Keep that tested pair; do not pulse either line. Other hosts need checking.
    port = serial.Serial(port=None, baudrate=baud, timeout=0.1, write_timeout=2)
    port.dtr = True
    port.rts = True
    port.port = name
    port.open()
    if sys.platform != "win32":
        import termios
        attributes = termios.tcgetattr(port.fileno())
        attributes[2] &= ~termios.HUPCL
        termios.tcsetattr(port.fileno(), termios.TCSANOW, attributes)
    return port


def lines_until(port, deadline: float):
    pending = bytearray()
    while time.monotonic() < deadline:
        chunk = port.read(min(max(port.in_waiting, 1), 16384))
        if not chunk:
            continue
        pending.extend(chunk)
        while b"\n" in pending:
            raw, _, tail = pending.partition(b"\n")
            pending = bytearray(tail)
            if len(raw) > MAX_LINE_BYTES:
                raise ValueError("serial line exceeds 1 MiB safety bound")
            yield raw.rstrip(b"\r").decode("utf-8", "replace")
        if len(pending) > MAX_LINE_BYTES:
            raise ValueError("unterminated serial line exceeds 1 MiB safety bound")


def send_command(port, command: str) -> None:
    encoded = (command + "\n").encode("ascii")
    port.reset_input_buffer()
    if port.write(encoded) != len(encoded):
        raise IOError("short serial command write")


def validate_file(path: Path, *, allow_nonfinite: bool = False) -> dict:
    import duckdb
    import pyarrow as pa
    import pyarrow.parquet as pq

    parquet = pq.ParquetFile(path)
    table = parquet.read()
    if len(set(table.column_names)) != len(table.column_names):
        raise ValueError("duplicate column names cannot be compared unambiguously")
    # Schema v3 annotates the UTC fields TIMESTAMP(NANOS, UTC). Compare them as
    # the stored INT64 nanoseconds on both sides: exact, and independent of
    # DuckDB's microsecond TIMESTAMPTZ conversion (which also needs pytz).
    timestamps = [field.name for field in table.schema if pa.types.is_timestamp(field.type)]
    table = pa.table({name: column.cast(pa.int64()) if name in timestamps else column
                      for name, column in zip(table.column_names, table.columns)})
    expected = table.to_pylist()
    selection = ", ".join(f'epoch_ns("{name}") AS "{name}"' if name in timestamps else f'"{name}"'
                          for name in table.column_names)
    with duckdb.connect(":memory:") as connection:
        # Compare file columns only; Hive directory columns are query metadata.
        cursor = connection.execute(f"SELECT {selection} FROM read_parquet(?, hive_partitioning=false)",
                                    [str(path.resolve())])
        names = [column[0] for column in cursor.description]
        actual_rows = cursor.fetchall()
    if names != table.column_names or len(actual_rows) != len(expected):
        raise ValueError("PyArrow and DuckDB disagree on columns or row count")
    for index, (arrow_row, duck_row) in enumerate(zip(expected, actual_rows)):
        for name, actual in zip(names, duck_row):
            wanted = arrow_row[name]
            both_nan = isinstance(actual, float) and isinstance(wanted, float) and math.isnan(actual) and math.isnan(wanted)
            if actual != wanted and not both_nan:
                raise ValueError(f"reader mismatch at row {index}, column {name}: {wanted!r} != {actual!r}")

    columns = []
    for field, column in zip(table.schema, table.columns):
        if pa.types.is_floating(field.type):
            if not allow_nonfinite and any(value is not None and not math.isfinite(value) for value in column.to_pylist()):
                raise ValueError(f"non-finite non-null measurement in {field.name}")
        columns.append({"name": field.name, "type": str(field.type), "nulls": column.null_count})

    summary = {
        "path": str(path),
        "bytes": path.stat().st_size,
        "rows": table.num_rows,
        "row_groups": parquet.metadata.num_row_groups,
        "readers_match": True,
        "columns": columns,
    }
    if "sequence" in names:
        sequence = table["sequence"].to_pylist()
        if any(value is None for value in sequence):
            raise ValueError("null sequence identity")
        if any(right <= left for left, right in zip(sequence, sequence[1:])):
            raise ValueError("sequence is not strictly increasing within this file")
        summary["sequence"] = {
            "first": sequence[0] if sequence else None,
            "last": sequence[-1] if sequence else None,
            "missing_between_rows": sum(right - left - 1 for left, right in zip(sequence, sequence[1:])),
        }
    if "monotonic_us" in names:
        ticks = table["monotonic_us"].to_pylist()
        if any(value is None for value in ticks):
            raise ValueError("null monotonic acquisition time")
        intervals = [right - left for left, right in zip(ticks, ticks[1:])]
        if any(delta <= 0 for delta in intervals):
            raise ValueError("monotonic acquisition time is not strictly increasing")
        summary["cadence_us"] = {
            "min": min(intervals) if intervals else None,
            "max": max(intervals) if intervals else None,
            "mean": sum(intervals) / len(intervals) if intervals else None,
            "max_deviation_from_10s": max((abs(delta - 10_000_000) for delta in intervals), default=None),
        }
    return summary


def receive_file(lines, name: str) -> bytes:
    """Parse one transfer, ignoring interleaved normal telemetry lines."""
    safe_name(name)
    expected_size = None
    result = bytearray()
    for line in lines:
        if line.startswith("PARQUET ERROR"):
            raise RuntimeError(line)
        if match := BEGIN.fullmatch(line):
            if expected_size is not None:
                raise ValueError("duplicate transfer BEGIN")
            if match[1] != name:
                raise ValueError("transfer relative path does not match requested file")
            expected_size = int(match[2])
            if not 12 <= expected_size <= MAX_FILE_BYTES:
                raise ValueError("transfer size outside 12-byte to 32-MiB bound")
        elif match := DATA.fullmatch(line):
            if expected_size is None:
                raise ValueError("transfer data before BEGIN")
            if int(match[1]) != len(result):
                raise ValueError(f"transfer offset mismatch: expected {len(result)}, got {match[1]}")
            payload = bytes.fromhex(match[2])
            if len(result) + len(payload) > expected_size:
                raise ValueError("transfer exceeds declared size")
            result.extend(payload)
        elif match := END.fullmatch(line):
            if expected_size is None or match[1] != name:
                raise ValueError("unexpected transfer END")
            if int(match[2]) != expected_size or len(result) != expected_size:
                raise ValueError("incomplete transfer or END size mismatch")
            actual_crc = binascii.crc32(result) & 0xFFFFFFFF
            if actual_crc != int(match[3], 16):
                raise ValueError(f"CRC32 mismatch: computed {actual_crc:08x}, received {match[3]}")
            return bytes(result)
        elif line.startswith("PARQUET DATA"):
            raise ValueError(f"malformed transfer line: {line[:160]}")
    raise TimeoutError("deadline reached before a complete Parquet transfer")


def save_verified(payload: bytes, name: str, directory: Path) -> dict:
    destination = output_destination(directory, name)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix=".parquet-transfer-", dir=destination.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        summary = validate_file(temporary)
        # Hard-link publication is atomic and refuses to overwrite an existing path.
        try:
            os.link(temporary, destination)
        except FileExistsError:
            if destination.is_symlink() or destination.read_bytes() != payload:
                raise FileExistsError(f"destination exists with different bytes: {destination}") from None
        summary["path"] = str(destination)
        summary["crc32"] = f"{binascii.crc32(payload) & 0xFFFFFFFF:08x}"
        return summary
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def capture(port, args) -> int:
    deadline = time.monotonic() + args.seconds
    stream = None
    seen = False
    ready = False
    try:
        if args.out:
            args.out.parent.mkdir(parents=True, exist_ok=True)
            stream = args.out.open("x", encoding="utf-8")
        for line in lines_until(port, deadline):
            seen = True
            print(line, flush=True)
            if stream:
                stream.write(line + "\n")
                stream.flush()
            if args.until_ready and line.startswith("PARQUET READY"):
                ready = True
                break
    finally:
        if stream:
            stream.close()
    if not seen:
        raise TimeoutError("no serial lines received before deadline")
    if args.until_ready and not ready:
        raise TimeoutError("capture deadline reached before PARQUET READY")
    return 0


def command(port, args) -> int:
    text = " ".join(args.text.split())
    if text not in {"parquet status", "parquet schema", "parquet list", "parquet flush",
                    "parquet erase-archive CONFIRM",
                    "parquet interval 600", "parquet interval 900", "parquet interval 1800",
                    "parquet interval 3600", "parquet codec none", "parquet codec lz4", "parquet codec-test"}:
        raise ValueError("supported commands: parquet status/schema/list/flush/interval 600|900|1800|3600/"
                         "codec none/codec lz4/codec-test/erase-archive CONFIRM; use fetch for get")
    send_command(port, text)
    for line in lines_until(port, time.monotonic() + args.timeout):
        print(line, flush=True)
        if line.startswith("PARQUET ERROR"):
            raise RuntimeError(line)
        if text == "parquet erase-archive CONFIRM":
            # Native ESP-IDF output can share the UART line with a physical
            # owner reply. Match the exact sentinel after an unrelated prefix.
            if acknowledgement := re.search(r"PARQUET ERASE_ARCHIVE (ok=[01]\b.*)", line):
                if acknowledgement[1].startswith("ok=1"):
                    return 0
                raise RuntimeError("PARQUET ERASE_ARCHIVE " + acknowledgement[1])
        elif text == "parquet schema":
            if line == "PARQUET SCHEMA END":
                return 0
        elif text == "parquet list":
            if line == "PARQUET LIST END":
                return 0
        elif text == "parquet codec-test":
            if line.startswith("PARQUET BENCH END"):
                return 0
        elif text == "parquet flush":
            if line.startswith(("PARQUET READY", "PARQUET FLUSH")):
                return 0
        elif line.startswith(("PARQUET STATUS", "PARQUET INTERVAL", "PARQUET CONFIG")):
            return 0
    raise TimeoutError("deadline reached before command response")


def sync_time(port, timeout: float, read_lines=None) -> int:
    """Explicitly trust this host's UTC clock, without reopening the connection."""
    epoch = int(time.time())
    send_command(port, f"parquet time {epoch}")
    deadline = time.monotonic() + timeout
    lines = read_lines(deadline) if read_lines else lines_until(port, deadline)
    for line in lines:
        if read_lines is None:
            print(line, flush=True)
        if line.startswith("PARQUET ERROR"):
            raise RuntimeError(line)
        if match := TIME_RESPONSE.fullmatch(line):
            if int(match[1]) != epoch:
                raise ValueError("device time acknowledgement does not match the host UTC sent")
            return epoch
    raise TimeoutError("deadline reached before host UTC acknowledgement")


def bench(port, args) -> int:
    """Capture and fetch a newly completed batch without reopening serial.

    Default: wait for automatic rotation, failing if no READY arrives in seconds.
    --flush-after-capture: collect for seconds, explicitly flush, then fetch.
    Flush and transfer each have their own bounded --timeout period.
    """
    log = None
    ready_name = None
    try:
        if args.log:
            args.log.parent.mkdir(parents=True, exist_ok=True)
            log = args.log.open("x", encoding="utf-8")

        def recorded(deadline):
            for line in lines_until(port, deadline):
                if log:
                    log.write(line + "\n")
                    log.flush()
                # The log retains complete transfer bytes; stdout stays readable.
                if not line.startswith("PARQUET DATA offset="):
                    print(line, flush=True)
                yield line

        if args.sync_time:
            sync_time(port, args.timeout, recorded)

        for line in recorded(time.monotonic() + args.seconds):
            if line.startswith("PARQUET ERROR"):
                raise RuntimeError(line)
            if match := READY.match(line):
                ready_name = safe_name(match[1])
                if args.until_ready:
                    break

        if not args.until_ready:
            ready_name = None
            send_command(port, "parquet flush")
            for line in recorded(time.monotonic() + args.timeout):
                if line.startswith("PARQUET ERROR"):
                    raise RuntimeError(line)
                if match := READY.match(line):
                    ready_name = safe_name(match[1])
                    break

        if ready_name is None:
            raise TimeoutError("bench deadline reached before a finalized Parquet batch")
        send_command(port, f"parquet get {ready_name}")
        payload = receive_file(recorded(time.monotonic() + args.timeout), ready_name)
        summary = save_verified(payload, ready_name, args.out)
        summary["single_serial_session"] = True
        summary["rotation"] = "automatic" if args.until_ready else "manual-flush"
        summary["host_time_sync_requested"] = args.sync_time
        print(json.dumps(summary, indent=2))
        return 0
    finally:
        if log:
            log.close()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    subs = parser.add_subparsers(dest="action", required=True)
    for action in ("capture", "command", "fetch", "bench", "sync-time"):
        sub = subs.add_parser(action)
        sub.add_argument("--port", required=True)
        sub.add_argument("--baud", type=int, default=115200)
        if action == "bench":
            sub.add_argument("--seconds", type=positive_seconds, default=960.0, help="capture/rotation deadline; default covers a 15-minute rotation")
            sub.add_argument("--timeout", type=positive_seconds, default=60.0, help="separate deadline for flush and transfer")
            sub.add_argument("--out", type=Path, required=True, help="destination directory")
            sub.add_argument("--log", type=Path, help="exclusive new log file, including transfer bytes")
            sub.add_argument("--sync-time", action="store_true", help="first set device UTC from this host's clock, on the same serial connection")
            mode = sub.add_mutually_exclusive_group()
            mode.add_argument("--until-ready", dest="until_ready", action="store_true", help="wait for automatic rotation and fetch (default)")
            mode.add_argument("--flush-after-capture", dest="until_ready", action="store_false", help="capture for --seconds, manually flush, then fetch")
            sub.set_defaults(until_ready=True)
        elif action == "sync-time":
            sub.add_argument("--timeout", type=positive_seconds, default=10.0)
        elif action == "capture":
            sub.add_argument("--seconds", type=positive_seconds, default=30.0)
            sub.add_argument("--out", type=Path, help="exclusive new log file")
            sub.add_argument("--until-ready", action="store_true")
        else:
            sub.add_argument("--timeout", type=positive_seconds, default=60.0)
            if action == "command":
                sub.add_argument("text", help="quoted firmware command")
            else:
                sub.add_argument("name", help="relative Hive path or legacy flat filename reported by the device")
                sub.add_argument("--out", type=Path, required=True, help="destination root; relative Hive directories are preserved")
    inspect = subs.add_parser("inspect")
    inspect.add_argument("path", type=Path)
    inspect.add_argument("--allow-nonfinite", action="store_true", help="allow IEEE NaN/infinity in format conformance fixtures")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        if args.action == "inspect":
            print(json.dumps(validate_file(args.path, allow_nonfinite=args.allow_nonfinite), indent=2))
            return 0
        if args.action == "fetch":
            safe_name(args.name)
        with open_port(args.port, args.baud) as port:
            if args.action == "sync-time":
                sync_time(port, args.timeout)
                return 0
            if args.action == "bench":
                return bench(port, args)
            if args.action == "capture":
                return capture(port, args)
            if args.action == "command":
                return command(port, args)
            send_command(port, f"parquet get {args.name}")
            payload = receive_file(lines_until(port, time.monotonic() + args.timeout), args.name)
        print(json.dumps(save_verified(payload, args.name, args.out), indent=2))
        return 0
    except KeyboardInterrupt:
        print("parquet-device: interrupted", file=sys.stderr)
        return 130
    except Exception as exc:
        print(f"parquet-device: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
