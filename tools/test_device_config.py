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

from test_location import compile_h3


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
        location = root / "firmware/common/location/src"
        command += ["-I", str(Path(directory) / "h3"), "-I", str(location), str(location / "aq_location.cpp"),
                    *compile_h3(root, Path(directory), args.sanitize)]
        subprocess.run(command, check=True)
        for scenario in ("headless", "screen", "migration", "validation",
                         "credentials", "sensor", "nvs", "unavailable", "time-location"):
            result = subprocess.run([str(executable), scenario],
                                    capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(f"{scenario}: {result.stderr}")
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
