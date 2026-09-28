"""Test physical UART provisioning with real validators/control actions and SDK fakes."""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    common = root / "firmware/common"
    with tempfile.TemporaryDirectory(prefix="aq-logger-provision-") as directory:
        executable = Path(directory) / "provision-test"
        command = ["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                   "-I", str(root / "tools/host_fakes/config")]
        for library in ["src", "runtime/src", "connectivity/src", "logger/src"]:
            command += ["-I", str(common / library)]
        command += [str(common / "logger/tests/test_provision.cpp"),
                    str(common / "logger/src/aq_logger_provision.cpp"),
                    str(common / "runtime/src/device_config.cpp"),
                    str(common / "runtime/src/debug_log.cpp"),
                    str(common / "connectivity/src/control_sync.cpp"),
                    "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    print("UART provisioning: UTF-8 hex round-trip, secrecy, validators, actions and bounds passed")


if __name__ == "__main__":
    main()
