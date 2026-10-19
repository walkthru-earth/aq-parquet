# Host tools

Run these scripts through pinned `pixi` tasks; `pixi.toml` names the task and
`docs/shared/development.md` explains setup. `tools/host_fakes/` enables pure
C++ contract tests and cannot prove behavior on a physical board.

| Need | Tool / gate |
| --- | --- |
| Parquet footer/writer and reader compatibility | `parquet-test --sanitize`, `inspect_parquet.py` |
| Footer reader and Iceberg DataFile metrics | `reader-test` (ASan + UBSan, crafted footers, fuzz loop) |
| File size, footer share and host encoding or compaction variants | `size-survey`, `size-survey-test` |
| Telemetry dictionary, units and validity | `telemetry-contract-test --sanitize` |
| BLE/LAN archive and framing | `archive-sync-test`, `test_ble_sync.py` |
| Router/local/public NTP order and H3 privacy geometry | `network-time-test`, `location-test`, `config-test`, `control-sync-test` |
| Checked serial readback, fetch and bounded capture | `parquet_device.py`, `capture_serial.py` |
| Footer rebuild for a quarantined `.partial` from its page headers and the board field table, output in a separate directory and checked by PyArrow and DuckDB | `rebuild-partial --board waveshare --out <dir> <files>`, `rebuild-partial-test --sanitize` |
| Power cut cycles at the write, footer and rename stages, per boot loss account, baseline SHA-256 manifest and compare (owner bench only, tests use a fake port) | `bench-power-cut run|analyze|manifest|compare`, `bench-power-cut-test` |
| Discharge run curve of `battery_mv`, SOC, CRATE and `battery_health_state` against UTC as SVG, with power-critical closes and small files per boot from serial or LOG_TAIL logs (owner bench, #61 item 1, tests use a generated run) | `analyze-discharge <files> --log <log> --out <dir>`, `analyze-discharge-test` |
| Full-flash backup publication (offline fake esptool) | `backup-test` |
| Native FAT mount root and archive directory creation | `logger-directory-test` |
| Per-file C++ dialect of an SDK fixture build, and every fixture entry point calling it | `check_idf_target.py --compile-dialect <build>`, `compile-dialect-test` |
| Native SDK and affected board builds | `idf-setup`, `cores3-build`, `waveshare-build [logger|diagnostic]` |
| Shared sensor drivers on fake buses | `pms-frame-test`, `ltr553-test`, `max17048-test` |
| Battery health states, thresholds, debounce, settling and the boot Low or Critical tie | `battery-health-test` |
| COREDUMP line once per saved core dump, NVS record and its read or write failures | `coredump-report-test` |
| Board-declared LIVE keys and JSON formatting | `logger-live-test` |
| POWER STATE lines, one finalize per discharge, the report wake, emergency close outcomes, retry and pass lines | `logger-power-test` |
| Battery close stopping LIST, READ and slow peer waits, the Retry remount recheck | `archive-sync-test`, `wifi-link-test` |
| LIVE key set frozen per dictionary digest (`fixtures/live_keys/`) | `telemetry-contract-test`, `waveshare-contract-test` |
| Formatting and static diagnostics | `fmt-check`, `lint` |
| Full host gate, same entry point as CI | `gate` (`tools/gate.sh`, add new host test tasks to its list) |
| CI image size table from `idf.py size --format json2`, absolute sizes with a warning when the baseline is malformed | `size_report.py`, `size-report-test` |
| App image share of the smallest app slot in the board `partitions.csv`, fail closed over the budget in the tool | `ota-slot-check --label --partitions --build`, `ota-slot-check-test` |
| Static task stack worst case, xTaskCreate name and stack size cross check, and linker section budgets per image (`stack_budget.json`) | `stack-budget [--check] [--path]`, `stack-budget-test` |

- Never write eFuses or format SD; flash writes require the root `AGENTS.md`
  safety sequence and an explicit checked serial port. Only one process owns
  a serial port at a time.
- `owner-pin` and provisioning output are secrets. Do not retain them in logs,
  artifacts, fixtures, tests or documentation. Keep LAN tokens in ignored
  artifacts and never print them.
- Keep host fixtures deterministic. Record actual hardware evidence only in
  the relevant board's `bench-verified.md` with date, image hash and limits.

- `.github/workflows/ci.yml` runs `pixi run gate` and the board image builds on
  every pull request. Keep the gate free of tasks that need hardware, Bluetooth,
  a LAN peer or the SDK. Each gate task is bounded by `AQ_GATE_TASK_TIMEOUT`
  (300 s), and the gate fails if a pixi `*-test` task is missing from its list.
  `CI result` is the only required check, so a new CI job must join its
  `needs` list, see `docs/shared/development.md#required-checks`.
- Firmware builds use the pinned native ESP-IDF wrapper and exact managed
  component pins. Host tests remain in Pixi. Never introduce an Arduino build
  route or infer physical native behavior from fixture compilation.
