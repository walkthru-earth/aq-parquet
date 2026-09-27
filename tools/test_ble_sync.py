"""Frame-level tests for tools/ble_sync.py against docs/shared/ble-sync-protocol.md v2.

Run: pixi run python tools/test_ble_sync.py
A fake client answers control writes with scripted response frames; nothing
touches a radio. This proves the host parser's reading of the contract, not
the firmware.
"""
import asyncio
import binascii
import struct

import ble_sync as ble

NAME = "station=abc/unsynced/boot=def/data_unsynced_def_0-1-0.parquet"
FILE = b"PAR1" + bytes(range(256)) * 3 + struct.pack("<I", 40) + b"PAR1"
CONFIG = (b'{"ble":{"pair":"random","pin_set":1,"pin_default":0,"bonds":1,"display":1},'
          b'"wifi":{"on":0,"ssid":"","psk_set":0,"state":"off","ip":"","rssi":0,"mac":"aa"},'
          b'"lan":{"on":1,"port":47390,"host":"aq-6b40","clients":0}}')
TOKEN = bytes(range(32))
LOG = b"PARQUET BEGIN x\n" + b"BLE CONNECT peer=y\n" * 20
INFO = b'{"proto":2,"max_read":16384,"station":"abc","dev":"0123456789ab"}'
STATUS = b'{"part":3,"qf":7,"qb":123456,"open":0,"open_rg":0}'


class FakeClient:
    """Scripted device: LIST of two files, one openable file served in chunks."""

    def __init__(self, chunk=100, drop_frame=False):
        self.chunk = chunk
        self.callback = None
        self.handle = 0
        self.drop_frame = drop_frame
        self.mtu_size = 517

    async def start_notify(self, _uuid, callback):
        self.callback = callback

    def emit(self, frame: bytes):
        self.callback(None, bytearray(frame))

    async def read_gatt_char(self, uuid):
        if uuid == ble.INFO:
            return bytearray(INFO)
        return bytearray(STATUS if uuid == ble.STATUS else b'{"buf":1}')

    async def write_gatt_char(self, _uuid, data, response=True):
        op = data[0]
        if op == ble.OP_LIST:
            self.emit(bytes([ble.F_FILE]) + struct.pack("<I", len(FILE)) + NAME.encode())
            self.emit(bytes([ble.F_FILE]) + struct.pack("<I", 7) + b"legacy-parquet/x.parquet")
            self.emit(bytes([ble.F_LIST_END]) + struct.pack("<HHII", 2, 0, 1000, 10))
        elif op == ble.OP_OPEN:
            name = data[1:].decode()
            if name != NAME:
                self.emit(bytes([ble.F_ERROR, op, 3]) + b"not finalized")
                return
            self.handle += 1
            crc = binascii.crc32(FILE) & 0xFFFFFFFF
            self.emit(bytes([ble.F_OPENED]) + struct.pack("<HII", self.handle, len(FILE), crc) + name.encode())
        elif op == ble.OP_READ:
            handle, offset, length = struct.unpack_from("<HII", data, 1)
            end = min(len(FILE), offset + length)
            first = True
            for position in range(offset, end, self.chunk):
                if self.drop_frame and first:
                    first = False
                    continue  # simulate a lost notification: parser must reject
                self.emit(bytes([ble.F_CHUNK]) + struct.pack("<HI", handle, position) + FILE[position:min(end, position + self.chunk)])
            self.emit(bytes([ble.F_READ_END]) + struct.pack("<HIB", handle, end, 0))
        elif op == ble.OP_CLOSE:
            (handle,) = struct.unpack_from("<H", data, 1)
            self.emit(bytes([ble.F_CLOSED]) + struct.pack("<H", handle))
        elif op == ble.OP_SET_TIME:
            (epoch,) = struct.unpack_from("<q", data, 1)
            self.emit(bytes([ble.F_TIME_SET]) + struct.pack("<qq", epoch, 123456))
        elif op == ble.OP_FLUSH:
            self.emit(bytes([ble.F_ERROR, op, 10]) + b"empty")
        elif op == ble.OP_GET_CONFIG:
            self.emit(bytes([ble.F_CONFIG, 0]) + CONFIG)
        elif op == ble.OP_SET_CONFIG:
            lines = data[1:].decode().split("\n")
            if any(line.startswith("ble.pin=") and len(line) != len("ble.pin=") + 6 for line in lines):
                self.emit(bytes([ble.F_ERROR, op, 12]) + b"ble.pin")
                return
            self.emit(bytes([ble.F_CONFIG, 1]) + CONFIG.replace(b'"random"', b'"fixed"'))
        elif op == ble.OP_WIFI_SCAN:
            self.emit(bytes([ble.F_WIFI_AP]) + struct.pack("<bBB", -58, 3, 6) + b"home")
            self.emit(bytes([ble.F_WIFI_AP]) + struct.pack("<bBB", -80, 0, 11) + b"cafe")
            self.emit(bytes([ble.F_WIFI_SCAN_END]) + struct.pack("<HB", 2, 0))
        elif op == ble.OP_GET_TOKEN:
            self.emit(bytes([ble.F_TOKEN]) + struct.pack("<H", 47390) + TOKEN)
        elif op == ble.OP_LOG_TAIL:
            (max_bytes,) = struct.unpack_from("<H", data, 1)
            text = LOG[-max_bytes:]
            for position in range(0, len(text), 50):
                self.emit(bytes([ble.F_LOG]) + text[position:position + 50])
            self.emit(bytes([ble.F_LOG_END]) + struct.pack("<IH", len(LOG), len(text)))
        elif op == ble.OP_REBOOT:
            self.emit(bytes([ble.F_REBOOTING]) + struct.pack("<H", 500))


async def run():
    client = FakeClient()
    session = ble.Session(client)
    await session.start()
    status = await session.status()
    assert status["qf"] == 7 and status["qb"] == 123456 and status["part"] == 3, status
    listed, end = await session.list_files()
    assert listed == {NAME: len(FILE), "legacy-parquet/x.parquet": 7}, listed
    assert end["count"] == 2 and end["sd_kib"] == 1000
    payload, transfer = await session.fetch(NAME, 250)
    assert payload == FILE
    assert transfer["windows"] == -(-len(FILE) // 250), transfer
    result = await session.set_time(1_800_000_000)
    assert result == {"epoch_s": 1_800_000_000, "monotonic_us": 123456}
    try:
        await session.flush()
    except ble.ProtocolError as error:
        assert "code=10" in str(error) and "nothing-to-flush" in str(error), error
    else:
        raise AssertionError("ERROR frame must raise")
    try:
        await session.open("station=abc/other.parquet")
    except ble.ProtocolError as error:
        assert "code=3" in str(error)
    else:
        raise AssertionError("OPEN of unknown file must raise")
    # A lost CHUNK must be detected by the absolute offset check, never patched over.
    lossy = ble.Session(FakeClient(drop_frame=True))
    await lossy.start()
    try:
        await lossy.fetch(NAME, 250)
    except ble.ProtocolError as error:
        assert "CHUNK offset" in str(error), error
    else:
        raise AssertionError("missing chunk must fail")
    # v2: configuration, scan, token, log, reboot.
    config = await session.get_config()
    assert config["ble"]["pair"] == "random" and config["reboot_required"] is False, config
    changed = await session.set_config(["ble.pair=fixed", "ble.pin=654321"])
    assert changed["ble"]["pair"] == "fixed" and changed["reboot_required"] is True, changed
    try:
        await session.set_config(["ble.pin=12"])
    except ble.ProtocolError as error:
        assert "code=12" in str(error) and "ble.pin" in str(error), error
    else:
        raise AssertionError("invalid config must raise")
    networks = await session.wifi_scan()
    assert [n["ssid"] for n in networks] == ["home", "cafe"] and networks[0]["auth"] == "WPA2", networks
    port, token = await session.get_token()
    assert port == 47390 and token == TOKEN
    text, meta = await session.log_tail(100)
    assert text.encode() == LOG[-100:] and meta["total_bytes_since_boot"] == len(LOG), meta
    assert await session.reboot() == {"delay_ms": 500}
    print("PASS ble_sync frame codec: list, fetch, time, error, lost-chunk detection, config, scan, token, log, reboot")
    await run_lan()


async def run_lan():
    """A scripted TCP device: handshake, HELLO, framed requests, pushes between CHUNKs."""
    fake = FakeClient(chunk=200)
    pushed = []

    async def serve(reader, writer):
        head = await reader.readexactly(36)
        if head != b"AQS1" + TOKEN:
            writer.write(struct.pack("<H", 3) + bytes([ble.F_ERROR, 0, 14]))
            await writer.drain()
            writer.close()
            return
        hello = bytes([ble.F_HELLO, 2]) + struct.pack("<H", 1024) + INFO
        writer.write(struct.pack("<H", len(hello)) + hello)
        emitted = []
        fake.callback = lambda _c, data: emitted.append(bytes(data))
        while True:
            try:
                (length,) = struct.unpack("<H", await reader.readexactly(2))
                request = await reader.readexactly(length)
            except asyncio.IncompleteReadError:
                return
            emitted.clear()
            await fake.write_gatt_char(None, request)
            for index, frame in enumerate(emitted):
                writer.write(struct.pack("<H", len(frame)) + frame)
                if index == 0 and request[0] == ble.OP_READ:
                    live = bytes([ble.F_LIVE]) + b'{"seq":1}'
                    writer.write(struct.pack("<H", len(live)) + live)  # push mid-transfer
            await writer.drain()

    server = await asyncio.start_server(serve, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    link = ble.LanLink("127.0.0.1", TOKEN, port)
    await link.connect()
    assert link.payload_max == 1024 and link.mtu_size - 3 == 1024
    await link.start_notify(ble.LIVE, lambda _c, data: pushed.append(bytes(data)))
    session = ble.Session(link)
    await session.start()
    assert (await session.info())["proto"] == 2
    listed, _ = await session.list_files()
    payload, transfer = await session.fetch(NAME, 1000)
    assert payload == FILE and transfer["windows"] == 1, transfer
    assert pushed == [b'{"seq":1}'], pushed
    assert await link.read_gatt_char(ble.LIVE) == bytearray(b'{"seq":1}')
    await link.disconnect()
    bad = ble.LanLink("127.0.0.1", bytes(32), port)
    try:
        await bad.connect()
    except ble.ProtocolError as error:
        assert "code=14" in str(error), error
    else:
        raise AssertionError("wrong token must be refused")
    server.close()
    await server.wait_closed()
    print("PASS ble_sync LAN link: handshake, HELLO, framed fetch with LIVE push interleaved, auth refusal")


if __name__ == "__main__":
    asyncio.run(run())
