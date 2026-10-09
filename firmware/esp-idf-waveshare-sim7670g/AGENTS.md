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
- GPIO38 RGB is a board-local startup/card/sensor indicator. Camera, modem,
  GNSS and charger control are disabled. The board adapter alone owns MAX17048
  I²C on SDA15/SCL16; never initialize that controller from another driver.
  Do not guess modem
  power pins or change USB routing/DIP switches for the logger.
- The owner subsequently installed an 18650 and verified battery-only boot.
  `kBatteryInstalled` is the explicit board configuration; a MAX17048 response
  still cannot prove battery presence. Read failures leave battery fields null.
  Charger state has no verified ESP32 signal. No external RTC: UTC remains null until a real host or network anchor,
  and a reboot starts a new unanchored clock epoch.
- Current firmware is `idf-waveshare-parquet-v2.3`, schema v3/dictionary v3
  with 56 fields. Preserve the original 52-field prefix. Shared H3 helpers
  publish only the owner-configured coarsened station cell and its center;
  country is optional and owner-declared. Do not turn on modem GNSS for this.
- Fixed pairing PIN is random per device. Retrieve it only through the
  explicit physical-serial owner command; never put PIN/PSK/token values in
  logs, artifacts or docs. Preserve network-side encryption/authentication.
- Contract changes require `waveshare-contract-test --sanitize`; writer and
  runtime changes also require shared gates, both board builds and real
  readback proportional to the change. Retain images/logs outside `build/`
  in ignored `artifacts/`, and record hashes and limitations in board docs.
