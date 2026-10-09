# Shared development workflow

This repository targets ESP32-S3 with native **ESP-IDF 6.1**. Application source
uses no Arduino APIs or Arduino component. Board pins, PSRAM mode, power
sequencing, storage mounting and hardware evidence belong to the board adapter;
the four opt-in components (AQCommon, AQRuntime, AQConnectivity and AQLogger)
belong to [`firmware/common`](../../firmware/common/README.md).

## Host and toolchain

Host dependencies are pinned in root `pixi.toml` and `pixi.lock`. Start with
`pixi install`; run host and firmware commands through `pixi run`. Add host
packages with `pixi add`. The SDK is installed separately by the pinned
`tools/setup-idf.sh`; `tools/idf.sh` enters its own Python/compiler environment
and invokes `idf.py`, while host validators continue to use Pixi.

```sh
pixi install
pixi run idf-setup
pixi run cores3-build
pixi run waveshare-build
pixi run waveshare-build diagnostic
```

| Board adapter | Source | Configuration | Hardware scope |
| --- | --- | --- | --- |
| [CoreS3](../../firmware/esp-idf-cores3/README.md) | `bringup/main.cpp` and board telemetry adapter | 16 MB flash, Quad PSRAM, USB Serial/JTAG console, native M5 components and SDSPI on existing SPI2 | Native qualification pending; historical Arduino images in the board bench record |
| [Waveshare V2](../../firmware/esp-idf-waveshare-sim7670g/README.md) | `logger/main.cpp` or `diagnostic/main.cpp`, board IO adapter | 16 MB flash, OPI PSRAM for logger, no PSRAM for diagnostic, UART0 console and one-bit SDMMC | Native qualification pending; historical Arduino images in the board bench record |

`tools/idf-dependencies.lock` pins ESP-IDF 6.1 and its exact source commit.
Component manifests pin M5Unified **0.2.25**, M5GFX **0.2.31**, mDNS **1.14.0**
and Waveshare led_strip **3.1.0~1**; these were the latest stable releases
observed on **2026-10-09**. LZ4 remains vendored **1.10.0** with checked hashes; the
[official latest release](https://github.com/lz4/lz4/releases/tag/v1.10.0) was
still 1.10.0 when checked on 2026-10-09.
Commit each board’s generated `dependencies.lock` to pin transitive component
resolution; downloaded `managed_components/` and build outputs are ignored. Use the [ESP-IDF 6.1 programming guide](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/get-started/index.html),
[native NimBLE APIs](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/bluetooth/nimble/index.html)
and [netif/SNTP manual](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/network/esp_netif_programming.html)
when changing integrations.

SDK downloads use `$AQ_TOOLCHAIN_ROOT`, default
`~/.cache/m5stack-aq-parquet/toolchains`; legacy `$M5_TOOLCHAIN_ROOT` is accepted.
`AQ_IDF_PATH` and `AQ_IDF_TOOLS_PATH` can select an already installed checkout
and SDK tools directory; the wrapper still enforces the exact source commit.
SDK compiler/Python tools default to the official shared `~/.espressif`;
`AQ_IDF_TOOLS_PATH` overrides that tools directory.
Each board owns its sdkconfig defaults, partitions and manifest dependencies.
Sharing a download cache does not make a sibling board a dependency.

Board wrappers verify dictionary/vendor hashes and generated ESP32-S3 flash,
PSRAM and partition configuration. Build output is ephemeral under
`firmware/<board>/build/<variant>/`; retain reviewable binaries and image hashes
in `artifacts/` before a real hardware run. The backup-gated flash wrappers
require the checked character-device port and matching 16,777,216-byte backup,
and flash the verified image without an implicit rebuild or erase:

```sh
pixi run cores3-flash <checked-port> backup/m5stack-cores3-flash-<timestamp>.bin
pixi run waveshare-flash <checked-port> backup/waveshare-sim7670g-v2-flash-<timestamp>.bin
pixi run waveshare-flash <checked-port> backup/waveshare-sim7670g-v2-flash-<timestamp>.bin diagnostic
```

Follow [the full identification/backup sequence](../../AGENTS.md#before-any-physical-flash-write)
first. Never erase station/config NVS or format the card. The migration retains
NVS namespaces, keys and value types and existing flash partition offsets;
Arduino-era BLE bond compatibility still needs native hardware qualification.

If the checkout or managed environment moved and a tool reports an old
interpreter path, run `pixi install` to reconcile it. Use `python -m esptool` /
`python -m espefuse` through project tasks rather than stale entry-point shebangs.

## Native migration validation 2026-10-09

| Gate | Confirmed result | Scope |
| --- | --- | --- |
| `cores3-build` | Passed with native ESP-IDF 6.1/M5 components | Source/link/flash-PSRAM-partition target checks; no native board flash |
| `waveshare-build` and `waveshare-build diagnostic` | Both passed | Logger OPI PSRAM and diagnostic no-PSRAM configurations; no native hardware run |
| `connectivity-build-test`, `logger-build-test`, `waveshare-psram-build` | Passed | Generic native shared components and PSRAM probe compilation; fixtures not flashed |
| `fmt-check`, `lint` | Passed | Project formatting and static diagnostics |
| `common-test`, `ltr553-test`, `config-test`, `control-sync-test`, `archive-sync-test`, `wifi-link-test`, `debug-log-test`, `logger-status-test`, `logger-provision-test`, `logger-work-queue-test`, `ble-advert-test` | Passed with applicable sanitizer tasks | Real shared implementation logic, native platform fakes and wire/queue/log bounds |
| `pms-frame-test`, `parquet-test --sanitize`, `telemetry-contract-test --sanitize`, `waveshare-contract-test --sanitize` | Passed | Sensor/writer/schema contracts and independent host reader checks |

These results establish native build and host-contract compatibility. They do
not establish physical USB/UART, PSRAM, display/card arbitration, SD timing,
radio coexistence, bonded reconnect, phone transfers or power-cut durability.
No native hardware qualification is claimed; add dated board/image readback
before extending the historical Arduino results.

## Dependency and shared-code policy

Use current official documentation and verify the latest stable release before
choosing or changing an SDK/component version. Pin the exact version/source
identity and generated dependency resolution; document any older compatibility
choice with evidence. Do not silently track floating `latest`/`master` or add
Arduino APIs/components. SDK and host-tool environments remain separate.

AQCommon, AQRuntime, AQConnectivity and AQLogger stay board-neutral and reusable.
Each board owns pin maps, controller initialization, power/display/sensor/RTC
capabilities, filesystem mounting and its measurement/provenance dictionary.
Reuse board-neutral interfaces with borrowed IO callbacks rather than copying
a sibling's GPIO, PSRAM or mount policy.

## Adding a board

1. Create `docs/boards/<board>/README.md`, hardware/pin ownership notes, sensor notes and a `bench-verified.md`. Record the exact PCB revision and upstream sources; keep measured and source-checked claims separate.
2. Create one justified, self-contained native `firmware/esp-idf-<board>/` adapter with README, pinned dependencies, setup/build/flash scripts, partition table and build settings. Do not include a sibling's sources. Add explicit root Pixi tasks and lint inputs.
3. Register common component directories in CMake `EXTRA_COMPONENT_DIRS` and declare `REQUIRES common` for AQCommon; opt into AQRuntime (`firmware/common/runtime`) for settings/logging, AQConnectivity (`firmware/common/connectivity`) for BLE/Wi-Fi/control/file sync, and AQLogger (`firmware/common/logger`) for the sampling deadline, single worker and archive lifecycle. Select the Plantower model explicitly. Keep board services and measurement definitions in the trial; pass a board-specific `created_by` when using the shared writer.
4. Audit GPIO/controller ownership, strapping pins, flash size, PSRAM mode, console transport and recovery before enabling peripherals. The common ESP32-S3 chip does not imply common board wiring.
5. Follow the [identification and full-backup sequence](../../AGENTS.md#before-any-physical-flash-write) on the checked port. Extend the backup/restore scripts deliberately for another flash capacity; the current helpers accept only the two 16 MiB targets.
6. Verify a small diagnostic, then the sensor byte contract and storage on a checked card. Add a versioned measurement schema before logging; unavailable measurements stay null. Use AQLogger with static schema/provenance descriptors, a board acquisition callback and already mounted storage capacity callbacks. Supply paired bus callbacks when borrowing a board mutex, and optional RTC callbacks on the main-loop task. AQLogger owns the nonblocking request queue, shared ArchiveSession, clock/flush/reboot/status and storage lifecycle; the board owns peripheral setup and actual measurement/RTC IO. Sharing the implementation does not prove it has run on another board.
7. Retain captures and image/ELF files in the trial's ignored `artifacts/`, and record hashes, date, commands and limitations in that board's bench file. Add the board to the [docs router](../README.md) and root README.

## Verification

Run `pixi run fmt-check` and `pixi run lint` for project C/C++ changes. The formatter discovers project sources while excluding build/vendor trees. Preserve vendored source bytes and notices.

Shared Plantower changes require `pixi run pms-frame-test`. Writer/codec/footer changes require `pixi run parquet-test --sanitize`; measurement-contract changes require `pixi run telemetry-contract-test --sanitize`. Build every affected native board and relevant diagnostic. Host tests establish parser/format behavior; board-specific storage, timing and power-loss claims require real hardware evidence.

The shared module gates are `pixi run common-test`, `ltr553-test`, `config-test`, `control-sync-test`, `archive-sync-test`, `wifi-link-test`, `connectivity-build-test`, `logger-build-test`, `logger-work-queue-test`, `debug-log-test`, `logger-status-test` and `logger-provision-test`. The Waveshare dictionary also requires `pixi run waveshare-contract-test --sanitize`. See the [module map](../../firmware/common/README.md) for ownership and callbacks. The generic ESP32-S3 compile fixture never gets flashed.

The LAN task waits for socket readiness with a 100 ms housekeeping timeout; it
no longer sleeps after every request. The archive worker checks one sample and
one command per pass, and waits on a binary wakeup only when idle. All sample,
serial and radio producers copy to the bounded queues before signaling; wakeups
may coalesce without dropping queued work. `logger-work-queue-test` covers the
queue-check/wait race, pre-start signals, sample priority and saturation. These
are scheduling and host-test guarantees, not measured on-board transfer speeds.
See [LAN performance qualification](lan-performance.md) for the one-versus-two
collector procedure and current validation limits.

## Local provisioning and client boundaries

The shared [sync protocol](ble-sync-protocol.md) retains its UUIDs/opcodes and
512-byte BLE frame cap. STATUS/LIVE JSON allows 480 bytes. Clients request MTU
517 where possible; MTU 483 carries a maximum snapshot in one notification.
Notifications exceeding negotiated MTU − 3 are omitted after the complete value
is cached; use characteristic long reads for complete snapshots.
Interpret LIVE `t`/`rh` through the board's advertised schema/dictionary. A board
without an RTC hook has no retained clock anchor; UTC stays null after boot until
automatic host SET_TIME or successful Wi-Fi SNTP. Raw unsynchronized rows remain in the `unsynced` tree; the mobile archive reconciles derived partitions from same-boot anchors.

Physical UART owner commands (`owner-pin`, `wifi-profile`, `config-hex`) bypass
DebugLog for secret-bearing replies. Use `pixi run owner-pin` and
`pixi run provision-usb` with explicitly checked ports; host tools must keep
credential profiles in memory and exclude them from captures and artifacts.
They add no radio opcode or secret CONFIG JSON field. The real SET_CONFIG
validator/actions remain the single provisioning implementation.
