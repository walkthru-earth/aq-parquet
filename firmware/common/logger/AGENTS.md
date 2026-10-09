# Shared opt-in logger runtime

AQLogger owns the monotonic sampling deadline, command queue and single archive
worker for consumers that opt into this native ESP-IDF component. A board supplies a measurement
contract, acquisition callback, mounted filesystem and optional bus/RTC hooks.
The native runtime preserves the established logger contract; board pin/peripheral
initialization and each board's schema/provenance remain in its own trial.

- Never include a board trial, M5Unified or a board filesystem/controller initializer here. Storage is already
  mounted and capacity callbacks borrow the board's filesystem instance.
- Preserve the `parquet` NVS namespace and `station` key. Radio initialization
  belongs to AQConnectivity; retain its shared identity and protocol contract.
- Only the worker accesses the filesystem after startup. Acquisition and RTC
  callbacks run only on the main task through begin/poll; a host clock command
  must enqueue an RTC request rather than performing hardware IO on the worker.
- Preserve 10-second monotonic sampling, absent-field/type-safe runtime setters,
  null UTC before a real anchor, visible drop/deadline/error counts, and immutable
  finalized files. Keep 90-row RAM batches, eight-row-group writer bounds and
  fsync after each row group; `.partial` files are preserved/quarantined rather
  than repaired or deleted. Do not imply power-cut durability from normal resets.
- Startup archive scans must not print each filename. Retain a bounded summary
  so offline startup does not depend on a USB reader draining archive output.
  Explicit serial `parquet list` still emits entries and its END marker.
- Board metadata must describe its hardware honestly. PMS5003T has four particle
  bins and ambient temperature/humidity; do not synthesize CoreS3-only bins.
- `parquet owner-pin` is physical serial-only and bypasses DebugLog. Never
  expose its response through BLE/LAN, CONFIG JSON, LOG_TAIL, boot output or
  retained host captures. `parquet wifi-profile` likewise writes only directly
  to the physical UART. `parquet config-hex` must reuse common SET_CONFIG
  validation/actions and emit only secret-free acknowledgements/allowlisted
  error keys; never log raw command bodies or decoded credentials.
- The compile fixture is generic ESP32-S3 with no M5 libraries. Compilation is
  not a board measurement. Host/board contract tests and real readback belong to
  the consuming trial's gates and board bench record.
