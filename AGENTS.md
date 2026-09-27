# AGENTS.md

Firmware project for **ESP32-S3 air-quality boards** in C and/or C++. Current boards are M5Stack CoreS3 and Waveshare ESP32-S3-SIM7670G-4G V2.
This repo can hold **more than one framework trial** against the same board and the same shared reference docs, with **one active trial per board at a time**. Sensor modules are optional add-ons, never assumed to be attached.

## Repo layout

```
firmware/
  <framework>-<variant>/   one self-contained trial, own build system and lockfile
  common/                  reusable ESP32-S3 Arduino code and its own AGENTS.md
docs/shared/               board-neutral data, protocol and security contracts
docs/boards/<board>/       board wiring, power, drivers and measured evidence
tools/                     host scripts: device readback, Parquet validation, logs
backup/                    flash images taken before writes, git-ignored
```

Trial directory names state the framework first, for example `firmware/idf-cpp/`, `firmware/arduino-m5unified/`, `firmware/esp-rs/`.

### Rules for trials

0. **One active trial per board at a time.** Start another trial for the same board only when a concrete result justifies it: a measured limitation, missing driver, or failed requirement. A different physical board with incompatible pin/peripheral ownership can have its own trial; record that motivation in its README. Do not open parallel framework trials speculatively.
1. A trial is **self-contained**. Its build files, `sdkconfig.defaults`, partition CSV, dependency lock, and toolchain identity live inside its own directory. Never reach into a sibling trial.
2. Shared knowledge lives in `docs/`, not duplicated per trial. If a trial discovers a board fact, promote it into the relevant `docs/` file rather than leaving it in one trial's README.
3. Every trial has a `README.md` recording the framework and pinned version, what the trial is meant to prove, current status (active, parked, or abandoned and why), the build and flash commands, and the date it was last verified on real hardware.
4. Trials may disagree. Do not refactor one trial to match another unless asked. An abandoned trial stays in the tree with its status recorded, because the reason it failed is the useful part.
5. Cross-trial comparison belongs in a top-level note, not inside a trial.

## Read on demand, not up front

`docs/README.md` is the router. Open only the shared contract and board-specific files a task actually needs. `firmware/common/AGENTS.md` governs shared code.

| Task touches | Open |
| --- | --- |
| Host tooling, reusable code, adding another board | `docs/shared/development.md`, `firmware/common/AGENTS.md` |
| CoreS3 wiring, peripherals, power, sleep, boot | `docs/boards/m5stack-cores3/cores3-hardware.md` |
| CoreS3 toolchain, memory, driver ownership | `docs/boards/m5stack-cores3/cores3-development.md` |
| CoreS3 Wi-Fi/BLE coexistence | `docs/boards/m5stack-cores3/cores3-wireless.md` |
| CoreS3 microSD and SPI | `docs/boards/m5stack-cores3/cores3-storage.md` |
| CoreS3 M134/PMSA003 | `docs/boards/m5stack-cores3/addons.md`, `addon-air-quality.md` |
| CoreS3 real-board evidence | `docs/boards/m5stack-cores3/bench-verified.md` |
| Waveshare V2 pins, rails, TF, boot | `docs/boards/waveshare-esp32-s3-sim7670g/hardware.md` |
| Waveshare PMS5003T wiring and byte contract | `docs/boards/waveshare-esp32-s3-sim7670g/pms5003t.md` |
| Phone BLE/LAN sync and background triggers | `docs/shared/ble-sync-protocol.md`, `background-sync-triggers.md` |
| Measurement schema, Parquet, Hive, observation model | `docs/shared/telemetry-pipeline.md`, `table-and-observation-model.md` |

## Host environment

Host tooling is managed by **pixi** (`pixi.toml`, `pixi.lock`). Run project commands as `pixi run <task>` or `pixi run <tool>`. Add host packages with `pixi add`, never by pip-installing into the managed environment.

Firmware SDKs are not conda packages, so the project bootstraps them itself rather than asking you to install anything by hand. Each trial ships a pinned `setup.sh` and is driven by a root pixi task, so a clean machine needs only `pixi install` followed by that trial's setup task.

SDKs are downloaded into `$AQ_TOOLCHAIN_ROOT`, which defaults to the existing `~/.cache/m5stack-aq-parquet/toolchains` cache (legacy `M5_TOOLCHAIN_ROOT` remains accepted). That path sits outside the repo on purpose, so several git worktrees share one copy and nothing large is ever written inside the tree. Build inside the SDK's exported environment so pixi does not shadow SDK-selected tools. See `docs/shared/development.md`.

## Do not brick the board

Identify the exact board and serial port before any flash write. The sequence below applies to either verified 16 MB ESP32-S3 board; board-specific PSRAM, power and recovery rules follow it. Never infer flash size from the product name alone. Run these in order the first time a physical board is connected, before any write.

| Step | Command | Why |
| --- | --- | --- |
| 1 | `pixi run ports` | Confirm which serial device is the board. Flashing the wrong port is the most common way to damage an unrelated device. |
| 2 | `pixi run chip --port <checked-port>` | Read-only identification. Refuses if the target is not an ESP32-S3. |
| 3 | `pixi run flash-id --port <checked-port>` | Confirm the actual flash size before assuming a partition layout. Both current targets are documented as 16 MB, but verify the unit. |
| 4 | `pixi run efuse --port <checked-port>` | Read-only. Confirms flash type and that no secure-boot or flash-encryption fuse has been burned, which is what makes the board recoverable. |
| 5 | `pixi run backup --board <board> --port <checked-port>` | Full verified 16 MB image into `backup/`, timestamped. Board is `m5stack-cores3` or `waveshare-sim7670g-v2`. Do this once per board before the first write, and keep it. |
| 6 | `ls -l backup/` | **Confirm the file exists and is 16777216 bytes.** A failed read can still exit zero behind a shell pipeline. Do not skip this. |

`pixi run restore --board <board> --port <checked-port> backup/<matching-file>.bin` writes a saved image back. It is destructive, so name the file explicitly. `pixi run backup-partitions backup/<file>.bin` shows what was on the board, without touching hardware.

Further rules.

- The Waveshare V2 board has embedded 8 MB PSRAM and a quad 16 MB flash, as read from the attached ESP32-S3 on 2026-09-28. Its exact PSRAM build mode remains unverified; the first diagnostic does not initialize PSRAM. Its GPIO2 is free of the V1 fuel-gauge conflict only on V2. See its board docs.
- CoreS3 is **Quad PSRAM, 16 MB flash**. Selecting Octal PSRAM because another S3 board uses it produces a board that does not boot.
- Never add pulls or drivers to ESP32-S3 strapping pins 0, 3, 45, 46 without a board-level audit. They may already carry board functions; Waveshare uses GPIO46 for TF card detect.
- Do not write eFuses. There is no undo. Nothing in this project needs them. Reading them with `pixi run efuse` is safe.
- **Never pass a high `--baud` to esptool on the CoreS3.** Measured, the default read 16 MB in 97 seconds, while `--baud 921600` corrupted the stream and aborted. The transport is native USB-Serial/JTAG, so a higher requested baud gains nothing.
- CoreS3 recovery from a bad image is hold RST about 3 seconds until the green LED, then release for download mode. Verify recovery works before flashing anything that reconfigures power or USB mode.
- `docs/boards/m5stack-cores3/cores3-hardware.md` covers rail assignments and expander bits. Raw expander writes can reset the screen or reverse the supply path, so use the `M5.Power` API.

## Reading the board back

For Bluetooth and LAN, `pixi run ble-sync …` (`tools/ble_sync.py`) is the host client for `docs/shared/ble-sync-protocol.md`; on macOS launch it from Terminal.app. Only `time`, `flush`, `set` and `reboot` write to the device. `--lan aq-xxxx.local --token-file artifacts/lan-token.txt` runs the same commands over TCP once `token --save` has fetched the token over BLE; the token is a bearer secret and stays in git-ignored `artifacts/`, never in docs or logs. If a worker-side command goes unanswered while sampling continues, look for `PARQUET ERROR operation=worker-stall` on serial and decode any task-watchdog backtrace with `xtensa-esp-elf-addr2line -pfiaC -e firmware/arduino-m5unified/build/bringup.ino.elf <addresses>` (see `docs/boards/m5stack-cores3/bench-verified.md`). `pixi run capture --port <port> --seconds <n>` records serial output for a bounded time and stops, so a silent board cannot hang the caller. Add `--out <file>` to keep the log, or `--until "<text>"` to stop early. `pixi run monitor <port> <baud>` is the interactive terminal.

On CoreS3, if a capture returns nothing, the board may be sitting in the ROM download bootloader, which prints nothing at all. Confirm with `pixi run chip`, which still answers there.

During an active Parquet run, prefer `pixi run parquet-device capture --port <port> --seconds <n>` and the helper's `command`, `fetch` and `bench` operations; `pixi run python tools/inspect_parquet.py <fetched-file>` prints row groups, footer facts and statistics and cross-checks both readers. Allow only one process to own the port. On the measured macOS/CoreS3 pair, deasserting both DTR and RTS reset the board; this helper keeps both asserted and clears POSIX HUPCL. A successful `chip` operation is read-only to flash but can reset the running application on exit: do not use it as a harmless live-status probe. Use `parquet status` instead. See the trial README for exact commands and the bench record for the host-specific evidence.

## Parquet logging guardrails

- Preserve the 10-second monotonic sampling deadline, sensor validity/nulls and visible drop/error counters. Display refreshes are separate snapshots, not exact-value references for stored rows.
- One storage worker owns filesystem access after startup; display and SD share an application mutex, with display DMA completed before unlock. Do not infer safe concurrent SPI access or measured speedup from the presence of two cores.
- Keep station identity in NVS and the UTC Hive path contract. Unsynchronized time stays null and uses the `unsynced` tree; do not fabricate dates or silently rewrite previous rows after a clock update.
- **Offline-first ordering.** The card is the origin, a local archive (phone, laptop, hub) is the first copy, the cloud is an optional later step that the owner turns on. Device↔phone links (BLE now; Wi-Fi or LoRa device-to-device later) are sync transports that must work with no internet; a device or phone that has Wi-Fi or a SIM may add a path, never replace the local copy. Do not design a feature that needs a server to show the owner their own data.
- Finalized Parquet files are immutable. Retain `.partial` files; no automatic formatting, recovery, retention deletion or upload is implemented. RAM-only batching can lose the unfinished batch on reset, and a normal-reset readback does not prove power-cut durability. Since firmware v6 a file may hold several row groups (one per completed ≤ 90-row batch, each `fsync`ed) and stays `.partial` until the window closes or an explicit flush; a reset in between leaves complete row groups without a footer — report it, never repair it silently.
- Statistics and annotations are part of the contract: `min_value`/`max_value` only with `column_orders`, floats under the parquet-format 2.13 `TYPE_ORDER` rules (NaN excluded and counted, `-0.0`/`+0.0` normalised), no deprecated `min`/`max`, `TIMESTAMP(NANOS, UTC)` only on `ns_since_unix_epoch` fields. Prove any footer change with `pixi run parquet-test --sanitize` (it decodes the footer field by field) before flashing.
- Keep each measurement tied to its image/schema. The earlier 72-column/60-row and later 73-column/90-row uncompressed runs are separate evidence. A host fixture is not a hardware endurance test; Snappy/Zstd and radio/upload still need board measurements. LZ4 details and measured scope live in `docs/boards/m5stack-cores3/compression-benchmark.md`.

## Code quality gates

`pixi run fmt`, `pixi run fmt-check`, `pixi run lint` (cppcheck over `firmware/`), `pixi run hooks` to install pre-commit.

For writer/schema or readback changes, also run `pixi run parquet-test --sanitize` and proportionate SD readback tests. The format tasks discover project C/C++ files, excluding build/vendor trees. Preserve vendored source bytes/licenses and their `.clang-format-ignore` exclusion. Project lint excludes vendor diagnostics and checks one configuration; the linked codec is tested with sanitizers.

For the measurement contract, also run `pixi run telemetry-contract-test --sanitize`. `telemetry_fields.inc` is the single source for names/types/procedure/unit/validity; update its compiled SHA-256 intentionally when editing it. Do not invent deployment, calibration, sensor serials or UTC. Version schema changes explicitly; backward compatibility is not required, but existing saved files must not be deleted or rewritten without authorization. Mermaid diagrams should distinguish implemented work from planned services; metadata compatibility is not OGC API compliance.

Keep **captures, exports, benchmark reports and retained firmware binaries in the trial's git-ignored `artifacts/` directory, outside `build/`**. A full-card keep-safe copy of the owner's data may live in the root `exports/` directory (git-ignored); it is the owner's data, not repository evidence, so `docs/` cites file hashes rather than that directory. Arduino cleaned `build/` during a changed-configuration rebuild and removed the first local exports/logs; SD files were retained and all finalized Parquet files were restored into `artifacts/`. Never treat a build cache as evidence storage. Record artifact identities/hashes in the relevant board's `bench-verified.md`.

## Hard rules

1. One owner per peripheral or controller. Do not initialize the same I2C, SPI, or I2S controller from two driver families.
2. Treat every version number in the docs as a dated observation. Recheck upstream before changing a dependency, and pin exact versions plus lockfiles.
3. Do not copy Core or Core2 GPIO numbers. CoreS3 pin assignments differ.
4. Record the board revision and SKU when a source is ambiguous, and say so in the doc rather than picking a guess silently.
5. Most documentation here is source-checked, not bench-tested. Actual measurements belong in that board's `bench-verified.md` with date, method, firmware/image identity, and limits. The board bench file wins when sources disagree. Do not promote source checks into it as measurements.

## Keeping docs current

Board facts stay in `docs/boards/<board>/`, while device protocol, Parquet and data-flow contracts stay in `docs/shared/`. Reusable code belongs in `firmware/common/` under its own `AGENTS.md`. Preserve verified wiring, conflicts, protocol edge cases, and source links. For routine APIs an agent can look up on its own, leave a keyword and a link instead of prose.
