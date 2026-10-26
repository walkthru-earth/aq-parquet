"""Host tests for repro_paths.py, the manifest compare behind repro-paths-check.

It needs no SDK. The builds come from crafted os-build-sha256.json manifests
and image files, and the shell wrapper is checked for the calls it relies on.
"""

from __future__ import annotations

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile

import repro_paths

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools/repro_paths.py"
WRAPPER = ROOT / "tools/check_repro_paths.sh"
MANIFEST = {
    "board": "waveshare", "variant": "logger", "sdk_commit": "a" * 40,
    "files": {"opensensor_waveshare.bin": "1" * 64, "bootloader/bootloader.bin": "2" * 64,
              "sdkconfig": "3" * 64, "compile_commands.json": "4" * 64,
              "project_description.json": "5" * 64},
}


def changed(**edits: object) -> dict:
    manifest = copy.deepcopy(MANIFEST)
    for key, value in edits.items():
        if key == "files":
            manifest["files"].update(value)
        else:
            manifest[key] = value
    return manifest


def run(root: Path, first: dict, second: dict, images: tuple[bytes, bytes] | None = None):
    builds = []
    for index, manifest in enumerate((first, second)):
        build = root / f"checkout-{index}" / "build"
        build.mkdir(parents=True)
        (build / "os-build-sha256.json").write_text(json.dumps(manifest))
        if images:
            (build / "opensensor_waveshare.bin").write_bytes(images[index])
        builds.append(build)
    return subprocess.run([sys.executable, str(SCRIPT), str(root / "checkout-0"), str(builds[0]),
                           str(root / "checkout-1"), str(builds[1])],
                          capture_output=True, text=True, timeout=60, check=False)


def main() -> None:
    assert repro_paths.compare(MANIFEST, copy.deepcopy(MANIFEST)) == []
    # The compiler database and project description hold absolute paths by design.
    assert repro_paths.compare(MANIFEST, changed(files={"compile_commands.json": "6" * 64,
                                                        "project_description.json": "7" * 64})) == []
    assert repro_paths.compare(MANIFEST, changed(files={"opensensor_waveshare.bin": "8" * 64})) == \
        ["opensensor_waveshare.bin differs"]
    assert repro_paths.compare(MANIFEST, changed(files={"sdkconfig": "9" * 64})) == ["sdkconfig differs"]
    assert any("board" in problem for problem in repro_paths.compare(MANIFEST, changed(board="cores3")))
    assert any("sdk_commit" in problem for problem in repro_paths.compare(MANIFEST, changed(sdk_commit="b")))
    extra = changed(files={"ota_data_initial.bin": "0" * 64})
    assert any("file lists differ" in problem for problem in repro_paths.compare(MANIFEST, extra))
    assert repro_paths.compare(MANIFEST, {**MANIFEST, "files": None}) != []
    assert repro_paths.compare({"files": {}}, {"files": {}}) != [], "an empty manifest must not pass"
    only_paths = {"files": {"compile_commands.json": "4" * 64}}
    assert repro_paths.compare(only_paths, copy.deepcopy(only_paths)) != [], \
        "a manifest with only path bound files must not pass"

    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        result = run(root / "same", MANIFEST, changed(files={"compile_commands.json": "6" * 64}))
        assert result.returncode == 0, result.stdout + result.stderr
        assert "waveshare/logger" in result.stdout and "opensensor_waveshare.bin" in result.stdout

        # A leaked checkout path is named, with the count of differing bytes.
        leaked = root / "leak"
        image_a = b"\xe9" * 64 + str(leaked / "checkout-0").encode()
        image_b = b"\xe9" * 64 + str(leaked / "checkout-1").encode()
        result = run(leaked, MANIFEST, changed(files={"opensensor_waveshare.bin": "8" * 64}), (image_a, image_b))
        assert result.returncode == 1, result.stdout
        assert "opensensor_waveshare.bin differs" in result.stdout, result.stdout
        assert "checkout 1 path in build 1" in result.stdout, result.stdout
        assert "first at offset 0x" in result.stdout, result.stdout

        missing = root / "missing"
        (missing / "checkout-0" / "build").mkdir(parents=True)
        (missing / "checkout-1" / "build").mkdir(parents=True)
        result = subprocess.run([sys.executable, str(SCRIPT),
                                 str(missing / "checkout-0"), str(missing / "checkout-0" / "build"),
                                 str(missing / "checkout-1"), str(missing / "checkout-1" / "build")],
                                capture_output=True, text=True, timeout=60, check=False)
        assert result.returncode != 0 and "os-build-sha256.json" in result.stderr, result.stderr

    # The wrapper must pin the build time, build twice and fail on a compare failure.
    wrapper = WRAPPER.read_text()
    assert "set -euo pipefail" in wrapper
    assert "export SOURCE_DATE_EPOCH" in wrapper
    assert wrapper.count("worktree add --detach") == 1 and 'for checkout in "${first}" "${second}"' in wrapper
    assert wrapper.rstrip().splitlines()[-3].startswith('python "${repo_root}/tools/repro_paths.py"'), \
        "the compare must be the last command, so its exit status is the script's"

    print("repro_paths.py host tests passed")


if __name__ == "__main__":
    main()
