#!/usr/bin/env python3
"""Check the actual Arduino compiler target and selected ESP32-S3 memory SDK.

The copied build/sdkconfig can describe the SDK's default memory variant;
compile_commands.json records the include path used by this build instead.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("fqbn")
    parser.add_argument("--memory", choices=["qio_opi", "qio_qspi"], required=True)
    args = parser.parse_args()
    options = json.loads((args.build / "build.options.json").read_text())
    if options["fqbn"] != args.fqbn:
        raise SystemExit("build FQBN does not match its declared target")
    commands = json.loads((args.build / "compile_commands.json").read_text())
    suffix = f"/{args.memory}/include"
    headers = {Path(value[2:]) / "sdkconfig.h"
               for command in commands for value in command["arguments"]
               if value.startswith("-I") and value.endswith(suffix)}
    if len(headers) != 1:
        raise SystemExit("expected one selected ESP32-S3 memory SDK include path")
    config = next(iter(headers)).read_text()
    mode = "OCT" if args.memory == "qio_opi" else "QUAD"
    for macro in ["CONFIG_ESPTOOLPY_FLASHSIZE_16MB", f"CONFIG_SPIRAM_MODE_{mode}"]:
        if f"#define {macro} 1" not in config:
            raise SystemExit(f"selected SDK does not define {macro}")
    print(f"Verified FQBN and selected 16 MB flash / {mode} PSRAM SDK")


if __name__ == "__main__":
    main()
