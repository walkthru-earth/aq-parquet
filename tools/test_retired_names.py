"""Host tests for check_retired_names.py and the way tools/lint.sh reads its exit code (#93)."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile

import check_retired_names as rn

OLD = "A" + "Q"
FIRES = [
    "mdns_service_add(nullptr, \"_" + "aq" + "sync\", \"_tcp\", 47390);",
    "constexpr char kMagic[] = \"" + OLD + "S1\";",
    "BASE = \"C0A5" + "E9F0-{:04x}-4b1a-9c3e-2d7f8a6b4e01\"",
    "| Service | " + OLD + " Sync |",
    "nvs_open(\"" + "aq" + "cfg\", NVS_READWRITE, &handle);",
    "The `" + "aq" + "dump` namespace holds one record.",
    "CONFIG_" + OLD + "_LOGGER_WORKER_WDT=y",
    "config " + OLD + "_PM_DFS_MIN_MHZ",
    "std::printf(\"" + OLD + " STATUS ok\\n\");",
    "if line.startswith('" + OLD + " LIVE'):",
    "Look for `" + OLD + " BOOT` in the log.",
    OLD + " WIFI connected",
    "std::printf(\"%s " + OLD + " READY\\n\", prefix);",
    "ESP_LOGI(kTag, \"boot " + OLD + " TF mounted\");",
    "log('[' + now + '] " + OLD + " OWNER_PIN set')",
]
QUIET = [
    "mdns_service_add(nullptr, \"_ossync\", \"_tcp\", 47390);",
    "constexpr char kMagic[] = \"OSS1\";",
    "nvs_open(\"oscfg\", NVS_READWRITE, &handle);",
    "CONFIG_OS_LOGGER_WORKER_WDT=y",
    "std::printf(\"OS STATUS ok\\n\");",
    "\"AD AE AF AG AI AL AM AO " + OLD + " AR AS AT\"",
    "for (const char *code : {\"EG\", \"US\", \"" + OLD + "\"})",
    "PARQUET LIST ok",
    "aq_logger.cpp and " + OLD + "_BENCH stay, only wire names moved.",
    "#ifndef " + OLD + "_SHARED_H",
    "constexpr char kIso[] = \"AN AO " + OLD + " AR AS\";",
    "Tested near " + OLD + " READY state in prose without a literal.",
    "printf(\"%s OS READY\\n\", prefix);",
]


def check_patterns() -> None:
    for line in FIRES:
        assert len(rn.scan_text(line)) == 1, line
    for line in QUIET:
        assert rn.scan_text(line) == [], line


def check_files() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "docs/shared").mkdir(parents=True)
        (root / "docs/shared/mobile-migration.md").write_text(FIRES[0] + "\n")
        (root / "docs/shared/new.md").write_text("ok\n" + FIRES[1] + "\n")
        (root / "pixi.lock").write_text(FIRES[0] + "\n")
        names = ["docs/shared/mobile-migration.md", "docs/shared/new.md", "pixi.lock", "gone.md"]
        # History and lock files are exempt, a missing path is skipped.
        assert rn.findings(root, names) == ["docs/shared/new.md:2: retired LAN handshake"]


def check_repo() -> None:
    # The tracked tree is clean, and the checker scans itself without a hit.
    names = rn.tracked(rn.ROOT)
    assert names is not None and "tools/check_retired_names.py" in names
    assert rn.findings(rn.ROOT, names) == []


def check_exit_codes() -> None:
    script = Path(__file__).resolve().parent / "check_retired_names.py"
    with tempfile.TemporaryDirectory() as tmp:
        bad = Path(tmp) / "bad.md"
        bad.write_text(FIRES[4] + "\n")
        result = subprocess.run([sys.executable, str(script), str(bad)], capture_output=True, text=True)
        assert result.returncode == 1 and "retired NVS namespace" in result.stdout, result
        good = Path(tmp) / "good.md"
        good.write_text(QUIET[0] + "\n")
        result = subprocess.run([sys.executable, str(script), str(good)], capture_output=True, text=True)
        assert result.returncode == 0 and result.stdout == "", result
    # Outside any repo git cannot list files, which must not read as clean.
    with tempfile.TemporaryDirectory() as tmp:
        assert rn.tracked(Path(tmp)) is None


def main() -> None:
    check_patterns()
    check_files()
    check_repo()
    check_exit_codes()
    print(f"PASS retired names, {len(FIRES)} fire, {len(QUIET)} quiet, history exempt, exit codes 0 and 1, an unlisted repo is never clean")


if __name__ == "__main__":
    main()
