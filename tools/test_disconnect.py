"""Pin protocol 3 BYE/DIAG/ABORTED golden frames from the real firmware codec.

Host contract checks; no radio behavior is established.
Run: pixi run disconnect-test
"""
from __future__ import annotations

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

import ble_sync as client

# Mirrored in docs/shared/ble-sync-protocol.md, "Protocol 3 contract"
# (#protocol-3-contract).
GOLDEN_BYE = "4b0104030201f5ff0300100e0000"
GOLDEN_EVENT = "78000000070000000302ffff00000200"
GOLDEN_DIAG = ("4c0200" + "0000" + "0500" + "0000" + "0100" + "0000" * 13
               + "01" + GOLDEN_EVENT)
GOLDEN_ABORTED = ["4d00", "4d03"]
# LIST_END: count u32 70000, partials 1, sd_kib 15558144, used_kib 1024.
GOLDEN_LIST_END = "117011010001000066ed0000040000"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    runtime = root / "firmware/common/runtime/src"
    connectivity = root / "firmware/common/connectivity/src"
    with tempfile.TemporaryDirectory(prefix="aq-disconnect-test-") as directory:
        executable = Path(directory) / "disconnect-test"
        command = [
            "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-I", str(root / "tools/host_fakes/config"), "-I", str(runtime),
            "-I", str(root / "firmware/common/src"), "-I", str(connectivity),
            str(root / "firmware/common/connectivity/tests/test_disconnect.cpp"),
            str(connectivity / "disconnect.cpp"), str(runtime / "debug_log.cpp"),
            str(root / "tools/host_fakes/config/aq_console.cpp"),
            "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        lines = subprocess.check_output([str(executable)], text=True).split()
    assert lines == [GOLDEN_BYE, GOLDEN_EVENT, GOLDEN_DIAG, *GOLDEN_ABORTED], lines

    bye = client.decode_bye(bytes.fromhex(GOLDEN_BYE))
    assert bye == {"reason": "idle-timeout", "reason_code": 1, "generation": 0x01020304,
                   "err": -11, "queued_frames": 3, "uptime_s": 3600}
    assert len(bytes.fromhex(GOLDEN_BYE)) + 2 == 16  # LAN length prefix
    diag = client.decode_diag(bytes.fromhex(GOLDEN_DIAG))
    counts = [0] * 17
    counts[1], counts[3] = 5, 1
    assert diag == {"boot_seq": 2, "counts": counts,
                    "events": [{"uptime_s": 120, "generation": 7, "reason": "auth-failed",
                                "reason_code": 3, "link": 2, "link_name": "lan2", "err": -1,
                                "queued_frames": 0, "boot_seq": 2}]}
    list_end = bytes.fromhex(GOLDEN_LIST_END)
    assert len(list_end) == 15 and list_end[0] == client.F_LIST_END
    assert struct.unpack_from("<IHII", list_end, 1) == (70000, 1, 15558144, 1024)
    assert len(client.DISCONNECT_REASONS) == 17
    print("Disconnect: 16-byte event, BYE/DIAG/ABORTED/LIST_END golden vectors, "
          "DIAG truncation 61/29 and persistent counters passed")


if __name__ == "__main__":
    main()
