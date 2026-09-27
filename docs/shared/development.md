# Shared development workflow

This repository currently targets ESP32-S3 boards with separate Arduino trials. Board pins, PSRAM mode, power sequencing, storage mounting and hardware evidence belong to the board/trial; shared framing and file-format code belongs to [`firmware/common`](../../firmware/common/AGENTS.md).

## Host and toolchain

Host dependencies are pinned in root `pixi.toml` and `pixi.lock`. Start with `pixi install`; run commands through `pixi run`. Add host packages with `pixi add`. A trial's `setup.sh` bootstraps its pinned firmware SDK/CLI, which are outside the Pixi environment.

| Board/trial | Setup | Build | Current hardware scope |
| --- | --- | --- | --- |
| [CoreS3 / Arduino-M5Unified](../../firmware/arduino-m5unified/README.md) | `pixi run arduino-setup` | `pixi run arduino-build` | Measured Parquet logger, display, SD and local sync |
| [Waveshare V2 / Arduino](../../firmware/arduino-waveshare-sim7670g/README.md) | `pixi run waveshare-setup` | `pixi run waveshare-build` | Booted PMS5003T/TF diagnostic; valid sensor frames and card operation still pending |

SDK downloads use `$AQ_TOOLCHAIN_ROOT`; the existing default is `~/.cache/m5stack-aq-parquet/toolchains` and the legacy `$M5_TOOLCHAIN_ROOT` is accepted. The old cache name is retained to reuse installed SDKs after the GitHub rename. Each trial owns its exact dependency lock and build options; a shared download cache does not make a sibling trial a dependency. Firmware wrappers isolate the SDK environment from Pixi's host tools.

If the checkout or managed environment has moved and a tool reports a missing interpreter at the old path, run `pixi install` to reconcile the environment. Reinstall the affected Pixi package if it still embeds that path. Use `python -m esptool` / `python -m espefuse` through project tasks rather than stale entry-point shebangs.

## Adding a board

1. Create `docs/boards/<board>/README.md`, hardware/pin ownership notes, sensor notes and a `bench-verified.md`. Record the exact PCB revision and upstream sources; keep measured and source-checked claims separate.
2. Create one justified, self-contained `firmware/<framework>-<variant>/` trial with README, pinned dependencies, setup/build/flash scripts, partition table and build settings. Do not include a sibling's sources. Add explicit root Pixi tasks and lint inputs.
3. Consume `firmware/common` as a library. Select the Plantower model explicitly. Keep board services and measurement definitions in the trial; pass a board-specific `created_by` when using the shared writer.
4. Audit GPIO/controller ownership, strapping pins, flash size, PSRAM mode, console transport and recovery before enabling peripherals. The common ESP32-S3 chip does not imply common board wiring.
5. Follow the [identification and full-backup sequence](../../AGENTS.md#do-not-brick-the-board) on the checked port. Extend the backup/restore scripts deliberately for another flash capacity; the current helpers accept only the two 16 MiB targets.
6. Verify a small diagnostic, then the sensor byte contract and storage on a checked card. Add a versioned measurement schema before logging; unavailable measurements stay null. Adopt BLE/LAN only when that trial implements the [shared protocol](ble-sync-protocol.md).
7. Retain captures and image/ELF files in the trial's ignored `artifacts/`, and record hashes, date, commands and limitations in that board's bench file. Add the board to the [docs router](../README.md) and root README.

## Verification

Run `pixi run fmt-check` and `pixi run lint` for project C/C++ changes. The formatter discovers project sources while excluding build/vendor trees. Preserve vendored source bytes and notices.

Shared Plantower changes require `pixi run pms-frame-test`. Writer/codec/footer changes require `pixi run parquet-test --sanitize`; measurement-contract changes require `pixi run telemetry-contract-test --sanitize`. Build every affected trial. Host tests establish parser/format behavior; board-specific storage, timing and power-loss claims require real hardware evidence.
