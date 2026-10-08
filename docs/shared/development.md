# Shared development workflow

This repository currently targets ESP32-S3 boards with separate Arduino trials. Board pins, PSRAM mode, power sequencing, storage mounting and hardware evidence belong to the board/trial; the four shared libraries (AQCommon, AQRuntime, AQConnectivity and AQLogger) belong to [`firmware/common`](../../firmware/common/README.md).

## Host and toolchain

Host dependencies are pinned in root `pixi.toml` and `pixi.lock`. Start with `pixi install`; run commands through `pixi run`. Add host packages with `pixi add`. A trial's `setup.sh` bootstraps its pinned firmware SDK/CLI, which are outside the Pixi environment.

| Board/trial | Setup | Build | Current hardware scope |
| --- | --- | --- | --- |
| [CoreS3 / Arduino-M5Unified](../../firmware/arduino-m5unified/README.md) | `pixi run arduino-setup` | `pixi run arduino-build` | Measured Parquet logger, display, SD and local sync |
| [Waveshare V2 / Arduino](../../firmware/arduino-waveshare-sim7670g/README.md) | `pixi run waveshare-setup` | `pixi run waveshare-build` | Shared logger adapter implemented; retained TF/PMS diagnostic and per-image hardware evidence in the board bench record |

SDK downloads use `$AQ_TOOLCHAIN_ROOT`; the existing default is `~/.cache/m5stack-aq-parquet/toolchains` and the legacy `$M5_TOOLCHAIN_ROOT` is accepted. The old cache name is retained to reuse installed SDKs after the GitHub rename. Each trial owns its exact dependency lock and build options; a shared download cache does not make a sibling trial a dependency. Firmware wrappers isolate the SDK environment from Pixi's host tools.

If the checkout or managed environment has moved and a tool reports a missing interpreter at the old path, run `pixi install` to reconcile the environment. Reinstall the affected Pixi package if it still embeds that path. Use `python -m esptool` / `python -m espefuse` through project tasks rather than stale entry-point shebangs.

## Adding a board

1. Create `docs/boards/<board>/README.md`, hardware/pin ownership notes, sensor notes and a `bench-verified.md`. Record the exact PCB revision and upstream sources; keep measured and source-checked claims separate.
2. Create one justified, self-contained `firmware/<framework>-<variant>/` trial with README, pinned dependencies, setup/build/flash scripts, partition table and build settings. Do not include a sibling's sources. Add explicit root Pixi tasks and lint inputs.
3. Consume AQCommon via `firmware/common`; opt into AQRuntime (`firmware/common/runtime`) for settings/logging, AQConnectivity (`firmware/common/connectivity`) for BLE/Wi-Fi/control/file sync, and AQLogger (`firmware/common/logger`) for the sampling deadline, single worker and archive lifecycle. Select the Plantower model explicitly. Keep board services and measurement definitions in the trial; pass a board-specific `created_by` when using the shared writer.
4. Audit GPIO/controller ownership, strapping pins, flash size, PSRAM mode, console transport and recovery before enabling peripherals. The common ESP32-S3 chip does not imply common board wiring.
5. Follow the [identification and full-backup sequence](../../AGENTS.md#do-not-brick-the-board) on the checked port. Extend the backup/restore scripts deliberately for another flash capacity; the current helpers accept only the two 16 MiB targets.
6. Verify a small diagnostic, then the sensor byte contract and storage on a checked card. Add a versioned measurement schema before logging; unavailable measurements stay null. Use AQLogger with static schema/provenance descriptors, a board acquisition callback and already mounted storage capacity callbacks. Supply paired bus callbacks when borrowing a board mutex, and optional RTC callbacks on the main-loop task. AQLogger owns the nonblocking request queue, shared ArchiveSession, clock/flush/reboot/status and storage lifecycle; the board owns peripheral setup and actual measurement/RTC IO. Sharing the implementation does not prove it has run on another board.
7. Retain captures and image/ELF files in the trial's ignored `artifacts/`, and record hashes, date, commands and limitations in that board's bench file. Add the board to the [docs router](../README.md) and root README.

## Verification

Run `pixi run fmt-check` and `pixi run lint` for project C/C++ changes. The formatter discovers project sources while excluding build/vendor trees. Preserve vendored source bytes and notices.

Shared Plantower changes require `pixi run pms-frame-test`. Writer/codec/footer changes require `pixi run parquet-test --sanitize`; measurement-contract changes require `pixi run telemetry-contract-test --sanitize`. Build every affected trial. Host tests establish parser/format behavior; board-specific storage, timing and power-loss claims require real hardware evidence.

The shared module gates are `pixi run common-test`, `ltr553-test`, `config-test`, `control-sync-test`, `archive-sync-test`, `wifi-link-test`, `connectivity-build-test`, `logger-build-test`, `logger-status-test` and `logger-provision-test`. The Waveshare dictionary also requires `pixi run waveshare-contract-test --sanitize`. See the [module map](../../firmware/common/README.md) for ownership and callbacks. The generic ESP32-S3 compile fixture never gets flashed.

## Local provisioning and client boundaries

The shared [sync protocol](ble-sync-protocol.md) retains its UUIDs/opcodes and
512-byte BLE frame cap. STATUS/LIVE JSON allows 480 bytes. Clients request MTU
517 where possible; MTU 483 carries a maximum snapshot in one notification.
Notifications exceeding negotiated MTU − 3 are omitted after the complete value
is cached; use characteristic long reads for complete snapshots.
Interpret LIVE `t`/`rh` through the board's advertised schema/dictionary. A board
without an RTC hook has no retained clock anchor; UTC stays null after boot until
a host SET_TIME and unsynchronized rows remain in the `unsynced` tree.

Physical UART owner commands (`owner-pin`, `wifi-profile`, `config-hex`) bypass
DebugLog for secret-bearing replies. Use `pixi run owner-pin` and
`pixi run provision-usb` with explicitly checked ports; host tools must keep
credential profiles in memory and exclude them from captures and artifacts.
They add no radio opcode or secret CONFIG JSON field. The real SET_CONFIG
validator/actions remain the single provisioning implementation.
