# CoreS3 native ESP-IDF application

The active M5Stack CoreS3 logger adapter is `bringup/`. Its build and pinned
dependencies are pinned by the shared ESP-IDF lock and this trial’s manifests; `README.md` has commands and historical
revisions. Board wiring, power, RTC, SPI/SD arbitration and verified images are
in `../../docs/boards/m5stack-cores3/` (start at its `README.md`).

- Use `pixi run idf-setup`, `pixi run cores3-build` and, after the safety
  sequence, `pixi run cores3-flash <checked-port> <full-backup>`. M5Unified and
  M5GFX are native IDF components; never add Arduino APIs or another SPI owner.
- Native migration has no real-board evidence yet; retained Arduino image
  measurements remain historical. Qualify USB console, LCD/SD arbitration,
  pairing and immutable readback before claiming native hardware behavior.
- CoreS3 has 16 MB flash and **Quad** PSRAM. Do not copy Waveshare GPIO,
  PSRAM or SDMMC setup. Use `M5.Power` rather than raw expander writes.
- This adapter owns M5Unified peripherals, sensor acquisition, display and
  board callbacks. Shared `AQLogger`, `AQConnectivity`, `AQRuntime` and
  `AQCommon` own their services; never add a second owner for a bus/radio.
- M134/PMSA003 is optional. Preserve missing sensor values as null, and keep
  RTC writes on the main task rather than the storage worker.
- Current firmware is `idf-cores3-parquet-v6.9`, schema v4/dictionary v3 with
  81 fields. Preserve the original 77-field prefix. H3 and NTP policy are
  shared services: capture coarsened station location with each sample, keep
  country owner-declared, and never initialize GPS or infer country here.
- Keep test captures and image hashes in ignored `artifacts/`, outside `build/`.
  Real board observations go in `../../docs/boards/m5stack-cores3/bench-verified.md`.
- Before flash, follow the root `AGENTS.md` port, eFuse, size and full-backup
  sequence. `pixi run chip` may reset the running app; use `parquet status`
  for live status. Do not pass high baud to CoreS3 flash readback.
