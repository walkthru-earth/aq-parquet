# aq-parquet

ESP32-S3 air-quality firmware with reusable sampling, Parquet, settings, Bluetooth and Wi-Fi/LAN synchronization. The **M5Stack CoreS3** and **Waveshare ESP32-S3-SIM7670G-4G V2.0** each have a native ESP-IDF application and separate hardware documentation. Application source uses no Arduino APIs or Arduino component. Both applications implement writing real measurements directly to microSD as Parquet; host tools retrieve and validate the files without conversion.

Each board owns its pins, peripherals, schema and acquisition callbacks. Five opt-in ESP-IDF components under [`firmware/common/`](firmware/common/README.md) provide encoding/drivers (**AQCommon**), settings/logging (**AQRuntime**), radios and sync (**AQConnectivity**), the sampling/storage worker (**AQLogger**), and official H3 geometry (**AQLocation**). Both logger adapters consume these components. Shared contracts live in `docs/shared/`; wiring and measured evidence live in `docs/boards/<board>/`.

- `AGENTS.md` is the entry point for humans and coding agents.
- `docs/` routes to [shared contracts](docs/README.md#shared-contracts) and [board references](docs/README.md#boards), including the [telemetry and Parquet pipeline](docs/shared/telemetry-pipeline.md).
- `product/` holds the [mobile app workflows](product/mobile-app.md), [architecture and privacy](product/mobile-architecture.md), and [Bluetooth SQL idea](product/bluetooth-parquet.md); these are proposals, not implemented features.
- The [community extension shortlist](product/community-extension-shortlist.md) maps DuckDB candidates to these product ideas, with adoption limits and source references.
- `firmware/<framework>-<variant>/` holds each self-contained trial.

## Current status

| Native firmware | Measurements and board adapter | Hardware evidence |
| --- | --- | --- |
| [CoreS3 v6.9](firmware/esp-idf-cores3/README.md) | 81 columns; native M5Unified/M5GFX, RTC, display and shared SPI2 card arbitration | Short native SD/USB readback, network UTC and retained settings/bonds verified; phone transfers/endurance remain unqualified; [bench record](docs/boards/m5stack-cores3/bench-verified.md#board-1-first-native-esp-idf-flash-and-readback) |
| [Waveshare logger v2.3](firmware/esp-idf-waveshare-sim7670g/README.md) | 56 columns; native UART PMS5003T, I²C gauge, one-bit SDMMC and OPI PSRAM | Native image requires bench qualification; earlier Arduino image results remain in the [bench record](docs/boards/waveshare-esp32-s3-sim7670g/bench-verified.md) |

Both source adapters implement the existing offline archive, configuration, BLE
and authenticated local TCP/mDNS contract. The native migration retained the existing measurement fields; new schemas
append four nullable H3 location fields. It retains persistent station/config NVS keys and types, and flash partition
offsets. Existing BLE bond compatibility is not guaranteed; native bonded
reconnect or re-pairing needs a hardware check. Cellular/GNSS/camera integration
remains future board work. See the [component map](firmware/common/README.md).

Automatic time tries router DHCP option 42, configured local NTP hosts, then
permitted public fallback. RTC and phone anchors remain available offline.
Location is unset until explicit provisioning: the mobile app sends an H3 cell
at a configurable precision (default maximum resolution 5), firmware derives
its center, and optional owner-declared country is included in Parquet metadata.
Location changes split immutable files. See [time and location](docs/shared/time-and-location.md).

The toolchain pins **ESP-IDF 6.1**. Managed components pin **M5Unified 0.2.25** and
**M5GFX 0.2.31** for CoreS3, **mDNS 1.14.0** for connectivity, and **led_strip
3.1.0~1** for Waveshare; vendored LZ4 remains 1.10.0 and H3 is pinned to 4.5.0.
Pins were checked on **2026-10-09**. M5GFX 0.2.32 is now available; the
[development guide](docs/shared/development.md#host-and-toolchain) records why
this hardware qualification retains 0.2.31. [ESP-IDF 6.1 documentation](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/get-started/index.html)
and [development workflow](docs/shared/development.md) describe the native build.
[Native board/fixture builds and host gates passed on 2026-10-09](docs/shared/development.md#native-migration-validation-2026-10-09).
CoreS3 also passed a [short native hardware readback](docs/boards/m5stack-cores3/bench-verified.md#board-1-first-native-esp-idf-flash-and-readback); Waveshare remains unqualified on hardware.
Historical footprint, throughput and durability observations below apply only
to their recorded Arduino images and do not establish native hardware behavior.
[Iceberg/OGC decisions](docs/shared/table-and-observation-model.md) describe
later host/cloud work; dictionaries do not claim SensorThings API compliance.

An eight-row queue feeds a separate storage task and a bounded PSRAM batch. Default rotation is **15 minutes / up to 90 rows**, configurable to **10 minutes / up to 60 rows** or (firmware v6) **30 / 60 minutes with two / four 90-row row groups per file**. The writer emits immutable Parquet without an Arrow runtime, with **UNCOMPRESSED** and opt-in **LZ4_RAW** codecs, per-column min/max statistics and `TIMESTAMP(NANOS, UTC)` on the UTC fields. Reboot restores uncompressed output. Persistent station identity and UTC-aligned Hive partitions use:

```text
output/station=<UUID>/year=YYYY/month=MM/day=DD/
  data_HHMM_<boot>_<first>-<last>-<attempt>.parquet
```

Measured on one board/card: a full automatic 60-row batch was **28,059 bytes** and finalized in **122 ms**, with no recorded drops or missed deadlines. A later full **90-row uncompressed Hive file was 39,869 bytes** and also passed both readers with zero recorded health errors. See the dated [bench record](docs/boards/m5stack-cores3/bench-verified.md) for image identities and scope, and the [compression experiment](docs/boards/m5stack-cores3/compression-benchmark.md) for codec comparisons.

**What the evidence establishes:** direct on-device Parquet is feasible for this workload and its files interoperate with PyArrow/DuckDB without host conversion. On the same 60 rows, three LZ4 comparisons produced **14,080-byte files versus 28,537 bytes uncompressed (50.7% smaller)** and **93.0 ms versus 118.4 ms median finalization (21.4% faster)**. That supports choosing LZ4 for this tested logger; it is not a claim of superiority over every format, card, codec or cloud architecture. Filesystem allocated space, energy use and production durability have not been compared.

Offline logging needs no internet. The measured 60-row LZ4 rate projects to about **2.03 MB/day / 0.74 GB/year before filesystem overhead** at ten-minute rotation. A 32 GB card therefore offers multi-year storage capacity in principle, not a guaranteed card or battery lifetime. [Capacity assumptions and future synchronization](docs/shared/telemetry-pipeline.md#offline-capacity-and-reconnection) explain allocation-unit overhead, power, clock drift and the uploader that still needs to be built.

**Feasibility, not production durability:** unfinished RAM rows are lost on reset; interrupted partial files are quarantined at boot but not repaired. The [security and production-hardening boundary](docs/shared/security-hardening.md) documents implemented controls and unimplemented protections. Power-cut recovery, upload, cloud compaction and Iceberg remain open; Snappy/Zstd results are host-only. UTC comes from an explicit host anchor, successful native SNTP callback, or CoreS3 RTC restore, without a measured accuracy guarantee. CoreS3 can restore its earlier estimate from its RTC; Waveshare needs a new host or network anchor after reboot. Until an anchor exists, files use `unsynced/boot=<boot>/` with null UTC. Unsupported measurements remain null; the Waveshare PMS5003T supplies ambient temperature/humidity. Camera/audio streams are outside these scalar loggers.

## Getting started

Choose the [CoreS3](firmware/esp-idf-cores3/README.md) or
[Waveshare V2](firmware/esp-idf-waveshare-sim7670g/README.md) adapter. Build through
Pixi and ESP-IDF from the command line:

```sh
pixi install
pixi run idf-setup
pixi run cores3-build
pixi run waveshare-build              # logger
pixi run waveshare-build diagnostic   # retained UART/read-only TF fixture
```

Before any physical flash write, follow [AGENTS.md](AGENTS.md#before-any-physical-flash-write)
on the exact board and checked serial port, in this order:

```sh
pixi run ports
pixi run chip --port <checked-port>
pixi run flash-id --port <checked-port>
pixi run efuse --port <checked-port>
pixi run backup --board <board> --port <checked-port>
ls -l backup/   # confirm the full backup is exactly 16777216 bytes
```

Use `m5stack-cores3` or `waveshare-sim7670g-v2` for `<board>`. The chip command
can reset a running app. Flash tasks require both the checked port and the
matching full backup; they flash the verified build without erasing NVS:

```sh
pixi run cores3-flash <checked-port> backup/m5stack-cores3-flash-<timestamp>.bin
pixi run waveshare-flash <checked-port> backup/waveshare-sim7670g-v2-flash-<timestamp>.bin
```

After native flashing, qualify serial startup, sensor/null behavior, SD writes,
clock transition and immutable USB/BLE/LAN readback on each board before
claiming native hardware behavior. Host tools retain their protocol:

```sh
pixi run parquet-device sync-time --port <checked-port>
pixi run parquet-device command --port <checked-port> 'parquet status'
pixi run parquet-device command --port <checked-port> 'parquet list'
pixi run parquet-test --sanitize
```

Only one host process owns a serial port at a time. Keep retained binaries,
captures and fetched files in the selected trial's ignored `artifacts/`, outside
the rebuildable `build/`; full-card owner copies belong in ignored `exports/`.

The SDK installer uses `$AQ_TOOLCHAIN_ROOT`, default
`~/.cache/m5stack-aq-parquet/toolchains`; `$M5_TOOLCHAIN_ROOT` remains accepted
for the existing shared cache. The firmware wrapper isolates ESP-IDF Python and
compiler tools from pinned Pixi host validators. See the [development guide](docs/shared/development.md).

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
