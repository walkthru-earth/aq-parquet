# CoreS3 Arduino trial

The active M5Stack CoreS3 logger adapter is `bringup/`. Its build and pinned
dependencies are local to this trial; `README.md` has commands and historical
revisions. Board wiring, power, RTC, SPI/SD arbitration and verified images are
in `../../docs/boards/m5stack-cores3/` (start at its `README.md`).

- CoreS3 has 16 MB flash and **Quad** PSRAM. Do not copy Waveshare GPIO,
  PSRAM or SDMMC setup. Use `M5.Power` rather than raw expander writes.
- This adapter owns M5Unified peripherals, sensor acquisition, display and
  board callbacks. Shared `AQLogger`, `AQConnectivity`, `AQRuntime` and
  `AQCommon` own their services; never add a second owner for a bus/radio.
- M134/PMSA003 is optional. Preserve missing sensor values as null, and keep
  RTC writes on the main task rather than the storage worker.
- Keep test captures and image hashes in ignored `artifacts/`, outside `build/`.
  Real board observations go in `../../docs/boards/m5stack-cores3/bench-verified.md`.
- Before flash, follow the root `AGENTS.md` port, eFuse, size and full-backup
  sequence. `pixi run chip` may reset the running app; use `parquet status`
  for live status. Do not pass high baud to CoreS3 flash readback.
