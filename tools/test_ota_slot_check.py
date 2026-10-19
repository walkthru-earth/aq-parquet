"""Host tests for ota_slot_check.py on crafted partition tables and images."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile

import ota_slot_check as check

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = Path(__file__).resolve().with_name("ota_slot_check.py")
BOARD_TABLES = (
    "firmware/esp-idf-cores3/bringup/partitions.csv",
    "firmware/esp-idf-waveshare-sim7670g/logger/partitions.csv",
    "firmware/esp-idf-waveshare-sim7670g/diagnostic/partitions.csv",
)
TABLE = """# Name, Type, SubType, Offset, Size, Flags
nvs,      data, nvs,   0x9000,  0x5000,
otadata,  data, ota,   0xe000,  0x2000,
app0,     app,  ota_0, 0x10000, {app0},
app1,     app,  ota_1, ,        {app1},
"""


def build(root: Path, image_bytes: int, *, app0: str = "0x1000", app1: str = "4K") -> tuple[Path, Path]:
    table = root / "partitions.csv"
    table.write_text(TABLE.format(app0=app0, app1=app1))
    out = root / "build"
    out.mkdir(exist_ok=True)
    (out / "project_description.json").write_text(json.dumps({"app_bin": "aq.bin"}))
    (out / "aq.bin").write_bytes(b"\xe9" * image_bytes)
    return table, out


def run(table: Path, out: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(SCRIPT), "--label", "cores3 (bringup)",
                           "--partitions", str(table), "--build", str(out)],
                          capture_output=True, text=True, timeout=60, check=False)


def rejected(result: subprocess.CompletedProcess[str], text: str) -> None:
    assert result.returncode != 0, result.stdout
    assert "OTA slot check failed" in result.stderr and text in result.stderr, result.stderr


def main() -> None:
    # Every board table names two OTA slots of 6 MiB today.
    for relative in BOARD_TABLES:
        assert check.app_slots((ROOT / relative).read_text()) == {"app0": 0x600000, "app1": 0x600000}, relative
    assert 0 < check.MAX_SHARE < 1

    assert check.parse_size("0x600000") == 0x600000
    assert check.parse_size("4K") == 4096 and check.parse_size("6M") == 6 * 1024 * 1024
    assert check.parse_size("4096") == 4096

    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        limit = int(4096 * check.MAX_SHARE)

        table, out = build(root, limit)
        result = run(table, out)
        assert result.returncode == 0, result.stderr
        assert "| cores3 (bringup) |" in result.stdout and f"| {limit:,} |" in result.stdout

        table, out = build(root, limit + 1)
        rejected(run(table, out), "over the")

        # The smallest app slot sets the limit, since the image must fit both.
        table, out = build(root, limit, app1="0x800")
        rejected(run(table, out), "app1")

        table, out = build(root, 0)
        rejected(run(table, out), "empty")

        # ESP-IDF reads a numeric type with int(x, 0), so 0x0000 is an app slot too.
        table, out = build(root, 100_000, app0="6M")
        table.write_text(table.read_text().replace("app1,     app,", "app1,     0x0000,"))
        rejected(run(table, out), "app1")
        assert check.app_slots("a, 0x0000, ota_0, , 4K,\nd, 0x01, nvs, , 4K,\n") == {"a": 4096}
        # Keywords match in any case, and custom data types up to 0xFE are not app slots.
        assert check.app_slots("a, APP, ota_0, , 4K,\nb, Bootloader, primary, , 4K,\n"
                               "t, partition_table, primary, , 4K,\nc, 0x40, 0x00, , 4K,\n"
                               "d, 0xFE, 0x00, , 4K,\n") == {"a": 4096}
        try:
            check.app_slots("a, app, ota_0, , 4K,\nb, 0xZZ, ota_1, , 4K,\n")
        except check.CheckError as error:
            assert "0xZZ" in str(error), error
        else:
            raise AssertionError("an unknown partition type must raise, not skip the row")

        # An app_bin that names a directory must not pass as an image.
        table, out = build(root, 16)
        (out / "project_description.json").write_text(json.dumps({"app_bin": "."}))
        rejected(run(table, out), "not a file")

        table, out = build(root, 16)
        table.unlink()
        rejected(run(table, out), "partitions.csv")

        malformed = {
            "no app rows": "nvs, data, nvs, 0x9000, 0x5000,\n",
            "empty file": "",
            "short row": "app0, app, ota_0, 0x10000\n",
            "text size": "app0, app, ota_0, 0x10000, big,\n",
            "zero size": "app0, app, ota_0, 0x10000, 0,\n",
            "negative size": "app0, app, ota_0, 0x10000, -4K,\n",
            "duplicate name": "app0, app, ota_0, , 4K,\napp0, app, ota_1, , 4K,\n",
            "unknown hex type": "app0, app, ota_0, , 4K,\napp1, 0xZZ, ota_1, , 4K,\n",
            "unknown type word": "app0, app, ota_0, , 4K,\napp1, aap, ota_1, , 4K,\n",
            "empty type": "app0, app, ota_0, , 4K,\napp1, , ota_1, , 4K,\n",
            "type over 0xFE": "app0, app, ota_0, , 4K,\napp1, 0x100, ota_1, , 4K,\n",
            "negative type": "app0, app, ota_0, , 4K,\napp1, -1, ota_1, , 4K,\n",
        }
        for name, text in malformed.items():
            table, out = build(root, 16)
            table.write_text(text)
            result = run(table, out)
            assert result.returncode != 0, name
            assert "OTA slot check failed" in result.stderr, f"{name}: {result.stderr}"

        table, out = build(root, 16)
        (out / "project_description.json").write_text("{truncated")
        rejected(run(table, out), "project_description.json")

        table, out = build(root, 16)
        (out / "aq.bin").unlink()
        rejected(run(table, out), "aq.bin")

    print("ota_slot_check.py host tests passed")


if __name__ == "__main__":
    main()
