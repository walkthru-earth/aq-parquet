# aq-parquet

ESP32-S3 air-quality firmware with reusable sampling, Parquet, settings, Bluetooth and Wi-Fi/LAN synchronization. The **M5Stack CoreS3** and **Waveshare ESP32-S3-SIM7670G-4G V2.0** each have an active Arduino firmware and separate hardware documentation. Both write real measurements directly to microSD as Parquet; host tools retrieve and validate the files without conversion.

Each board owns its pins, peripherals, schema and acquisition callbacks. Four opt-in libraries under [`firmware/common/`](firmware/common/README.md) provide encoding/drivers (**AQCommon**), settings/logging (**AQRuntime**), radios and sync (**AQConnectivity**), and the sampling/storage worker (**AQLogger**). Both logger adapters consume these libraries. Shared contracts live in `docs/shared/`; wiring and measured evidence live in `docs/boards/<board>/`.

- `AGENTS.md` is the entry point for humans and coding agents.
- `docs/` routes to [shared contracts](docs/README.md#shared-contracts) and [board references](docs/README.md#boards), including the [telemetry and Parquet pipeline](docs/shared/telemetry-pipeline.md).
- `product/` holds the [mobile app workflows](product/mobile-app.md), [architecture and privacy](product/mobile-architecture.md), and [Bluetooth SQL idea](product/bluetooth-parquet.md); these are proposals, not implemented features.
- The [community extension shortlist](product/community-extension-shortlist.md) maps DuckDB candidates to these product ideas, with adoption limits and source references.
- `firmware/<framework>-<variant>/` holds each self-contained trial.

## Current status

| Firmware | Measurements and board adapter | Current hardware evidence |
| --- | --- | --- |
| [CoreS3 v6.5](firmware/arduino-m5unified/README.md) | 77 columns; M5Unified sensors, RTC, display and SPI card arbitration | Shared engine sampling, unattended startup and identical USB/BLE/LAN Parquet readback; [bench record](docs/boards/m5stack-cores3/bench-verified.md) |
| [Waveshare logger v1](firmware/arduino-waveshare-sim7670g/README.md) | 49 columns; PMS5003T UART, one-bit SDMMC, OPI PSRAM, headless pairing | Real SD readback, warm-up/null handling, reset retention, host UTC transition and compression; [bench record](docs/boards/waveshare-esp32-s3-sim7670g/bench-verified.md) |

Both implement the same offline archive, configuration, BLE and authenticated local TCP/mDNS services. Waveshare first encrypted macOS pairing and Wi-Fi/LAN transfer still need completion; advertising discovery is verified. It runs on USB power with battery fields null, and has no external RTC anchor. Cellular/GNSS/camera integration remains separate future board work. See the [module map](firmware/common/README.md) and [official Waveshare resource audit](docs/boards/waveshare-esp32-s3-sim7670g/software-resources.md).

The CoreS3 dependencies include Arduino-ESP32 3.3.11, M5Unified 0.2.21 and M5GFX 0.2.28. The Waveshare trial pins its own Arduino/NimBLE dependencies and has no M5 library dependency. Earlier CoreS3 full-window/compression numbers below remain tied to their recorded images. [Iceberg/OGC decisions and contract usage](docs/shared/table-and-observation-model.md) describe later host/cloud work; the dictionaries do not claim SensorThings API compliance.

An eight-row queue feeds a separate storage task and a bounded PSRAM batch. Default rotation is **15 minutes / up to 90 rows**, configurable to **10 minutes / up to 60 rows** or (firmware v6) **30 / 60 minutes with two / four 90-row row groups per file**. The writer emits immutable Parquet without an Arrow runtime, with **UNCOMPRESSED** and opt-in **LZ4_RAW** codecs, per-column min/max statistics and `TIMESTAMP(NANOS, UTC)` on the UTC fields. Reboot restores uncompressed output. Persistent station identity and UTC-aligned Hive partitions use:

```text
output/station=<UUID>/year=YYYY/month=MM/day=DD/
  data_HHMM_<boot>_<first>-<last>-<attempt>.parquet
```

Measured on one board/card: a full automatic 60-row batch was **28,059 bytes** and finalized in **122 ms**, with no recorded drops or missed deadlines. A later full **90-row uncompressed Hive file was 39,869 bytes** and also passed both readers with zero recorded health errors. See the dated [bench record](docs/boards/m5stack-cores3/bench-verified.md) for image identities and scope, and the [compression experiment](docs/boards/m5stack-cores3/compression-benchmark.md) for codec comparisons.

**What the evidence establishes:** direct on-device Parquet is feasible for this workload and its files interoperate with PyArrow/DuckDB without host conversion. On the same 60 rows, three LZ4 comparisons produced **14,080-byte files versus 28,537 bytes uncompressed (50.7% smaller)** and **93.0 ms versus 118.4 ms median finalization (21.4% faster)**. That supports choosing LZ4 for this tested logger; it is not a claim of superiority over every format, card, codec or cloud architecture. Filesystem allocated space, energy use and production durability have not been compared.

Offline logging needs no internet. The measured 60-row LZ4 rate projects to about **2.03 MB/day / 0.74 GB/year before filesystem overhead** at ten-minute rotation. A 32 GB card therefore offers multi-year storage capacity in principle, not a guaranteed card or battery lifetime. [Capacity assumptions and future synchronization](docs/shared/telemetry-pipeline.md#offline-capacity-and-reconnection) explain allocation-unit overhead, power, clock drift and the uploader that still needs to be built.

**Feasibility, not production durability:** unfinished RAM rows are lost on reset; interrupted partial files are quarantined at boot but not repaired. The [security and production-hardening boundary](docs/shared/security-hardening.md) documents implemented controls and unimplemented protections. Power-cut recovery, upload, cloud compaction and Iceberg remain open; Snappy/Zstd results are host-only. UTC is a host-supplied estimate. CoreS3 can restore its earlier estimate from its RTC; Waveshare needs a new host anchor after reboot. Until an anchor exists, files use `unsynced/boot=<boot>/` with null UTC. Unsupported measurements remain null; the Waveshare PMS5003T supplies ambient temperature/humidity. Camera/audio streams are outside these scalar loggers.

## Getting started

Choose the [CoreS3 board](docs/boards/m5stack-cores3/README.md) or [Waveshare V2 board](docs/boards/waveshare-esp32-s3-sim7670g/README.md). The shared safety readback precedes any firmware write. The Waveshare [trial README](firmware/arduino-waveshare-sim7670g/README.md) covers its logger, headless pairing and retained diagnostic. Commands below after the safety sequence show the CoreS3 logger; the same serial readback helper accepts either checked ESP32 port.

```sh
pixi install
pixi run ports     # find the board
pixi run chip --port <checked-port>      # confirm ESP32-S3; may reset the running app
pixi run flash-id --port <checked-port>  # confirm flash size
pixi run efuse --port <checked-port>     # read-only security and flash-type check
pixi run backup --board <board> --port <checked-port>  # full flash image before the first write
ls -l backup/     # confirm the image is exactly 16777216 bytes
```

Read the "Do not brick the board" section of `AGENTS.md` before flashing anything.

Build the active firmware with `pixi run arduino-setup` and `pixi run arduino-build`. Flash only after the safety sequence, using an explicit checked port:

```sh
pixi run arduino-flash /dev/cu.usbmodem101
```

Then supply time and inspect the logger, using the checked port and only one serial process at a time:

```sh
pixi run parquet-device sync-time --port /dev/cu.usbmodem101
pixi run parquet-device command --port /dev/cu.usbmodem101 'parquet status'
pixi run parquet-device command --port /dev/cu.usbmodem101 'parquet list'
pixi run parquet-test --sanitize
```

The [trial README](firmware/arduino-m5unified/README.md#inspect-the-live-logger) has bounded capture, flush, fetch and DuckDB query commands. Prefer its Parquet serial helper during logging: serial control-line settings caused unwanted resets in the initial host implementation and were corrected for this macOS/CoreS3 pair. Do not reset merely to read data.

Keep exports and captures in the trial's git-ignored **`artifacts/`**, never `build/`: Arduino rebuilds can clean their build directory. `pixi run python tools/export_parquet.py --port <port> --out firmware/arduino-m5unified/artifacts/exports/<new-name>` retrieves every listed finalized file, including legacy files on the current firmware, without deleting or flushing device data.

Firmware SDKs are not conda packages, so the project fetches them itself at pinned versions. A clean machine needs `pixi install` and then the setup task for whichever trial you are building, with no manual SDK installation. SDKs land in `$AQ_TOOLCHAIN_ROOT`, default `~/.cache/m5stack-aq-parquet/toolchains` (the existing shared cache path; `M5_TOOLCHAIN_ROOT` remains accepted), deliberately outside the repo so git worktrees share one copy. See the [shared development and board-extension guide](docs/shared/development.md).

## Hardware

Current boards: [M5Stack CoreS3](docs/boards/m5stack-cores3/README.md) (K128, 16 MB flash, 8 MB Quad PSRAM) and [Waveshare ESP32-S3-SIM7670G-4G V2](docs/boards/waveshare-esp32-s3-sim7670g/README.md) (16 MB flash, 8 MB OPI PSRAM, PMS5003T on GPIO1/2). Sensor modules are optional; the standalone PMS5003T and M134/PMSA003 have different frame data and are documented separately.

## License and attribution

Licensed under [CC BY 4.0](LICENSE), matching other Walkthru.Earth repositories.

Exception: vendored [LZ4 1.10.0](firmware/common/vendor/lz4/README.md) retains its BSD-2-Clause license and upstream notices.

**If you use any of this work, you must credit us visibly.** Attribution belongs
somewhere a reader actually sees it, such as your README, your documentation, your
about screen, or your paper. A buried comment in source does not count.

Minimum credit.

> Based on [walkthru-earth/aq-parquet](https://github.com/walkthru-earth/aq-parquet)
> by Walkthru.Earth, licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).

BibTeX.

```bibtex
@software{walkthru_aq_parquet,
  author  = {Youssef Harby, Myagmarjargal Mendbayar},
  title   = {aq-parquet: ESP32-S3 air-quality Parquet firmware},
  year    = {2026},
  url     = {https://github.com/walkthru-earth/aq-parquet},
  license = {CC-BY-4.0},
  note    = {Walkthru.Earth}
}
```

State what you changed if you modified the work. Attribution does not imply we endorse your project.
