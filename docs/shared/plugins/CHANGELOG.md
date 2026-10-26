# Plugin contract changelog

Newest first. Versions follow section 7 of the [README](README.md).

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
