# Documentation router

The device protocol, telemetry format, and offline-first data flow are shared across board trials. Wiring, power, peripheral ownership, build settings, and measured performance belong to the individual board. Source-checked claims and real-device results remain separate.

## Shared contracts

| Topic | Reference |
| --- | --- |
| Host tooling and shared modules | [Development workflow](shared/development.md), [shared component map](../firmware/common/README.md) |
| New ESP32-S3 board hooks and capabilities | [Board porting](shared/board-porting.md), [AQLogger adapter contract](../firmware/common/logger/README.md) |
| Shared logger worker, board measurement/RTC hooks and immutable archives | [AQLogger adapter contract](../firmware/common/logger/README.md) |
| Sampling, nulls, Parquet, Hive partitions, and offline archives | [Telemetry pipeline](shared/telemetry-pipeline.md) |
| Dictionary, observation semantics, and proposed Iceberg workflow | [Table and observation model](shared/table-and-observation-model.md) |
| Small file sizes, encodings, compaction and range reads | [Small Parquet files](shared/parquet-small-files.md) |
| Proposed STAC metadata contract, firmware audit and review (research, not implemented) | [STAC contract research](shared/stac-contract/README.md) |
| Proto 3 BLE/LAN frames, disconnect evidence, pairing and UART owner provisioning | [Sync protocol](shared/ble-sync-protocol.md) |
| Router/local NTP, configurable H3 privacy and mobile deployment provisioning | [Time and location](shared/time-and-location.md) |
| Phone background sync triggers | [Background sync triggers](shared/background-sync-triggers.md) |
| Security and production-hardening boundary | [Security hardening](shared/security-hardening.md) |

## Boards

| Board | Status | Reference |
| --- | --- | --- |
| M5Stack CoreS3 with optional M134/PMSA003 | Native AQLogger adapter with a short native USB/SD/sensor/time readback. Proto 3 phone transfers and endurance remain pending. Historical Arduino radio evidence stays image-specific | [CoreS3 board docs](boards/m5stack-cores3/README.md), [trial](../firmware/esp-idf-cores3/README.md) |
| Waveshare ESP32-S3-SIM7670G-4G V2 with PMS5003T | Native ESP-IDF logger and retained diagnostic; native hardware unverified, historical Arduino evidence in its bench record | [Waveshare board docs](boards/waveshare-esp32-s3-sim7670g/README.md), [trial](../firmware/esp-idf-waveshare-sim7670g/README.md) |

The five shared ESP-IDF components are AQCommon (encoding/drivers), AQRuntime (settings/logs),
AQConnectivity (local transports) AQLogger (sampling/archive worker) and AQLocation (official H3 geometry). A shared
source or host gate is not a hardware endurance result.

For any physical write, follow the board-specific safety sequence in [AGENTS.md](../AGENTS.md). Keep captures and firmware binaries in the relevant trial's ignored `artifacts/`, and record real-board measurements only in that board's bench record.
