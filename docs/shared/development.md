# Shared development workflow

This repository targets ESP32-S3 with native **ESP-IDF 6.1**. Application source
uses no Arduino APIs or Arduino component. Board pins, PSRAM mode, power
sequencing, storage mounting and hardware evidence belong to the board adapter;
the five opt-in components (AQCommon, AQRuntime, AQConnectivity, AQLogger and AQLocation)
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
| [CoreS3](../../firmware/esp-idf-cores3/README.md) | `bringup/main.cpp` and board telemetry adapter | 16 MB flash, Quad PSRAM, USB Serial/JTAG console, native M5 components and SDSPI on existing SPI2 | Short native USB Parquet/SD/network UTC check passed; phone transfers/endurance pending; [image evidence](../boards/m5stack-cores3/bench-verified.md#board-1-first-native-esp-idf-flash-and-readback) |
| [Waveshare V2](../../firmware/esp-idf-waveshare-sim7670g/README.md) | `logger/main.cpp` or `diagnostic/main.cpp`, board IO adapter | 16 MB flash, OPI PSRAM for logger, no PSRAM for diagnostic, UART0 console and one-bit SDMMC | Native qualification pending; historical Arduino images in the board bench record |

`tools/idf-dependencies.lock` pins ESP-IDF 6.1 and its exact source commit.
Component manifests pin M5Unified **0.2.26**, M5GFX **0.2.33**, mDNS **1.14.0**
and Waveshare led_strip **3.1.0~1**; official H3 **4.5.0** is vendored with exact
source/license hashes in `firmware/common/location/vendor/h3/manifest.json`.
Policy is to pin the latest stable release of every dependency. Rechecking on
**2026-10-16** confirmed M5Unified **0.2.26**, M5GFX **0.2.33**, mDNS **1.14.0**,
led_strip **3.1.0~1**, ESP-IDF **6.1** and H3 **4.5.0** as latest. Dated device
evidence records which component versions each qualified image used. LZ4 remains vendored **1.10.0** with checked hashes; the
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
and flash the verified image without an implicit rebuild or full-chip erase. They use
Pixi's pinned esptool with the validated generated `flash_args` response file;
ESP-IDF 6.1 has no `idf.py --no-deps` option. Only bootloader, partition table,
OTA metadata and app0 regions are written. NVS is outside this plan.

```sh
pixi run cores3-flash <checked-port> backup/m5stack-cores3-flash-<timestamp>.bin
pixi run waveshare-flash <checked-port> backup/waveshare-sim7670g-v2-flash-<timestamp>.bin
pixi run waveshare-flash <checked-port> backup/waveshare-sim7670g-v2-flash-<timestamp>.bin diagnostic
```

Follow [the full identification/backup sequence](../../AGENTS.md#before-any-physical-flash-write)
first. Never erase station/config NVS or format the card. The migration retains
NVS namespaces, keys and value types and existing flash partition offsets;
The logger configuration retains the earlier two-universal-MAC policy
(`CONFIG_ESP32S3_UNIVERSAL_MAC_ADDRESSES_TWO=y`), preserving Bluetooth base+1.
IDF's four-MAC default would shift Bluetooth to base+2 and fail to load the
existing singleton local IRK. Bonded client reconnect still needs qualification;
never erase keys to work around an identity mismatch.

If the checkout or managed environment moved and a tool reports an old
interpreter path, run `pixi install` to reconcile it. Use `python -m esptool` /
`python -m espefuse` through project tasks rather than stale entry-point shebangs.

## Native migration validation 2026-10-09

The migration passed native board and generic component builds plus host
contracts. Those results establish compilation and logic compatibility.
A subsequent [dated CoreS3 native flash/readback](../boards/m5stack-cores3/bench-verified.md#board-1-first-native-esp-idf-flash-and-readback)
qualifies only its short SD/USB/sensor/time run. Waveshare native hardware
remains pending. Historical Arduino results retain their original image scope.
Proto 3 phone transfers, reconnect, coexistence, sustained arbitration and
power-cut durability need separate device qualification.

## Dependency and shared-code policy

Use current official documentation and verify the latest stable release before
choosing or changing an SDK/component version. Pin the exact version/source
identity and generated dependency resolution; document any older compatibility
choice with evidence. Do not silently track floating `latest`/`master` or add
Arduino APIs/components. SDK and host-tool environments remain separate.

AQCommon, AQRuntime, AQConnectivity, AQLogger and AQLocation stay board-neutral and reusable.
Each board owns pin maps, controller initialization, power/display/sensor/RTC
capabilities, filesystem mounting and its measurement/provenance dictionary.
Reuse board-neutral interfaces with borrowed IO callbacks rather than copying
a sibling's GPIO, PSRAM or mount policy.

## Adding a board

Use the [ESP32-S3 porting note](board-porting.md) for logger hooks, transport
configuration, hardware capabilities and the qualification sequence. The
[adapter contract](../../firmware/common/logger/README.md) owns logger lifecycle
and callback details. Shared logic stays in `firmware/common`, with no board-name
conditionals or sibling trial dependencies.

## Verification

Every firmware PR runs the baseline below and builds both boards. Use cached
build directories and the pinned Pixi environment. Run additional gates for the
changed subsystem. Preserve vendored bytes and licenses. Generic fixtures never
get flashed.

```sh
pixi run fmt-check
pixi run lint
pixi run common-test
pixi run telemetry-contract-test --sanitize
pixi run archive-sync-test
pixi run control-sync-test
pixi run wifi-link-test
pixi run logger-status-test
pixi run connectivity-build-test
pixi run cores3-build
pixi run waveshare-build
```

| Changed area | Additional Pixi tasks |
| --- | --- |
| Writer, codec, footer or schema | `parquet-test --sanitize`, `reader-test`, `waveshare-contract-test --sanitize` |
| Plantower or LTR553 | `pms-frame-test`, `ltr553-test` |
| Disconnects, LAN rings or BLE policy | `disconnect-test`, `frame-ring-test`, `ble-policy-test`, `ble-advert-test`, `ble-sync-test`, `lan-concurrency-bench` |
| Settings, time or location | `config-test`, `network-time-test`, `location-test`, `logger-provision-test` |
| Logs and persistence | `debug-log-test`, `disconnect-test` |
| Logger queues and lifecycle | `logger-build-test`, `logger-work-queue-test`, `logger-watchdog-test`, `logger-rotation-test`, `logger-directory-test`, `logger-archive-reset-test` |
| Storage governor, CRC cache or DMA buffer | `storage-governor-test`, `crc-cache-test`, `sd-transfer-buffer-test` |
| Backup tooling | `backup-test` with offline fake esptool |
| Diagnostic or PSRAM configuration | `waveshare-build diagnostic`, `waveshare-psram-build` |

Task definitions in `pixi.toml` own exact commands and sanitizer defaults. The
archive, transport, settings, logger and geometry host fixtures compile actual
implementation code against platform fakes. `logger-directory-test` includes
FatFs mount-root handling through `stat`, since `mkdir` at the mounted root can
return `EINVAL`. `sd-transfer-buffer-test` covers the 4 KiB internal DMA buffer
reservation, failed-mount release/retry and lifetime, rather than SD timing.

The LAN task owns non-blocking socket IO. The storage worker services samples
between bounded ring/credit waits and consumes copied requests serially.
`logger-work-queue-test` covers queue-before-wakeup, sample priority and
saturation. None of these host gates proves throughput, radio coexistence,
physical persistence or power-cut durability. Use the
[LAN qualification procedure](lan-performance.md) and dated board evidence
before making those claims. Shared mount assertions enforce the
[descriptor budget](../../firmware/common/src/archive_files.h). Shared SDK
defaults enable fast seek and board target checks enforce radio settings. Do not carry old generated sdkconfig values into a new target.

## Local provisioning and client boundaries

The shared [sync protocol](ble-sync-protocol.md) retains its UUIDs/opcodes and
512-byte BLE frame cap. STATUS/LIVE JSON allows 480 bytes. Clients request MTU
517 where possible; MTU 483 carries a maximum snapshot in one notification.
Notifications exceeding negotiated MTU − 3 are omitted after the complete value
is cached; use characteristic long reads for complete snapshots.
LIVE `t`/`rh` are ambient only and CoreS3 sends IMU temperature as `imu_t`. A board
without an RTC hook has no retained clock anchor; UTC stays null after boot until
automatic host SET_TIME or successful Wi-Fi SNTP. Raw unsynchronized rows remain in the `unsynced` tree; the mobile archive reconciles derived partitions from same-boot anchors.

Physical UART owner commands (`owner-pin`, `wifi-profile`, `config-hex`) bypass
DebugLog for secret-bearing replies. Use `pixi run owner-pin` and
`pixi run provision-usb` with explicitly checked ports; host tools must keep
credential profiles in memory and exclude them from captures and artifacts.
They add no radio opcode or secret CONFIG JSON field. The real SET_CONFIG
validator/actions remain the single provisioning implementation.

## Local time and deployment location

The [time/location contract](time-and-location.md) describes default DHCP/router,
configured local and permitted public NTP order, offline RTC/host anchoring and
explicit mobile H3 provisioning. Use `pixi run network-time-test`, `location-test`,
`config-test` and `control-sync-test` for these changes, plus both schema reader
gates and affected board builds for logger location changes. These sanitizer
fixtures compile the official H3 core and real service/settings code. Existing
generated sdkconfig values override defaults: regenerate or explicitly update
`CONFIG_LWIP_SNTP_MAX_SERVERS=3` and
`CONFIG_LWIP_DHCP_GET_NTP_SRV=y` in disposable build configurations when moving
from the earlier native image. Also regenerate the logger configuration to select
`CONFIG_ESP32S3_UNIVERSAL_MAC_ADDRESSES_TWO=y`; the target checker rejects FOUR.
Do not modify owner NVS or physical flash for a
build configuration update.

Source and host checks do not establish router option-42 behavior on a specific
network, NTP accuracy/authenticity, physical radio coexistence, phone permission
or location behavior, SD timing or power-cut durability. The later CoreS3
native run observed successful network UTC and a short USB file readback;
router/local/public transitions and location provisioning remain unqualified.
Finalized owner archives and old schemas were
not rewritten.

## Owner maintenance

Physical serial `parquet erase-archive CONFIRM` (CoreS3 v6.10 / Waveshare v2.4)
removes only `/sd/output` and historical `/sd/parquet`, including partials and
quarantine. It closes archive/partial handles, discards the RAM batch, pauses
acquisition, rejects further archive requests and remains paused until reboot,
even after a deletion error. It never formats the card or deletes unrelated
card content. Use only for an explicitly requested destructive reset. The
`PARQUET ERASE_ARCHIVE` response reports success and deletion counts; a host
fixture does not prove physical FAT durability.
