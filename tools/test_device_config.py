"""Exercise real shared configuration/log code against host platform fakes.

NVS puts commit separately; failure tests establish runtime atomicity and error
reporting, not transactional persistence across keys or power-loss durability.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    shared = root / "firmware/common/runtime/src"
    with tempfile.TemporaryDirectory(prefix="aq-config-test-") as directory:
        executable = Path(directory) / "config-test"
        command = [
            "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-I", str(root / "tools/host_fakes/config"), "-I", str(shared),
            str(root / "tools/test_device_config.cpp"),
            str(shared / "device_config.cpp"), str(shared / "debug_log.cpp"),
            str(root / "tools/host_fakes/config/aq_console.cpp"),
            "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        for scenario in ("headless", "screen", "migration", "validation",
                         "credentials", "sensor", "nvs", "unavailable"):
            result = subprocess.run([str(executable), scenario], check=True,
                                    capture_output=True, text=True)
            if scenario == "credentials":
                document = json.loads(result.stdout)
                assert document["wifi"]["ssid"] == 'fixture"\\name'
                assert document["wifi"]["psk_set"] == 1
                assert "pin" not in document["ble"]
                assert "psk" not in document["wifi"]
                assert "token" not in document["lan"]
    print("Shared configuration: defaults, migration, validation, secrecy and NVS failures passed")


if __name__ == "__main__":
    main()
