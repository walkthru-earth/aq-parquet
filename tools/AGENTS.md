# Host tools

Run these scripts through pinned `pixi` tasks; `pixi.toml` names the task and
`docs/shared/development.md` explains setup. `tools/host_fakes/` enables pure
C++ contract tests and cannot prove behavior on a physical board.

| Need | Tool / gate |
| --- | --- |
| Parquet footer/writer and reader compatibility | `parquet-test --sanitize`, `inspect_parquet.py` |
| Telemetry dictionary, units and validity | `telemetry-contract-test --sanitize` |
| BLE/LAN archive and framing | `archive-sync-test`, `test_ble_sync.py` |
| Router/local/public NTP order and H3 privacy geometry | `network-time-test`, `location-test`, `config-test`, `control-sync-test` |
| Checked serial readback, fetch and bounded capture | `parquet_device.py`, `capture_serial.py` |
| Full-flash backup publication (offline fake esptool) | `backup-test` |
| Native SDK and affected board builds | `idf-setup`, `cores3-build`, `waveshare-build [logger|diagnostic]` |
| Formatting and static diagnostics | `fmt-check`, `lint` |

- Never write eFuses or format SD; flash writes require the root `AGENTS.md`
  safety sequence and an explicit checked serial port. Only one process owns
  a serial port at a time.
- `owner-pin` and provisioning output are secrets. Do not retain them in logs,
  artifacts, fixtures, tests or documentation. Keep LAN tokens in ignored
  artifacts and never print them.
- Keep host fixtures deterministic. Record actual hardware evidence only in
  the relevant board's `bench-verified.md` with date, image hash and limits.

- Firmware builds use the pinned native ESP-IDF wrapper and exact managed
  component pins. Host tests remain in Pixi. Never introduce an Arduino build
  route or infer physical native behavior from fixture compilation.
