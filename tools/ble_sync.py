"""Host client for the device sync service (docs/shared/ble-sync-protocol.md, v2).

Bench tool: exercises the same contract the phone app implements, from a
laptop, so the firmware can be verified without a handset. Read-only towards
the device except for the explicitly named write operations (`time`, `flush`,
`set`, `reboot`).

  pixi run ble-sync scan [--seconds 8]
  pixi run ble-sync info  [--name AQ-6b40]
  pixi run ble-sync status
  pixi run ble-sync live  [--seconds 35]
  pixi run ble-sync list
  pixi run ble-sync fetch <relative-name> --out <dir>
  pixi run ble-sync sync  --out <dir>          # every listed file, verified
  pixi run ble-sync time                       # send host UTC (explicit)
  pixi run ble-sync flush                      # finalize RAM rows (explicit)
  pixi run ble-sync config                     # GET_CONFIG as JSON
  pixi run ble-sync set wifi.ssid=home wifi.psk=... wifi.on=1   # SET_CONFIG
  pixi run ble-sync wifi-scan                  # WIFI_AP list
  pixi run ble-sync token --save artifacts/lan-token.txt        # BLE only
  pixi run ble-sync log [--bytes 8192]         # LOG_TAIL
  pixi run ble-sync reboot                     # explicit

Every command also runs over the LAN transport once a token is held:

  pixi run ble-sync --lan aq-6b40.local --token-file artifacts/lan-token.txt list

macOS pairs through a system passkey dialog on first encrypted access; type the
six digits shown on the CoreS3 screen (or the fixed PIN on a screenless
board). Bleak cannot drive pairing itself. `dns-sd -B _aqsync._tcp` browses
the LAN for devices.
"""
import argparse
import asyncio
import binascii
import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import time

from bleak import BleakClient, BleakScanner

import parquet_device as device

BASE = "c0a5e9f0-{:04x}-4b1a-9c3e-2d7f8a6b4e01"
SERVICE = BASE.format(1)
INFO, STATUS, LIVE, CONTROL, RESPONSE = (BASE.format(i) for i in range(2, 7))

OP_LIST, OP_OPEN, OP_READ, OP_CLOSE, OP_SET_TIME, OP_FLUSH, OP_STATUS = range(1, 8)
OP_GET_CONFIG, OP_SET_CONFIG, OP_WIFI_SCAN, OP_REBOOT, OP_LOG_TAIL, OP_GET_TOKEN = range(8, 14)
F_FILE, F_LIST_END = 0x10, 0x11
F_OPENED, F_CHUNK, F_READ_END, F_CLOSED = 0x20, 0x21, 0x22, 0x23
F_TIME_SET, F_FLUSHED, F_ERROR = 0x30, 0x31, 0x7F
F_CONFIG, F_WIFI_AP, F_WIFI_SCAN_END, F_REBOOTING = 0x40, 0x41, 0x42, 0x43
F_LOG, F_LOG_END, F_TOKEN, F_HELLO, F_STATUS, F_LIVE = 0x44, 0x45, 0x46, 0x47, 0x48, 0x49
ERRORS = {1: "malformed", 2: "invalid-name", 3: "not-finalized", 4: "open-failed",
          5: "bad-handle", 6: "range", 7: "busy", 8: "storage-unavailable",
          9: "invalid-epoch", 10: "nothing-to-flush", 11: "unknown-op",
          12: "invalid-config", 13: "not-on-this-link", 14: "auth-failed", 15: "wifi-unavailable"}
WIFI_AUTH = {0: "open", 1: "WEP", 2: "WPA", 3: "WPA2", 4: "WPA/WPA2", 5: "WPA2-Enterprise",
             6: "WPA3", 7: "WPA2/WPA3", 255: "other"}
LAN_PORT = 47390
LAN_PAYLOAD_MAX = 1024


class LanLink:
    """TCP transport with the BleakClient surface Session uses (docs: LAN transport).

    Length-prefixed frames; STATUS/LIVE pushes are demultiplexed by type byte and
    delivered to the callbacks registered for the matching characteristic UUID,
    so every command works unchanged over either link.
    """

    def __init__(self, host: str, token: bytes, port: int = LAN_PORT, timeout: float = 10.0):
        if len(token) != 32:
            raise ProtocolError("LAN token must be 32 bytes")
        self.host, self.port, self.token, self.timeout = host, port, token, timeout
        self.callbacks: dict[str, object] = {}
        self.latest = {STATUS: b"{}", LIVE: b"{}"}
        self.info_json = b"{}"
        self.payload_max = LAN_PAYLOAD_MAX
        self.reader = None
        self.writer = None
        self.task = None
        self.is_connected = False
        self.mtu_size = LAN_PAYLOAD_MAX + 3

    async def connect(self):
        self.reader, self.writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port), self.timeout)
        self.writer.write(b"AQS1" + self.token)
        await self.writer.drain()
        hello = await self._read_frame()
        if hello[0] == F_ERROR:
            raise ProtocolError(f"LAN handshake refused code={hello[2]} ({ERRORS.get(hello[2], '?')})")
        if hello[0] != F_HELLO:
            raise ProtocolError(f"expected HELLO, got 0x{hello[0]:02x}")
        proto, self.payload_max = struct.unpack_from("<BH", hello, 1)
        self.info_json = bytes(hello[4:])
        self.is_connected = True
        self.task = asyncio.create_task(self._pump())
        print(f"LAN CONNECTED host={self.host} port={self.port} proto={proto} payload_max={self.payload_max}", flush=True)

    async def _read_frame(self) -> bytes:
        header = await asyncio.wait_for(self.reader.readexactly(2), self.timeout)
        (length,) = struct.unpack("<H", header)
        if length == 0 or length > LAN_PAYLOAD_MAX:
            raise ProtocolError(f"LAN frame length {length} out of range")
        return await asyncio.wait_for(self.reader.readexactly(length), self.timeout)

    async def _pump(self):
        try:
            while True:
                header = await self.reader.readexactly(2)
                (length,) = struct.unpack("<H", header)
                frame = await self.reader.readexactly(length)
                if frame and frame[0] == F_STATUS:
                    self.latest[STATUS] = bytes(frame[1:])
                    self._deliver(STATUS, frame[1:])
                elif frame and frame[0] == F_LIVE:
                    self.latest[LIVE] = bytes(frame[1:])
                    self._deliver(LIVE, frame[1:])
                else:
                    self._deliver(RESPONSE, frame)
        except (asyncio.IncompleteReadError, ConnectionError, asyncio.CancelledError):
            self.is_connected = False

    def _deliver(self, uuid: str, data: bytes):
        callback = self.callbacks.get(uuid)
        if callback:
            callback(None, bytearray(data))

    async def start_notify(self, uuid: str, callback):
        self.callbacks[uuid] = callback

    async def read_gatt_char(self, uuid: str):
        if uuid == INFO:
            return bytearray(self.info_json)
        # Pushes arrive on the device's next tick after HELLO; give the first one a moment.
        for _ in range(20):
            if self.latest[uuid] != b"{}":
                break
            await asyncio.sleep(0.1)
        return bytearray(self.latest[uuid])

    async def write_gatt_char(self, _uuid: str, data: bytes, response=True):
        if not self.is_connected:
            raise ProtocolError("LAN link closed")
        self.writer.write(struct.pack("<H", len(data)) + bytes(data))
        await self.writer.drain()

    async def disconnect(self):
        self.is_connected = False
        if self.task:
            self.task.cancel()
        if self.writer:
            self.writer.close()


class ProtocolError(RuntimeError):
    pass


class Session:
    """One connection; serializes control requests and collects response frames."""

    def __init__(self, client: BleakClient, verbose=False, *, read_timeout=20.0):
        if not math.isfinite(read_timeout) or read_timeout <= 0:
            raise ValueError("read timeout must be finite and positive")
        self.client = client
        self.frames: asyncio.Queue[bytes] = asyncio.Queue()
        self.verbose = verbose
        self.bytes_received = 0
        self.read_timeout = read_timeout

    async def start(self):
        await self.client.start_notify(RESPONSE, self._on_response)

    def _on_response(self, _characteristic, data: bytearray):
        self.bytes_received += len(data)
        self.frames.put_nowait(bytes(data))

    async def request(self, payload: bytes):
        await self.client.write_gatt_char(CONTROL, payload, response=True)

    async def frame(self, timeout=15.0) -> bytes:
        try:
            data = await asyncio.wait_for(self.frames.get(), timeout)
        except asyncio.TimeoutError:
            raise ProtocolError("no response frame within deadline") from None
        if not data:
            raise ProtocolError("empty frame")
        if data[0] == F_ERROR:
            op, code = data[1], data[2]
            raise ProtocolError(f"device ERROR op=0x{op:02x} code={code} ({ERRORS.get(code, '?')}) "
                                f"detail={data[3:].decode('utf-8', 'replace')!r}")
        return data

    async def read_char(self, uuid: str) -> bytearray:
        # Bleak 3.0.2's CoreBluetooth backend ignores read timeout kwargs and
        # its delegate expires after 20 s, including during first pairing.
        # Reads are idempotent: retry backend timeouts within one fixed deadline.
        # The outer deadline also bounds backends that never finish a read.
        try:
            async with asyncio.timeout(self.read_timeout):
                while True:
                    try:
                        return await self.client.read_gatt_char(uuid)
                    except TimeoutError:
                        await asyncio.sleep(0.1)
        except TimeoutError:
            raise ProtocolError(f"characteristic read {uuid} did not complete within "
                                f"{self.read_timeout:g}s") from None

    async def info(self) -> dict:
        return json.loads(bytes(await self.read_char(INFO)))

    async def status(self) -> dict:
        return json.loads(bytes(await self.read_char(STATUS)))

    async def list_files(self) -> tuple[dict[str, int], dict]:
        await self.request(bytes([OP_LIST]))
        listed: dict[str, int] = {}
        while True:
            data = await self.frame()
            if data[0] == F_FILE:
                (size,) = struct.unpack_from("<I", data, 1)
                name = device.safe_name(data[5:].decode("utf-8"))
                listed[name] = size
            elif data[0] == F_LIST_END:
                count, partials, sd_kib, used_kib = struct.unpack_from("<HHII", data, 1)
                if count != len(listed):
                    raise ProtocolError(f"LIST_END count {count} != {len(listed)} received entries")
                return listed, {"count": count, "partials": partials,
                                "sd_kib": sd_kib, "sd_used_kib": used_kib}
            else:
                raise ProtocolError(f"unexpected frame 0x{data[0]:02x} during LIST")

    async def open(self, name: str) -> tuple[int, int, int]:
        await self.request(bytes([OP_OPEN]) + name.encode("ascii"))
        data = await self.frame()
        if data[0] != F_OPENED:
            raise ProtocolError(f"expected OPENED, got 0x{data[0]:02x}")
        handle, size, crc = struct.unpack_from("<HII", data, 1)
        echoed = data[11:].decode("utf-8")
        if echoed != name:
            raise ProtocolError(f"OPENED echoed {echoed!r}, requested {name!r}")
        return handle, size, crc

    async def read_window(self, handle: int, offset: int, length: int) -> bytes:
        await self.request(struct.pack("<BHII", OP_READ, handle, offset, length))
        out = bytearray()
        while True:
            data = await self.frame()
            if data[0] == F_CHUNK:
                got_handle, got_offset = struct.unpack_from("<HI", data, 1)
                if got_handle != handle:
                    raise ProtocolError("CHUNK for a different handle")
                if got_offset != offset + len(out):
                    raise ProtocolError(f"CHUNK offset {got_offset}, expected {offset + len(out)}")
                out.extend(data[7:])
            elif data[0] == F_READ_END:
                got_handle, next_offset, status = struct.unpack_from("<HIB", data, 1)
                if status:
                    raise ProtocolError(f"READ_END status={status} ({ERRORS.get(status, '?')})")
                if next_offset != offset + len(out):
                    raise ProtocolError(f"READ_END next_offset {next_offset} != {offset + len(out)}")
                return bytes(out)
            else:
                raise ProtocolError(f"unexpected frame 0x{data[0]:02x} during READ")

    async def close(self, handle: int):
        await self.request(struct.pack("<BH", OP_CLOSE, handle))
        data = await self.frame()
        if data[0] != F_CLOSED:
            raise ProtocolError(f"expected CLOSED, got 0x{data[0]:02x}")

    async def fetch(self, name: str, max_read: int) -> tuple[bytes, dict]:
        started = time.monotonic()
        handle, size, expected_crc = await self.open(name)
        payload = bytearray()
        windows = 0
        while len(payload) < size:
            window = await self.read_window(handle, len(payload), min(max_read, size - len(payload)))
            if not window:
                raise ProtocolError("empty READ window before end of file")
            payload.extend(window)
            windows += 1
        await self.close(handle)
        elapsed = time.monotonic() - started
        crc = binascii.crc32(payload) & 0xFFFFFFFF
        if crc != expected_crc:
            raise ProtocolError(f"CRC32 mismatch computed {crc:08x} device {expected_crc:08x}")
        if payload[:4] != b"PAR1" or payload[-4:] != b"PAR1":
            raise ProtocolError("missing PAR1 magic")
        return bytes(payload), {"bytes": size, "windows": windows, "seconds": round(elapsed, 3),
                                "bytes_per_s": round(size / elapsed) if elapsed else None,
                                "crc32": f"{crc:08x}"}

    async def set_time(self, epoch_s: int) -> dict:
        await self.request(struct.pack("<Bq", OP_SET_TIME, epoch_s))
        data = await self.frame()
        if data[0] != F_TIME_SET:
            raise ProtocolError(f"expected TIME_SET, got 0x{data[0]:02x}")
        epoch, mono = struct.unpack_from("<qq", data, 1)
        return {"epoch_s": epoch, "monotonic_us": mono}

    async def flush(self) -> dict:
        await self.request(bytes([OP_FLUSH]))
        data = await self.frame(timeout=60)
        if data[0] != F_FLUSHED:
            raise ProtocolError(f"expected FLUSHED, got 0x{data[0]:02x}")
        rows, finalized = struct.unpack_from("<HI", data, 1)
        return {"rows": rows, "files_finalized": finalized}

    async def get_config(self) -> dict:
        await self.request(bytes([OP_GET_CONFIG]))
        return self._config_frame(await self.frame())

    async def set_config(self, pairs: list[str]) -> dict:
        for pair in pairs:
            if "=" not in pair:
                raise ProtocolError(f"expected key=value, got {pair!r}")
        await self.request(bytes([OP_SET_CONFIG]) + "\n".join(pairs).encode("utf-8"))
        return self._config_frame(await self.frame())

    @staticmethod
    def _config_frame(data: bytes) -> dict:
        if data[0] != F_CONFIG:
            raise ProtocolError(f"expected CONFIG, got 0x{data[0]:02x}")
        config = json.loads(bytes(data[2:]))
        config["reboot_required"] = bool(data[1] & 1)
        return config

    async def wifi_scan(self) -> list[dict]:
        await self.request(bytes([OP_WIFI_SCAN]))
        networks = []
        while True:
            data = await self.frame(timeout=30)
            if data[0] == F_WIFI_AP:
                rssi, auth, channel = struct.unpack_from("<bBB", data, 1)
                networks.append({"ssid": data[4:].decode("utf-8", "replace"), "rssi": rssi,
                                 "auth": WIFI_AUTH.get(auth, str(auth)), "channel": channel})
            elif data[0] == F_WIFI_SCAN_END:
                count, status = struct.unpack_from("<HB", data, 1)
                if status:
                    raise ProtocolError(f"WIFI_SCAN_END status={status} ({ERRORS.get(status, '?')})")
                if count != len(networks):
                    raise ProtocolError(f"WIFI_SCAN_END count {count} != {len(networks)} received")
                return networks
            else:
                raise ProtocolError(f"unexpected frame 0x{data[0]:02x} during WIFI_SCAN")

    async def get_token(self) -> tuple[int, bytes]:
        await self.request(bytes([OP_GET_TOKEN]))
        data = await self.frame()
        if data[0] != F_TOKEN:
            raise ProtocolError(f"expected TOKEN, got 0x{data[0]:02x}")
        (port,) = struct.unpack_from("<H", data, 1)
        token = bytes(data[3:])
        if len(token) != 32:
            raise ProtocolError(f"TOKEN carries {len(token)} bytes, expected 32")
        return port, token

    async def log_tail(self, max_bytes: int) -> tuple[str, dict]:
        await self.request(struct.pack("<BH", OP_LOG_TAIL, max_bytes))
        text = bytearray()
        while True:
            data = await self.frame()
            if data[0] == F_LOG:
                text.extend(data[1:])
            elif data[0] == F_LOG_END:
                total, returned = struct.unpack_from("<IH", data, 1)
                if returned != len(text):
                    raise ProtocolError(f"LOG_END returned {returned} != {len(text)} received")
                return text.decode("utf-8", "replace"), {"total_bytes_since_boot": total, "returned": returned}
            else:
                raise ProtocolError(f"unexpected frame 0x{data[0]:02x} during LOG_TAIL")

    async def reboot(self) -> dict:
        await self.request(bytes([OP_REBOOT]))
        data = await self.frame(timeout=60)
        if data[0] != F_REBOOTING:
            raise ProtocolError(f"expected REBOOTING, got 0x{data[0]:02x}")
        (delay_ms,) = struct.unpack_from("<H", data, 1)
        return {"delay_ms": delay_ms}


def advertises_sync_service(ad):
    # Protocol 2.1 puts service data in ADV and the UUID list in SCAN_RSP.
    # A scan callback may contain ADV alone; discovery must recognize either.
    return any(uuid.lower() == SERVICE for uuid in ad.service_uuids) or any(
        uuid.lower() == SERVICE for uuid in ad.service_data)


async def find(args):
    if getattr(args, "address", None):
        return args.address
    name = getattr(args, "name", None)
    found = await BleakScanner.find_device_by_filter(
        lambda d, ad: advertises_sync_service(ad)
        and (name is None or d.name == name or ad.local_name == name),
        timeout=args.timeout)
    if not found:
        raise SystemExit("no device advertising the AQ sync service; is it powered and unpaired-or-bonded to this host?")
    print(f"BLE FOUND name={found.name} address={found.address}", flush=True)
    return found


def load_token(args) -> bytes:
    if getattr(args, "token", None):
        return bytes.fromhex(args.token)
    if getattr(args, "token_file", None):
        return bytes.fromhex(Path(args.token_file).read_text().strip())
    raise SystemExit("--lan needs --token <hex> or --token-file <path> (fetch it with the BLE `token` command)")


async def connect(args):
    if getattr(args, "lan", None):
        host, _, port = args.lan.partition(":")
        client = LanLink(host, load_token(args), int(port) if port else LAN_PORT, timeout=args.timeout)
        await client.connect()
    else:
        target = await find(args)
        client = BleakClient(target, timeout=args.timeout)
        await client.connect()
        print(f"BLE CONNECTED mtu={client.mtu_size}", flush=True)
    session = Session(client, read_timeout=args.timeout)
    await session.start()
    return client, session


ADVERT_FLAGS = ["no_utc", "new_files", "sd", "lan", "fail", "clk_restored"]


def decode_advert(payload):
    """Advertising service data, protocol v2.1 (docs/shared/ble-sync-protocol.md, 'Advertising payload').

    Ten little-endian bytes under the service UUID: version, flags, fin u32, boot16 u16, two spare.
    Returns a dict, or None for an unknown version / short payload (a pre-v2.1 device has none)."""
    if payload is None or len(payload) < 8 or payload[0] != 1:
        return None
    flags = payload[1]
    return {
        "ver": payload[0],
        "flags": [name for bit, name in enumerate(ADVERT_FLAGS) if flags & (1 << bit)],
        "fin": int.from_bytes(payload[2:6], "little"),
        "boot16": f"{int.from_bytes(payload[6:8], 'little'):04x}",
    }


async def cmd_scan(args):
    seen = {}

    def on_adv(d, ad):
        advert = decode_advert(ad.service_data.get(SERVICE))
        if advertises_sync_service(ad):
            seen[d.address] = (d.name, ad.rssi, advert)

    async with BleakScanner(on_adv):
        await asyncio.sleep(args.seconds)
    for address, (name, rssi, advert) in seen.items():
        extra = ""
        if advert is not None:
            extra = f" adv_ver={advert['ver']} flags={','.join(advert['flags']) or '-'} fin={advert['fin']} boot16={advert['boot16']}"
        print(f"BLE DEVICE name={name} address={address} rssi={rssi}{extra}")
    print(f"BLE SCAN END devices={len(seen)}")


async def cmd_info(args):
    client, session = await connect(args)
    try:
        print(json.dumps({"info": await session.info(), "status": await session.status()}, indent=2))
    finally:
        await client.disconnect()


async def cmd_live(args):
    client, session = await connect(args)
    deadline = time.monotonic() + args.seconds

    def show(kind):
        def handler(_c, data):
            print(f"{kind} {bytes(data).decode('utf-8', 'replace')}", flush=True)
        return handler

    try:
        print(f"LIVE {bytes(await session.read_char(LIVE)).decode('utf-8', 'replace')}")
        await client.start_notify(LIVE, show("LIVE"))
        await client.start_notify(STATUS, show("STATUS"))
        while time.monotonic() < deadline and client.is_connected:
            await asyncio.sleep(0.5)
    finally:
        await client.disconnect()


async def cmd_list(args):
    client, session = await connect(args)
    try:
        listed, end = await session.list_files()
        for name, size in sorted(listed.items()):
            print(f"BLE FILE name={name} bytes={size}")
        print("BLE LIST END " + " ".join(f"{k}={v}" for k, v in end.items()))
    finally:
        await client.disconnect()


def publish(payload: bytes, name: str, out: Path, transfer: dict, source_info: dict) -> dict:
    relative = name if name.startswith("legacy-parquet/") else "output/" + name
    summary = device.save_verified(payload, relative, out)
    return {"path": relative, "source": source_info.get("_transport", "ble") + "-readback", "bytes": len(payload), "rows": summary["rows"],
            "crc32": summary["crc32"], "sha256": hashlib.sha256(payload).hexdigest(),
            "readers_match": summary["readers_match"], "transfer": transfer,
            "station": source_info.get("station"), "device": source_info.get("dev")}


async def cmd_fetch(args):
    client, session = await connect(args)
    try:
        info = await session.info()
        info["_transport"] = "lan" if getattr(args, "lan", None) else "ble"
        payload, transfer = await session.fetch(device.safe_name(args.name), info.get("max_read", 4096))
        record = publish(payload, args.name, args.out, transfer, info)
        print(json.dumps(record), flush=True)
    finally:
        await client.disconnect()


async def cmd_sync(args):
    if (args.out / "manifest.json").exists():
        raise SystemExit("choose a new output directory; manifest already exists")
    client, session = await connect(args)
    records = {}
    started = time.monotonic()
    try:
        info = await session.info()
        info["_transport"] = "lan" if getattr(args, "lan", None) else "ble"
        listed, end = await session.list_files()
        print(f"LIST END count={end['count']} partials={end['partials']}", flush=True)
        names = sorted(listed)
        if args.limit:
            names = names[: args.limit]
        for name in names:
            payload, transfer = await session.fetch(name, info.get("max_read", 4096))
            if len(payload) != listed[name]:
                raise ProtocolError(f"listed {listed[name]} bytes, transferred {len(payload)}: {name}")
            records[name] = publish(payload, name, args.out, transfer, info)
            print(json.dumps(records[name]), flush=True)
    finally:
        await client.disconnect()
    total_bytes = sum(r["bytes"] for r in records.values())
    elapsed = time.monotonic() - started
    report = {"completed_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
              "transport": "lan" if getattr(args, "lan", None) else "ble",
              "protocol": info.get("proto"), "device": {k: v for k, v in info.items() if not k.startswith("_")}, "listing": end, "files": list(records.values()),
              "total_files": len(records), "total_bytes": total_bytes,
              "total_rows": sum(r["rows"] for r in records.values()),
              "wall_seconds": round(elapsed, 1), "bytes_per_s_incl_overhead": round(total_bytes / elapsed) if elapsed else None,
              "device_deletions": False, "flush_requested": False}
    args.out.mkdir(parents=True, exist_ok=True)
    with (args.out / "manifest.json").open("x") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(f"PASS {len(records)} files / {total_bytes} bytes in {elapsed:.1f}s -> {args.out}", flush=True)


async def cmd_time(args):
    client, session = await connect(args)
    try:
        epoch = int(time.time())
        print(json.dumps({"sent_epoch_s": epoch, "result": await session.set_time(epoch)}))
        await asyncio.sleep(1)
        print(json.dumps({"status": await session.status()}))
    finally:
        await client.disconnect()


async def cmd_flush(args):
    client, session = await connect(args)
    try:
        print(json.dumps({"flushed": await session.flush()}))
    finally:
        await client.disconnect()


async def cmd_config(args):
    client, session = await connect(args)
    try:
        print(json.dumps(await session.get_config(), indent=2))
    finally:
        await client.disconnect()


async def cmd_set(args):
    client, session = await connect(args)
    try:
        print(json.dumps(await session.set_config(args.pairs), indent=2))
    finally:
        await client.disconnect()


async def cmd_wifi_scan(args):
    client, session = await connect(args)
    try:
        networks = await session.wifi_scan()
        for network in sorted(networks, key=lambda n: -n["rssi"]):
            print(f"WIFI AP ssid={network['ssid']!r} rssi={network['rssi']} auth={network['auth']} channel={network['channel']}")
        print(f"WIFI SCAN END count={len(networks)}")
    finally:
        await client.disconnect()


async def cmd_token(args):
    client, session = await connect(args)
    try:
        port, token = await session.get_token()
        if args.save:
            args.save.parent.mkdir(parents=True, exist_ok=True)
            args.save.write_text(token.hex() + "\n")
            print(f"LAN TOKEN port={port} saved={args.save} (bearer secret: keep it out of docs and logs)")
        else:
            print(f"LAN TOKEN port={port} token={token.hex()}")
    finally:
        await client.disconnect()


async def cmd_log(args):
    client, session = await connect(args)
    try:
        text, meta = await session.log_tail(args.bytes)
        sys.stdout.write(text)
        if not text.endswith("\n"):
            sys.stdout.write("\n")
        print("LOG END " + " ".join(f"{k}={v}" for k, v in meta.items()))
    finally:
        await client.disconnect()


async def cmd_reboot(args):
    client, session = await connect(args)
    try:
        print(json.dumps({"rebooting": await session.reboot()}))
    finally:
        await client.disconnect()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--name", help="advertised local name, e.g. AQ-6b40")
    parser.add_argument("--address", help="skip scanning and connect to this address/UUID")
    parser.add_argument("--timeout", type=device.positive_seconds, default=20.0,
                        help="seconds per discovery, connection and characteristic read; "
                             "allow time for the system pairing dialog on first access")
    parser.add_argument("--lan", metavar="HOST[:PORT]", help="use the LAN transport (needs a token) instead of BLE")
    parser.add_argument("--token", help="LAN token as 64 hex characters")
    parser.add_argument("--token-file", help="file holding the LAN token hex (see `token --save`)")
    sub = parser.add_subparsers(dest="command", required=True)
    scan = sub.add_parser("scan")
    scan.add_argument("--seconds", type=float, default=8.0)
    scan.set_defaults(run=cmd_scan)
    sub.add_parser("info").set_defaults(run=cmd_info)
    sub.add_parser("status").set_defaults(run=cmd_info)
    live = sub.add_parser("live")
    live.add_argument("--seconds", type=float, default=35.0)
    live.set_defaults(run=cmd_live)
    sub.add_parser("list").set_defaults(run=cmd_list)
    fetch = sub.add_parser("fetch")
    fetch.add_argument("name")
    fetch.add_argument("--out", type=Path, required=True)
    fetch.set_defaults(run=cmd_fetch)
    sync = sub.add_parser("sync")
    sync.add_argument("--out", type=Path, required=True)
    sync.add_argument("--limit", type=int, default=0, help="only the first N listed files (bench)")
    sync.set_defaults(run=cmd_sync)
    sub.add_parser("time", help="send host UTC to the device (explicit write)").set_defaults(run=cmd_time)
    sub.add_parser("flush", help="finalize buffered rows on the device (explicit write)").set_defaults(run=cmd_flush)
    sub.add_parser("config", help="GET_CONFIG").set_defaults(run=cmd_config)
    setter = sub.add_parser("set", help="SET_CONFIG key=value ... (explicit write)")
    setter.add_argument("pairs", nargs="+", metavar="key=value")
    setter.set_defaults(run=cmd_set)
    sub.add_parser("wifi-scan", help="list Wi-Fi networks the device can see").set_defaults(run=cmd_wifi_scan)
    token = sub.add_parser("token", help="fetch the LAN token (BLE only)")
    token.add_argument("--save", type=Path, help="write the hex token to this file instead of printing it")
    token.set_defaults(run=cmd_token)
    log = sub.add_parser("log", help="LOG_TAIL: the device's recent serial log")
    log.add_argument("--bytes", type=int, default=8192)
    log.set_defaults(run=cmd_log)
    sub.add_parser("reboot", help="reboot the device after it finalizes its RAM batch (explicit)").set_defaults(run=cmd_reboot)
    args = parser.parse_args()
    if getattr(args, "out", None) is not None and "build" in args.out.parts:
        parser.error("use artifacts/, not a build directory that Arduino may clean")
    try:
        asyncio.run(args.run(args))
    except ProtocolError as error:
        print(f"FAIL {error}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
