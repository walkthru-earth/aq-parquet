# Shared power policy

- `aq_battery_health` is pure C++ with no IO, clocks, heap or board names.
  The board reads the gauge, builds one `opensensor::battery::Sample` per tick with
  monotonic time and owns every hardware decision.
- `State` codes are stored in telemetry. Never renumber them. Add new
  states at the end and update every board metadata table and the mobile
  migration note in the same change.
- `Thresholds` is the calibration entry point. The defaults are an
  uncalibrated starting point. Change them only with a dated discharge run
  recorded in the board's `bench-verified.md`, and bump the board
  configuration identity that embeds them.
- The classifier never resets, sleeps or sheds loads. Actions belong to the
  logger power hooks and the board.
- Host gate is `pixi run battery-health-test` with ASan and UBSan.
