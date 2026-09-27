# Shared firmware code

This Arduino library holds code reusable across ESP32-S3 board trials. `src/` has no board pin numbers, M5Unified calls, SD mount policy, RTC, power, display, modem, or radio initialization. The board trial owns those services and its measurement schema/version.

- Keep the Plantower byte framing and checksum here. Choose the sensor model explicitly at construction: PMSA003 and PMS5003T have the same 32-byte envelope, but different meanings at bytes 24–27. Never publish the PMS5003T's temperature/RH words as >5/>10 µm particle counts.
- Keep the bounded Parquet writer and LZ4 adapter here. Preserve `vendor/lz4` bytes and license; the adapter is tested with sanitizers and two readers via `pixi run parquet-test --sanitize`.
- A trial consumes this library through Arduino CLI `--libraries firmware/common`. Never include source from a sibling trial. New board trials pass their own `created_by` to the writer; the default is the historical CoreS3 identity. Schema identifiers and provenance remain trial-owned; do not change existing file identities as a side effect of a repository rename.
- Board-specific validation belongs in that trial's README and its board's docs. A host fixture is not a real SD or power-loss test.
