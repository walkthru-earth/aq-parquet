# Waveshare V2 native ESP-IDF adapter

One active firmware variant at a time: `logger/` is the full application;
`diagnostic/` is the retained UART/TF bring-up fixture. Both use this trial's
pinned ESP-IDF toolchain, component manifests and partition files. Never borrow a sibling trial's settings.

- Use `pixi run idf-setup`, `pixi run waveshare-build [logger|diagnostic]`
  and the backup-gated `waveshare-flash` task. No Arduino APIs or components.
  Native migration has no real-board evidence yet; earlier Arduino image
  results remain historical and require native readback to extend.
- Exact board: ESP32-S3-SIM7670G-4G **V2.0**, ESP32-S3R8 rev0.2, 16 MB Quad
  flash and 8 MB Octal PSRAM. OPI was qualified by an allocated-region memory
  test. Preserve UART0 console through CH343; USB CDC is not the console.
- The board adapter owns UART1 PMS5003T RX1/TX2, 9600 8N1; four particle bins
  plus temperature/RH. GPIO2 is valid on V2, conflicting with the V1 gauge.
- Mount TF with SDMMC CLK5/CMD4/D0=6 in one-bit mode without formatting. After
  startup, AQLogger's single worker owns every filesystem operation.
- AQLogger, AQConnectivity, AQRuntime and AQCommon own shared application
  services. Board code supplies only descriptors, sensor collection, card
  capacity hooks and board GPIO setup. No second radio initializer.
- Reserve the heap-owned 4 KiB internal DMA SD transfer buffer before radios
  and retain it while mounted. Shared connectivity prefers PSRAM for packet/TCP
  memory and bounds static Wi-Fi DMA buffers; the diagnostic excludes radios
  and PSRAM. Source/host validation does not qualify this board's native IO.
- GPIO38 RGB is a board-local startup/card/sensor indicator. Camera, modem,
  GNSS and charger control are disabled. The board adapter alone owns MAX17048
  I²C on SDA15/SCL16; never initialize that controller from another driver.
  Do not guess modem
  power pins or change USB routing/DIP switches for the logger.
- The owner subsequently installed an 18650 and verified battery-only boot.
  `kBatteryInstalled` is the explicit board configuration; a MAX17048 response
  still cannot prove battery presence. A failed register nulls only its own
  columns. A good VCELL with a bad SOC still writes `battery_mv` and drives
  `battery_health_state`, which is always written and reads 0 when unknown.
  Charger state has no verified ESP32 signal. No external RTC: UTC remains null until a real host or network anchor,
  and a reboot starts a new unanchored clock epoch.
- Current firmware is `idf-waveshare-parquet-v2.7`, schema v5/dictionary v5
  with 62 fields. Fields 57 to 62 hold MAX17048 diagnostics and the shared
  `power/` battery health state. Battery thresholds live in
  `telemetry_contract.h` (`kBatteryThresholds`) and are uncalibrated until a
  dated discharge run. VCELL senses the VBAT rail, so external power is only
  inferred. The gauge ALRT pin is tied to ground, so never rely on alerts. Preserve the original 52-field order. Field ids are the
  1-based `telemetry_fields.inc` position, so only append fields. Each `FIELD`
  row ends with its LIVE key ("" stays off LIVE) and its LIVE float decimals, so
  the dictionary digest covers both and any change needs a matching mobile app release. Shared H3 helpers
  publish only the owner-configured coarsened station cell and its center;
  country is optional and owner-declared. Do not turn on modem GNSS for this.
- Fixed pairing PIN is random per device. Retrieve it only through the
  explicit physical-serial owner command; never put PIN/PSK/token values in
  logs, artifacts or docs. Preserve network-side encryption/authentication.
- Contract changes require `waveshare-contract-test --sanitize`; writer and
  runtime changes also require shared gates, both board builds and real
  readback proportional to the change. Retain images/logs outside `build/`
  in ignored `artifacts/`, and record hashes and limitations in board docs.
