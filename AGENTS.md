# Agent map for aq-parquet

ESP32-S3 air-quality firmware. CoreS3 and Waveshare V2 each have one active,
self-contained native ESP-IDF application. Do not add Arduino APIs or an
Arduino component. Shared code lives in `firmware/common/`. Device
protocol and telemetry contracts live in `docs/shared/`. Open only the relevant
file in `docs/README.md` after this one.

| Task | Open next |
| --- | --- |
| CoreS3 pins, build, current bench evidence | `firmware/esp-idf-cores3/AGENTS.md`, `docs/boards/m5stack-cores3/README.md` |
| Waveshare V2 pins, build, current bench evidence | `firmware/esp-idf-waveshare-sim7670g/AGENTS.md`, `docs/boards/waveshare-esp32-s3-sim7670g/README.md` |
| Shared drivers, writer, settings, radio, logger | `firmware/common/AGENTS.md`, then its scoped `AGENTS.md` |
| New ESP32-S3 board, logger hooks and capabilities | `docs/shared/board-porting.md`, `firmware/common/logger/README.md` |
| BLE/LAN and mobile compatibility | `docs/shared/ble-sync-protocol.md`, `../opensensor-space-mobile/AGENTS.md` |
| Telemetry schema, Parquet, UTC, archive paths | `docs/shared/telemetry-pipeline.md`, `docs/shared/table-and-observation-model.md` |
| Router/local NTP, H3 and country/privacy configuration | `docs/shared/time-and-location.md`, `firmware/common/location/AGENTS.md` |
| Host scripts and gates | `tools/AGENTS.md`, `docs/shared/development.md` |

## Code boundaries

- Board trials own pins, peripheral initialization, measurement schema and
  acquisition. Do not borrow sibling trial settings or start competing trials
  without a measured reason. Shared ESP-IDF components remain board-neutral and opt-in.
  Add capabilities through `aqlogger::Hooks`, `aqsync::ServiceConfig` and board
  sdkconfig defaults, never board-name conditionals in `firmware/common`.
- One owner per peripheral and controller. Do not guess GPIO from another
  board. CoreS3 uses Quad PSRAM. Waveshare V2 uses OPI PSRAM. Inspect each
  board's wiring and bench record before changing pins, power or memory.
- Preserve 10-second monotonic sampling, absent/null measurements and null UTC until
  a real time anchor exists. The storage worker alone accesses files after
  startup. Finalized Parquet is immutable. Retain failed `.partial` files in
  provenance quarantine and leave crosslink suspects in place. The SD governor
  alarms and resumes after space or storage recovers, without automatic deletion. Do not
  silently repair, delete or rewrite owner data.
- The card is the origin. The local phone or laptop archive is the first copy.
  BLE/LAN sync works without internet. Cloud upload is optional future work.
- Keep device PIN, Wi-Fi PSK and LAN bearer token out of logs, radio responses,
  captured artifacts and docs. Contract changes require firmware and mobile
  consumers to agree. `docs/shared/ble-sync-protocol.md` owns wire behavior.
  Firmware reports proto 3 and mobile rejects earlier protocols. Preserve
  per-peer generations, ABORT, u32 LIST_END counts and typed disconnect evidence.
  Only the LAN task writes sockets and only the NimBLE host sends BLE responses.
- ESP-IDF source/toolchain is pinned by `tools/idf-dependencies.lock`. Managed
  components use exact manifest pins. Use `pixi run idf-setup`, `cores3-build`
  and `waveshare-build [logger|diagnostic]`. Never erase NVS during migration.
  Retain station/config namespaces, keys, types and flash partition offsets.
- Prefer DHCP/router and configured local NTP before permitted public fallback.
  Never infer internet availability or UTC from Wi-Fi association. Preserve RTC
  and authenticated host anchors offline. Location starts unset. Persist only
  the privacy-coarsened H3 cell and owner-declared country. Snapshot location per
  sample/file and split on changes. Never relabel finalized files.
- Bench claims require dated board evidence with image identity and limits.
  Host tests do not prove SD, radio coexistence or power-cut durability.

## Before any physical flash write

Identify the exact board and checked serial port. Before each physical flash
write, run these commands in order.

1. `pixi run ports`
2. `pixi run chip --port <port>`
3. `pixi run flash-id --port <port>`
4. `pixi run efuse --port <port>`
5. `pixi run backup --board <board> --port <port>`

Confirm the backup is **16,777,216 bytes** in `backup/`. The first four
commands read flash/device state. `chip` can reset a running app on exit. Never
write eFuses or format the card. Never assume flash size from a product name.
CoreS3 native USB readback must use esptool's default baud. 921600 corrupted a
measured read. See the selected board's docs for recovery and power rules.

## Dependency selection

Before choosing or changing SDK/component versions, check the latest stable release and current official documentation. Pin exact verified versions and source identity. Record compatibility exceptions with evidence. Shared components must remain reusable across board applications. Each board owns pins, controller initialization, mount policy and hardware capabilities.

## Host gate

Use the pinned pixi environment (`pixi install`, `pixi run <task>`), not ad hoc
pip packages. Run `pixi run fmt-check` and `pixi run lint`. For writer/schema or
archive changes run the relevant `pixi run parquet-test --sanitize`,
`telemetry-contract-test --sanitize`, `archive-sync-test` and board contract
gates. Every firmware PR builds both boards. Perform proportional real-device readback
before claiming hardware behavior. Keep captures/exports/binaries in ignored
trial `artifacts/`, never `build/`. Full-card owner copies live in ignored
`exports/`. `docs/shared/development.md` has setup and exact task names.
