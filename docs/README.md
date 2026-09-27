# Documentation router

The device protocol, telemetry format, and offline-first data flow are shared across board trials. Wiring, power, peripheral ownership, build settings, and measured performance belong to the individual board. Source-checked claims and real-device results remain separate.

## Shared contracts

| Topic | Reference |
| --- | --- |
| Host tooling, shared code and adding another board | [Development workflow](shared/development.md) |
| Sampling, nulls, Parquet, Hive partitions, and offline archives | [Telemetry pipeline](shared/telemetry-pipeline.md) |
| Dictionary, observation semantics, and proposed Iceberg workflow | [Table and observation model](shared/table-and-observation-model.md) |
| BLE/LAN sync frames, pairing, and transport | [Sync protocol](shared/ble-sync-protocol.md) |
| Phone background sync triggers | [Background sync triggers](shared/background-sync-triggers.md) |
| Security and production-hardening boundary | [Security hardening](shared/security-hardening.md) |

## Boards

| Board | Status | Reference |
| --- | --- | --- |
| M5Stack CoreS3 with optional M134/PMSA003 | Existing Arduino trial; real board and SD evidence through 2026-09-18 | [CoreS3 board docs](boards/m5stack-cores3/README.md), [trial](../firmware/arduino-m5unified/README.md) |
| Waveshare ESP32-S3-SIM7670G-4G V2 with PMS5003T | Identification, backup, diagnostic flash and boot verified 2026-09-28; no TF card inserted, no valid PMS frames observed yet | [Waveshare board docs](boards/waveshare-esp32-s3-sim7670g/README.md), [trial](../firmware/arduino-waveshare-sim7670g/README.md) |

For any physical write, follow the board-specific safety sequence in [AGENTS.md](../AGENTS.md). Keep captures and firmware binaries in the relevant trial's ignored `artifacts/`, and record real-board measurements only in that board's bench record.
