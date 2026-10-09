# Agent map — aq-parquet

ESP32-S3 air-quality firmware. CoreS3 and Waveshare V2 each have one active,
self-contained native ESP-IDF application; do not add Arduino APIs or an
Arduino component. Shared code lives in `firmware/common/`; device
protocol and telemetry contracts live in `docs/shared/`. Open only the relevant
file in `docs/README.md` after this one.

| Task | Open next |
| --- | --- |
| CoreS3 pins, build, current bench evidence | `firmware/esp-idf-cores3/AGENTS.md`, `docs/boards/m5stack-cores3/README.md` |
| Waveshare V2 pins, build, current bench evidence | `firmware/esp-idf-waveshare-sim7670g/AGENTS.md`, `docs/boards/waveshare-esp32-s3-sim7670g/README.md` |
| Shared drivers, writer, settings, radio, logger | `firmware/common/AGENTS.md`, then its scoped `AGENTS.md` |
| BLE/LAN and Android compatibility | `docs/shared/ble-sync-protocol.md`, `../opensensor-space-android/AGENTS.md` |
| Telemetry schema, Parquet, UTC, archive paths | `docs/shared/telemetry-pipeline.md`, `docs/shared/table-and-observation-model.md` |
| Host scripts and gates | `tools/AGENTS.md`, `docs/shared/development.md` |

## Code boundaries

- Board trials own pins, peripheral initialization, measurement schema and
  acquisition. Do not borrow sibling trial settings or start competing trials
  without a measured reason. Shared ESP-IDF components remain board-neutral and opt-in.
- One owner per peripheral and controller. Do not guess GPIO from another
  board. CoreS3 uses Quad PSRAM; Waveshare V2 uses OPI PSRAM. Inspect each
  board's wiring and bench record before changing pins, power or memory.
- Preserve 10-second monotonic sampling, absent/null measurements and UTC until
  a real time anchor exists. The storage worker alone accesses files after
  startup. Finalized Parquet is immutable; retain `.partial` files. Do not
  silently repair, delete or rewrite owner data.
- The card is the origin; local phone or laptop archive is the first copy.
  BLE/LAN sync works without internet. Cloud upload is optional future work.
- Keep device PIN, Wi-Fi PSK and LAN bearer token out of logs, radio responses,
  captured artifacts and docs. Contract changes require firmware and Android
  consumers to agree; `docs/shared/ble-sync-protocol.md` owns wire behavior.
- ESP-IDF source/toolchain is pinned by `tools/idf-dependencies.lock`; managed
  components use exact manifest pins. Use `pixi run idf-setup`, `cores3-build`
  and `waveshare-build [logger|diagnostic]`. Never erase NVS during migration;
  retain station/config namespaces, keys, types and flash partition offsets.
- Bench claims require dated board evidence with image identity and limits.
  Host tests do not prove SD, radio coexistence or power-cut durability.

## Before any physical flash write

Identify the exact board and checked serial port. On first connection, run in
order: `pixi run ports`; `pixi run chip --port <port>`; `pixi run flash-id --port
<port>`; `pixi run efuse --port <port>`; `pixi run backup --board <board> --port
<port>`; confirm the backup is **16,777,216 bytes** in `backup/`. The first four
commands read flash/device state; `chip` can reset a running app on exit. Never
write eFuses or format the card. Never assume flash size from a product name.
CoreS3 native USB readback must use esptool's default baud: 921600 corrupted a
measured read. See the selected board's docs for recovery and power rules.

## Dependency selection

Before choosing or changing SDK/component versions, check the latest stable release and current official documentation. Pin exact verified versions and source identity; record compatibility exceptions with evidence. Shared components must remain reusable across board applications; each board owns pins, controller initialization, mount policy and hardware capabilities.

## Host gate

Use the pinned pixi environment (`pixi install`, `pixi run <task>`), not ad hoc
pip packages. Run `pixi run fmt-check` and `pixi run lint`; for writer/schema or
archive changes run the relevant `pixi run parquet-test --sanitize`,
`telemetry-contract-test --sanitize`, `archive-sync-test` and board contract
gates. Build each affected board, then perform proportional real-device readback
before claiming hardware behavior. Keep captures/exports/binaries in ignored
trial `artifacts/`, never `build/`; full-card owner copies live in ignored
`exports/`. `docs/shared/development.md` has setup and exact task names.
