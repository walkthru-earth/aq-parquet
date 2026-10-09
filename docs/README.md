# Documentation router

The device protocol, telemetry format, and offline-first data flow are shared across board trials. Wiring, power, peripheral ownership, build settings, and measured performance belong to the individual board. Source-checked claims and real-device results remain separate.

## Shared contracts

| Topic | Reference |
| --- | --- |
| Host tooling, shared modules and adding another board | [Development workflow](shared/development.md), [shared component map](../firmware/common/README.md) |
| Shared logger worker, board measurement/RTC hooks and immutable archives | [AQLogger adapter contract](../firmware/common/logger/README.md) |
| Sampling, nulls, Parquet, Hive partitions, and offline archives | [Telemetry pipeline](shared/telemetry-pipeline.md) |
| Dictionary, observation semantics, and proposed Iceberg workflow | [Table and observation model](shared/table-and-observation-model.md) |
| BLE/LAN frames, 480-byte snapshots, pairing and physical UART owner provisioning | [Sync protocol](shared/ble-sync-protocol.md) |
| Router/local NTP, configurable H3 privacy and mobile deployment provisioning | [Time and location](shared/time-and-location.md) |
| Phone background sync triggers | [Background sync triggers](shared/background-sync-triggers.md) |
| Security and production-hardening boundary | [Security hardening](shared/security-hardening.md) |

## Boards

| Board | Status | Reference |
| --- | --- | --- |
| M5Stack CoreS3 with optional M134/PMSA003 | Native ESP-IDF logger adapter consumes AQLogger; native hardware unverified, historical Arduino SD/radio evidence in its bench record | [CoreS3 board docs](boards/m5stack-cores3/README.md), [trial](../firmware/esp-idf-cores3/README.md) |
| Waveshare ESP32-S3-SIM7670G-4G V2 with PMS5003T | Native ESP-IDF logger and retained diagnostic; native hardware unverified, historical Arduino evidence in its bench record | [Waveshare board docs](boards/waveshare-esp32-s3-sim7670g/README.md), [trial](../firmware/esp-idf-waveshare-sim7670g/README.md) |

The five shared ESP-IDF components are AQCommon (encoding/drivers), AQRuntime (settings/logs),
AQConnectivity (local transports) AQLogger (sampling/archive worker) and AQLocation (official H3 geometry). A shared
source or host gate is not a hardware endurance result.

For any physical write, follow the board-specific safety sequence in [AGENTS.md](../AGENTS.md). Keep captures and firmware binaries in the relevant trial's ignored `artifacts/`, and record real-board measurements only in that board's bench record.
