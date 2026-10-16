# CoreS3 native ESP-IDF application

The active M5Stack CoreS3 logger adapter is `bringup/`. Its build and pinned
dependencies are pinned by the shared ESP-IDF lock and this trial’s manifests; `README.md` has commands and historical
revisions. Board wiring, power, RTC, SPI/SD arbitration and verified images are
in `../../docs/boards/m5stack-cores3/` (start at its `README.md`).

- Use `pixi run idf-setup`, `pixi run cores3-build` and, after the safety
  sequence, `pixi run cores3-flash <checked-port> <full-backup>`. M5Unified and
  M5GFX are native IDF components; never add Arduino APIs or another SPI owner.
- Native v6.9 has a dated short USB Parquet readback in the board bench record:
  SD at 20 MHz, network UTC, retained identity/settings and zero health errors.
  Retained Arduino measurements remain historical. Phone reconnect/transfers,
  display stress, sustained arbitration and power-cut durability remain unqualified.
- CoreS3 has 16 MB flash and **Quad** PSRAM. Do not copy Waveshare GPIO,
  PSRAM or SDMMC setup. Use `M5.Power` rather than raw expander writes.
- This adapter owns M5Unified peripherals, sensor acquisition, display and
  board callbacks. Shared `AQLogger`, `AQConnectivity`, `AQRuntime` and
  `AQCommon` own their services; never add a second owner for a bus/radio.
- M134/PMSA003 is optional. Preserve missing sensor values as null, and keep
  RTC writes on the main task rather than the storage worker.
- Current firmware is `idf-cores3-parquet-v6.12`, schema v5/dictionary v4 with
  81 fields. Preserve the original 77-field order. Field ids are the 1-based
  `telemetry_fields.inc` position, so only append fields. H3 and NTP policy are
  shared services: capture coarsened station location with each sample, keep
  country owner-declared, and never initialize GPS or infer country here.
- Reserve the heap-owned 4 KiB internal DMA SD transfer buffer before starting
  radios; keep it alive for the mount and reuse it for eight-sector chunks.
  Shared connectivity prefers PSRAM for packet/TCP memory and bounds static
  Wi-Fi DMA buffers. Preserve that policy and measure free/largest/minimum DMA
  heap under actual LAN load before claiming memory or throughput headroom.
- Keep test captures and image hashes in ignored `artifacts/`, outside `build/`.
  Real board observations go in `../../docs/boards/m5stack-cores3/bench-verified.md`.
- Before flash, follow the root `AGENTS.md` port, eFuse, size and full-backup
  sequence. `pixi run chip` may reset the running app; use `parquet status`
  for live status. Do not pass high baud to CoreS3 flash readback.
