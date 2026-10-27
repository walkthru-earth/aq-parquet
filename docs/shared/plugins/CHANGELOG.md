# Plugin contract changelog

Newest first. Versions follow section 7 of the [README](README.md).

## 0.1.3, 2026-10-27

- Step 5 of section 11. The Waveshare logger collect runs a `static
  constexpr` module table of `pms5003t`, the waveshare identity module,
  `max17048` and `battery_health`, in
  `firmware/esp-idf-waveshare-sim7670g/logger/sensor_modules.h`. The logger
  and `Hooks::collect` keep their signatures.
- `os_sensor_module.h` adds `ossensor::Writer`, tied to one module's
  `Column` enum, with typed writes checked against the board field type at
  build time, runtime writes that skip
  `kNoSlot`, and a `[[nodiscard]]` `init_module`. Section 10 documents them.
- The old `apply_pms`, `apply_gauge` and `apply_battery_health` leave the
  Waveshare contract header. They live on as the test reference in
  `tools/waveshare_collect_reference.h`, and a row equality fixture in
  `waveshare-contract-test` compares both.
- No schema change. No `dict`, `desc` or row moves.

## 0.1.2, 2026-10-27

- Step 4 of section 11. The gate generates one C++ header per module
  manifest, `<id>_module.h` beside its `module.json`, for the six shared
  modules and the five board local modules. `system` has none, because the
  logger fills its columns by name.
- Each header has `kProcedure`, a `Column` enum with the column names
  verbatim, `kColumns` in manifest order, a `Segment` enum and
  `kColumnSegments` with the segment of each column, `kStatusColumn` and the
  `kStatus` constants when the module names a status column, and an error
  only `InitError` when the manifest lists `init_errors`.
- The headers are checked in. `pixi run plugin-contract` fails on a
  missing, stale or orphan header, and `pixi run plugin-headers` rewrites
  them.
- `firmware/common/logger/src/os_sensor_module.h` adds `ossensor::Module`,
  `ossensor::init_error` and the `consteval` `ossensor::resolve`. A second
  `resolve` form takes the selected segments and maps columns of other
  segments to `ossensor::kNoSlot`, so a board can ship only `max17048.core`.
  No board includes it yet.
- `pixi run sensor-module-test` compiles the headers against both boards'
  field tables and proves a missing name stops the build.
- No schema change, so the schemas stay under `v0.1.0/`. No board file,
  `dict` or `desc` moves.

## 0.1.1, 2026-10-27

- Optional release `baseline` in the lock schema, with `reason`,
  `migration` and `segments`. A baseline is per segment. Such a release may
  edit only the shipped segments it lists by module and segment id, it must
  edit each one it lists, and every other shipped segment keeps the old
  rule. Its `migration` names a mobile migration notes heading that starts a
  line, and that entry names the release dict. A patch per section 7, so the
  schema stays under `v0.1.0/`.
- The Waveshare lock gains its first baseline release for the gauge
  validity token rename (#28).

## 0.1.0, 2026-10-27

- Module, board and lock schemas under `v0.1.0/`, with refs into the vendored
  stac-opensensor v0.1.0 schemas.
- `status` names the status column only. Its codes are that column's
  `classification:classes`.
- Board `class_subsets` lists the classes a board ships for a column.
- Examples `examples/bme280.module.json` and `examples/weather.board.json`.
- Manifests for the logger segments, six shared sensor modules, five board
  local modules and both boards. They reproduce today's CoreS3 and Waveshare
  board files byte for byte, so `dict` and `desc` do not move.
- `shipped.lock` per board at today's `dict`. A lock absent at the base
  branch prints a notice instead of passing silently.
- `tools/plugin_contract.py` and `pixi run plugin-contract-test` in
  `pixi run gate`.
