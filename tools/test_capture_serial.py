"""Host test for tools/capture_serial.py send-on-cue, with a fake serial port.

Run with ``pixi run capture-serial-test``. The fake port replays scripted
chunks and records every write and every open, so the test proves a cue is
sent on the one open port, once, in order, and only after its pattern. No
serial port or board is touched.
"""

from __future__ import annotations

import contextlib
import io
from pathlib import Path
import sys
import tempfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_serial as cap  # noqa: E402


def require(condition: bool, description: str) -> None:
    if not condition:
        raise AssertionError(description)


class FakePort:
    """Replays chunks, one per read, then reads nothing. Writes are recorded."""

    opens = 0

    def __init__(self, chunks: list[bytes], fail_writes: int = 0) -> None:
        self.chunks = list(chunks)
        self.writes: list[bytes] = []
        self.fail_writes = fail_writes
        self.closed = False

    def read(self, size: int) -> bytes:
        require(not self.closed, "read after close")
        return self.chunks.pop(0)[:size] if self.chunks else b""

    def write(self, data: bytes) -> int:
        if self.fail_writes:
            self.fail_writes -= 1
            raise OSError("device disconnected")
        # A board that answers the stall like the bench image does.
        self.writes.append(data)
        if data.startswith(b"parquet bench-stall "):
            self.chunks.append(b"PARQUET BENCH stall_s=" + data[20:].strip() + b"\n")
        return len(data)

    def flush(self) -> None:
        pass

    def reset_input_buffer(self) -> None:
        pass

    def close(self) -> None:
        self.closed = True


def run(argv: list[str], port: FakePort) -> tuple[int, str, str]:
    FakePort.opens = 0
    stdout, stderr = io.StringIO(), io.StringIO()

    def open_port(*_args, **_kwargs) -> FakePort:
        FakePort.opens += 1
        return port

    with patch.object(cap.serial, "Serial", side_effect=open_port), \
            patch.object(sys, "argv", ["capture_serial.py", "--port", "/dev/fake", *argv]), \
            contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        code = cap.main()
    return code, stdout.getvalue(), stderr.getvalue()


def cue_sender_unit() -> None:
    cues = cap.CueSender([("PARQUET ROW", "parquet bench-stall 0")])
    require(cues.due(b"boot\nPARQUET RO") == [], "a partial pattern must not fire")
    fired = cues.due(b"boot\nPARQUET ROW seq=0\n")
    require(fired == [(b"PARQUET ROW", "parquet bench-stall 0")], "split pattern across reads must fire")
    require(cues.due(b"boot\nPARQUET ROW seq=0\nPARQUET ROW seq=1\n") == [], "a cue fires once")

    # Two cues on the same pattern need two separate lines after each send.
    cues = cap.CueSender([("ROW", "parquet flush"), ("ROW", "parquet status")])
    collected = bytearray(b"ROW ROW\n")
    require([c for _, c in cues.due(collected)] == ["parquet flush"], "one cue per call")
    require(cues.due(collected) == [], "a line seen before the send must not fire the next cue")
    collected += b"ROW\n"
    require([c for _, c in cues.due(collected)] == ["parquet status"], "the next cue waits for new output")
    require(not cues.pending, "all cues sent")

    # A failed write puts the cue back, it waits for its pattern again.
    cues = cap.CueSender([("ROW", "parquet flush")])
    collected = bytearray(b"ROW\n")
    cues.requeue(cues.due(collected), collected)
    require(cues.pending == [(b"ROW", "parquet flush")], "requeue restores the cue")
    require(cues.due(collected) == [], "a requeued cue ignores output it already saw")

    for bad in [("", "parquet flush"), ("ROW", ""), ("ROW", "parquet flush\nparquet list"),
                ("ROW", "parquet owner-pin"), ("ROW", " parquet wifi-profile"),
                ("ROW", "parquet config-hex 0102")]:
        try:
            cap.CueSender([bad])
        except ValueError:
            continue
        raise AssertionError(f"{bad!r} must be refused")


def capture_sends_on_cue() -> None:
    port = FakePort([b"ESP-ROM:esp32s3\n", b"PARQUET WATCHDOG mode=armed\nPARQUET RO",
                     b"W seq=0\n", b"PARQUET ROW seq=1\n"])
    with tempfile.TemporaryDirectory(prefix="aq-capture-") as directory:
        out = Path(directory) / "stall.log"
        code, stdout, stderr = run(["--seconds", "2", "--send-on", "PARQUET ROW", "parquet bench-stall 0",
                                    "--until", "PARQUET BENCH stall_s=0", "--out", str(out)], port)
        require(code == 0, f"capture failed with {code}, {stderr}")
        require(FakePort.opens == 1, "the port must be opened once, a second open resets the Waveshare")
        require(port.writes == [b"parquet bench-stall 0\n"], f"unexpected writes {port.writes}")
        require("PARQUET BENCH stall_s=0" in out.read_text(), "the stall reply must reach --out")
        require("PARQUET BENCH stall_s=0" in stdout, "the stall reply must reach stdout")
        require("[capture] sent 'parquet bench-stall 0'" in stderr, "the send must be reported")
        require(port.closed, "port closed at exit")


def capture_ordered_cues() -> None:
    port = FakePort([b"PARQUET ROW seq=0\n", b"PARQUET ROW seq=1\n", b"PARQUET ROW seq=2\n"])
    code, _, stderr = run(["--seconds", "1", "--send-on", "PARQUET ROW", "parquet flush",
                           "--send-on", "PARQUET ROW", "parquet status"], port)
    require(code == 0, f"capture failed with {code}, {stderr}")
    require(port.writes == [b"parquet flush\n", b"parquet status\n"], f"unexpected writes {port.writes}")


def capture_missing_cue() -> None:
    port = FakePort([b"boot only\n"])
    code, _, stderr = run(["--seconds", "0.5", "--send-on", "PARQUET ROW", "parquet bench-stall 0"], port)
    require(code == 3, f"a cue that never fired must fail, got {code}")
    require(port.writes == [], "nothing is written without the cue")
    require("never saw 'PARQUET ROW'" in stderr, "the missing cue must be named")


def capture_retries_failed_write() -> None:
    port = FakePort([b"PARQUET ROW seq=0\n", b"PARQUET ROW seq=1\n"], fail_writes=1)
    code, _, stderr = run(["--seconds", "1", "--send-on", "PARQUET ROW", "parquet bench-stall 5"], port)
    require(code == 0, f"capture failed with {code}, {stderr}")
    require("could not send" in stderr, "the failed write must be reported")
    require(port.writes == [b"parquet bench-stall 5\n"], "the cue is sent on the next pattern")


def capture_refuses_secret() -> None:
    port = FakePort([b"PARQUET ROW\n"])
    code, _, stderr = run(["--seconds", "0.5", "--send-on", "PARQUET ROW", "parquet owner-pin"], port)
    require(code == 2 and FakePort.opens == 0, "a secret command must be refused before the port opens")
    require("refusing" in stderr, "the refusal must be explained")


def capture_without_cue_writes_nothing() -> None:
    port = FakePort([b"PARQUET ROW seq=0\n"])
    code, _, _ = run(["--seconds", "0.5"], port)
    require(code == 0 and port.writes == [], "a plain capture never writes")


def main() -> None:
    cue_sender_unit()
    capture_sends_on_cue()
    capture_ordered_cues()
    capture_missing_cue()
    capture_retries_failed_write()
    capture_refuses_secret()
    capture_without_cue_writes_nothing()
    print("Capture serial: send on cue over one open, split patterns, ordered cues, missing cue exit, "
          "failed write retry, secret command refusal and plain capture passed")


if __name__ == "__main__":
    main()
