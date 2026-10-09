#!/usr/bin/env python3
"""Validate pinned native SDK, board target, legacy partitions and flash artifacts."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FLASH_BYTES = 16 * 1024 * 1024
PARTITIONS = [
    ("nvs", 1, 2, 0x9000, 0x5000, 0),
    ("otadata", 1, 0, 0xE000, 0x2000, 0),
    ("app0", 0, 0x10, 0x10000, 0x600000, 0),
    ("app1", 0, 0x11, 0x610000, 0x600000, 0),
    ("coredump", 1, 3, 0xC10000, 0x10000, 0),
    ("spiffs", 1, 0x82, 0xC20000, 0x3E0000, 0),
]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def sdk_lock() -> dict[str, str]:
    return dict(re.findall(r"^(ESP_IDF_[A-Z]+)=(\S+)$",
                          (ROOT / "tools/idf-dependencies.lock").read_text(), re.M))


def git(sdk: Path, *arguments: str) -> str:
    return subprocess.check_output(["git", "-C", str(sdk), *arguments], text=True).rstrip()


def check_sdk(sdk: Path, allow_missing: bool = False) -> str:
    expected = sdk_lock()["ESP_IDF_COMMIT"]
    require((sdk / "tools/idf.py").is_file(), "ESP-IDF checkout is incomplete")
    require(git(sdk, "rev-parse", "HEAD") == expected, "ESP-IDF commit differs from lock")
    # Ignore untracked build/download files, but never consume edited SDK code.
    require(not git(sdk, "status", "--porcelain", "--untracked-files=no",
                    "--ignore-submodules=none"), "ESP-IDF or a submodule has local changes")
    modules = git(sdk, "submodule", "status", "--recursive").splitlines()
    require(all(line.startswith(" ") or (allow_missing and line.startswith("-"))
                for line in modules), "ESP-IDF submodule missing or at a different commit")
    return expected


def partitions(data: bytes) -> list[tuple]:
    entries = []
    for offset in range(0, len(data), 32):
        entry = data[offset:offset + 32]
        if entry[:2] == b"\xeb\xeb":
            require(len(entry) == 32 and entry[16:] == hashlib.md5(data[:offset]).digest(),
                    "partition table MD5 mismatch")
            return entries
        if entry[:2] == b"\xff\xff":
            return entries
        require(len(entry) == 32 and entry[:2] == b"\xaa\x50", "invalid partition table")
        _, kind, subtype, start, size, label, flags = struct.unpack("<HBBII16sI", entry)
        entries.append((label.split(b"\0", 1)[0].decode(), kind, subtype, start, size, flags))
    raise ValueError("unterminated partition table")


def build_file(build: Path, name: str) -> Path:
    path = (build / name).resolve()
    require(path.is_relative_to(build) and path.is_file(), "missing/outside-build artifact: " + name)
    return path


def check_target(build: Path, board: str, variant: str) -> tuple[str, dict[str, str]]:
    require((board == "cores3") == (variant == "bringup"), "board and variant disagree")
    trial = ROOT / "firmware" / ("esp-idf-cores3" if board == "cores3"
                                 else "esp-idf-waveshare-sim7670g")
    lines = set(build_file(build, "sdkconfig").read_text().splitlines())
    required = ["CONFIG_IDF_TARGET_ESP32S3=y", "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y",
                "CONFIG_ESPTOOLPY_FLASHMODE_QIO=y", "CONFIG_PARTITION_TABLE_CUSTOM=y",
                "CONFIG_PARTITION_TABLE_OFFSET=0x8000", "CONFIG_FATFS_LFN_HEAP=y",
                "CONFIG_FATFS_MAX_LFN=255", "CONFIG_FREERTOS_HZ=1000",
                'CONFIG_ESPTOOLPY_FLASHFREQ="80m"',
                f'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="{variant}/partitions.csv"']
    if variant == "diagnostic":
        required.append("# CONFIG_SPIRAM is not set")
        require("CONFIG_BT_ENABLED=y" not in lines and "CONFIG_BT_NIMBLE_ENABLED=y" not in lines,
                "diagnostic unexpectedly enables Bluetooth")
    else:
        mode = "QUAD" if board == "cores3" else "OCT"
        required += ["CONFIG_SPIRAM=y", f"CONFIG_SPIRAM_MODE_{mode}=y",
                     "CONFIG_SPIRAM_SPEED_80M=y", "CONFIG_SPIRAM_USE_MALLOC=y",
                     "CONFIG_BT_ENABLED=y", "CONFIG_BT_NIMBLE_ENABLED=y",
                     "CONFIG_BT_NIMBLE_SECURITY_ENABLE=y", "CONFIG_BT_NIMBLE_SM_SC=y",
                     "CONFIG_BT_NIMBLE_SM_SC_ONLY=1", "CONFIG_BT_NIMBLE_NVS_PERSIST=y",
                     "# CONFIG_BT_NIMBLE_SM_LEGACY is not set",
                     "# CONFIG_BT_NIMBLE_SM_SC_DEBUG_KEYS is not set"]
    required += (["CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y"] if board == "cores3" else
                 ["CONFIG_ESP_CONSOLE_UART_DEFAULT=y", "CONFIG_ESP_CONSOLE_UART_NUM=0",
                  "CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200"])
    for setting in required:
        require(setting in lines, f"native target missing {setting}")
    require("CONFIG_ESP_CONSOLE_USB_CDC=y" not in lines, "unexpected USB CDC console")
    commands = json.loads(build_file(build, "compile_commands.json").read_text())
    require(bool(commands), "empty compiler database")
    require(any(Path(entry["file"]).resolve() == trial / variant / "main.cpp" for entry in commands),
            "compiler database does not include the selected board application")
    for entry in commands:
        arguments = entry.get("arguments") or shlex.split(entry["command"])
        command = " ".join(arguments)
        require(not re.search(r"(?:^|\s)-D\s*ARDUINO(?:\b|_)", command) and
                "/arduino-esp32/" not in command.lower(), "native compiler includes Arduino")
    description = json.loads(build_file(build, "project_description.json").read_text())
    require(Path(description["project_path"]).resolve() == trial,
            "build belongs to a different board project")
    require(Path(description["build_dir"]).resolve() == build, "build directory provenance mismatch")
    require(Path(description["config_file"]).resolve() == build / "sdkconfig",
            "build configuration provenance mismatch")
    require(description["target"] == "esp32s3", "project target is not ESP32-S3")
    sdk_commit = check_sdk(Path(description["idf_path"]))
    require(description["git_revision"] == "v" + sdk_lock()["ESP_IDF_VERSION"],
            "build used a different SDK revision")
    components = description["build_components"]
    if variant == "diagnostic":
        require("connectivity" not in components and "logger" not in components,
                "diagnostic unexpectedly includes application/radio runtime")
    else:
        require({"logger", "connectivity", "runtime", "common"}.issubset(components),
                "logger runtime component missing")
    require(not any("arduino" in component.lower() for component in components),
            "native target depends on Arduino")
    if board == "waveshare":
        require(not any("m5" in name.lower() for name in components),
                "Waveshare depends on M5 drivers")
    else:
        require({"m5stack__m5unified", "m5stack__m5gfx"}.issubset(components),
                "CoreS3 native M5 components missing")
    require(partitions(build_file(build, "partition_table/partition-table.bin").read_bytes()) ==
            PARTITIONS, "partition table differs from the existing owner-data layout")
    plan = json.loads(build_file(build, "flasher_args.json").read_text())
    expected = {0: "bootloader/bootloader.bin", 0x8000: "partition_table/partition-table.bin",
                0xE000: "ota_data_initial.bin", 0x10000: description["app_bin"]}
    actual = {int(offset, 0): name for offset, name in plan["flash_files"].items()}
    require(actual == expected, "unexpected flash plan; only bootloader/table/OTA metadata/app0 allowed")
    settings = plan["flash_settings"]
    # IDF deliberately flashes a DIO-readable bootloader even for runtime QIO.
    require(settings["flash_size"] == "16MB" and settings["flash_mode"] in {"dio", "qio"} and settings["flash_freq"] == "80m",
            "unexpected flasher memory settings")
    require(plan["extra_esptool_args"]["chip"] == "esp32s3", "unexpected flasher chip")
    limits = {0: 0x8000, 0x8000: 0x1000, 0xE000: 0x2000, 0x10000: 0x600000}
    for offset, name in actual.items():
        size = build_file(build, name).stat().st_size
        require(0 < size <= limits[offset], "flash artifact exceeds its permitted region: " + name)
    expected_flags = {"--flash-mode": settings["flash_mode"],
                      "--flash-size": settings["flash_size"],
                      "--flash-freq": settings["flash_freq"]}
    json_args = plan["write_flash_args"]
    require(len(json_args) == 6 and dict(zip(json_args[::2], json_args[1::2])) == expected_flags,
            "unexpected flash erase/encryption/options")
    # idf.py flash consumes flash_args; check it agrees with the JSON plan.
    arguments = shlex.split(build_file(build, "flash_args").read_text())
    require(len(arguments) == 14, "unexpected arguments in the actual flash response file")
    flags, images = {}, {}
    for option, value in zip(arguments[::2], arguments[1::2]):
        if option.startswith("--"):
            flags[option] = value
        else:
            images[int(option, 0)] = value
    require(flags == expected_flags and images == expected,
            "flash response file disagrees with the checked flash plan")
    files = set(actual.values()) | {"sdkconfig", "compile_commands.json",
                                  "project_description.json", "flasher_args.json", "flash_args"}
    hashes = {name: hashlib.sha256(build_file(build, name).read_bytes()).hexdigest()
              for name in sorted(files)}
    return sdk_commit, hashes


def check_backup(backup: Path, board: str) -> None:
    prefix = "m5stack-cores3" if board == "cores3" else "waveshare-sim7670g-v2"
    require(backup.name.startswith(prefix + "-flash-") and backup.suffix == ".bin",
            "backup does not match the selected board")
    require(backup.stat().st_size == FLASH_BYTES, "backup must be exactly 16777216 bytes")
    with backup.open("rb") as source:
        source.seek(0x8000)
        previous = partitions(source.read(0xC00))
    # The backup may predate the app partition sizes; preserve the persistent
    # namespace region in every supported native firmware transition.
    require([entry for entry in previous if entry[1:3] == (1, 2)] == [PARTITIONS[0]],
            "backup NVS region differs; native migration needs explicit layout planning")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, nargs="?")
    parser.add_argument("--board", choices=["cores3", "waveshare"])
    parser.add_argument("--variant", choices=["bringup", "logger", "diagnostic"])
    parser.add_argument("--sdk-only", type=Path)
    parser.add_argument("--allow-missing-submodules", action="store_true")
    parser.add_argument("--backup", type=Path)
    manifests = parser.add_mutually_exclusive_group()
    manifests.add_argument("--write-manifest", action="store_true")
    manifests.add_argument("--verify-manifest", action="store_true")
    args = parser.parse_args()
    try:
        if args.sdk_only:
            check_sdk(args.sdk_only.resolve(), args.allow_missing_submodules)
            print("Verified pinned ESP-IDF SDK and submodule identity")
            return
        require(args.build is not None and args.board is not None and args.variant is not None,
                "build, --board and --variant are required")
        build = args.build.resolve()
        commit, hashes = check_target(build, args.board, args.variant)
        manifest = {"board": args.board, "variant": args.variant, "sdk_commit": commit, "files": hashes}
        manifest_path = build / "aq-build-sha256.json"
        if args.verify_manifest:
            require(json.loads(manifest_path.read_text()) == manifest,
                    "reviewed build artifacts changed; build and verify again")
        if args.write_manifest:
            temporary = manifest_path.with_suffix(".json.partial")
            temporary.write_text(json.dumps(manifest, indent=2) + "\n")
            temporary.replace(manifest_path)
        if args.backup:
            check_backup(args.backup, args.board)
        print(f"Verified native ESP32-S3 {args.board}/{args.variant}: "
              "SDK, memory, console, components, partitions and flash plan")
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"native target rejected: {error}") from error


if __name__ == "__main__":
    main()
