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
resolution. The Waveshare diagnostic variant keeps its own in
`diagnostic/dependencies.lock`, so building it leaves the logger lock alone. Downloaded
`managed_components/` and build outputs are ignored. Use the [ESP-IDF 6.1 programming guide](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/get-started/index.html),
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

### C++ dialect

Firmware and host tests are pinned to C++23. ESP-IDF 6.1 adds `-std=gnu++26`
for chip targets, so every repo-owned component calls
`aq_pin_cxx_standard(${COMPONENT_LIB})` from
[`firmware/cmake/aq_cxx_standard.cmake`](../../firmware/cmake/aq_cxx_standard.cmake),
which appends `$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>`. Board and fixture
projects include that file before `project.cmake`. C sources such as vendored
H3 keep the SDK C dialect, and third-party components keep the SDK default.
The standard is named in four places. Change them together.

| Place | Spelling |
| --- | --- |
| `firmware/cmake/aq_cxx_standard.cmake` | `-std=gnu++23` in the function body |
| `tools/host_cxx.py` | `CXX_STD = "-std=c++23"` for every `tools/test_*.py` |
| `pixi.toml` task `pms-frame-test` | `clang++ -std=c++23` |
| `tools/lint.sh` | cppcheck `--std=c++23` |

`python tools/check_idf_target.py --cxx-dialect` fails when the four disagree,
or when a `tools/test_*.py` hardcodes a C++ `-std` instead of importing
`CXX_STD`. C standards such as `-std=c99` for H3 stay allowed.
`tools/lint.sh` runs it first. Every board build runs `check_idf_target.py`,
which repeats that check and reads `build/<variant>/compile_commands.json`. It
fails when the last `-std` on any repo-owned `.cpp` is not the pinned GNU
dialect, or when a repo `.c` file gets a C++ standard. `managed_components` and
build outputs are skipped. The logger compile test, the connectivity
fixture and the PSRAM probe have no board target, so they run only that
per-file check with `check_idf_target.py --compile-dialect <build>`, and
`compile-dialect-test` fails if one of them stops calling it. Style rules live at the top of
[`AGENTS.md`](../../AGENTS.md#c-standard-and-style).

`tools/lint.sh` then runs `tools/check_board_conditionals.py`, which fails on
any `#if`, `#ifdef`, `#ifndef` or `#elif` in `firmware/common` (vendor code
skipped) whose condition names a board, a modem or a chip target, such as
`CORES3`, `WAVESHARE`, `SIM7670` or `CONFIG_IDF_TARGET_ESP32S3`, also inside a
longer macro name, on a continued line or past a block comment. A name in a
trailing comment does not count. `CONFIG_IDF_TARGET_ARCH_*` names a CPU family
and stays allowed. Lint passes only on exit 0, so a check that cannot run
fails it. `board-conditionals-test` covers the rule. Shared code takes board
capabilities from hooks, `ServiceConfig` and sdkconfig defaults instead.

### Source paths

Two checkouts of one commit build the same image. Every repo-owned component
also calls `aq_map_source_paths(${COMPONENT_LIB})` from
[`firmware/cmake/aq_source_paths.cmake`](../../firmware/cmake/aq_source_paths.cmake),
which adds `-ffile-prefix-map=<repo root>=.`. `__FILE__` in asserts and
`ESP_ERROR_CHECK` then reads `./firmware/common/...` instead of the checkout
path, because the SDK's `CONFIG_COMPILER_HIDE_PATHS_MACROS` maps only the board
project directory. Each board project also calls `aq_map_debug_paths()`, a
`-fdebug-prefix-map` of the repo root for every component, since the app image
embeds the ELF SHA-256 and third-party debug info (managed components, the
NimBLE copy in the build directory) would otherwise still carry the path.
Neither flag changes code. The board build and `--compile-dialect` fail when a
repo C or C++ file misses the map. `pixi run repro-paths-check [board]
[variant]` builds HEAD twice and compares, see CI below. It needs the SDK and
the network for managed components, and it leaves uncommitted edits out.

Source paths in the ELF are relative to the repo root. Function and line
resolve from anywhere, but run `addr2line`, `gdb`, `idf.py monitor` and the
core dump tools from the repo root so they find the source files.

On 2026-10-17 one `std::format_to_n` call added to the CoreS3 image at `-Og`
with xtensa GCC 15.2.0 cost about 341 KB of flash image and 5 KB of DIRAM
against the same build with `snprintf`. Device code keeps `snprintf` until a
later toolchain shows a smaller cost.

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

Every firmware PR runs the host gate and builds both boards. Use cached
build directories and the pinned Pixi environment. Run additional gates for the
changed subsystem. Preserve vendored bytes and licenses. Generic fixtures never
get flashed.

```sh
pixi run gate
pixi run connectivity-build-test
pixi run cores3-build
pixi run waveshare-build
```

`pixi run gate` runs `tools/gate.sh`. It runs `fmt-check`, `lint` and every
host contract and fixture task in a fixed list, with ASan and UBSan wherever
the task supports `--sanitize`. It stops at the first failure and prints which
task failed. Pass `--keep-going` to run the whole list and see every failure.
Tasks that need a board, a serial port, Bluetooth, a LAN peer or the ESP-IDF
SDK are not in the gate. Add a new host test task to that list when you add it
to `pixi.toml`. The clang, clang-format and cppcheck used by the gate come from
exact Pixi pins on macOS and Linux, so local runs and CI see the same tools.

### Continuous integration

`.github/workflows/ci.yml` runs on every pull request, on pushes to `main`,
and by hand from the Actions tab (`workflow_dispatch`).

- **Workflow lint** runs `actionlint` (with `shellcheck`) and `zizmor` on
  `.github/` at exact conda-forge pins through `pixi exec`. Run the same
  commands locally before changing a workflow.
- **Host gate** runs `pixi run gate --keep-going` on Ubuntu with the locked
  Pixi environment. Each task runs under GNU `timeout` from the Pixi
  `coreutils` pin, 300 s by default (`AQ_GATE_TASK_TIMEOUT` overrides it), and
  a stalled task is reported as `TIMEOUT` by name in the gate summary. The
  gate refuses to start when a `*-test` task in `pixi.toml` is missing from its
  list, except the two SDK fixtures the board job runs.
- **Board builds** run `pixi run idf-setup`, then `cores3-build`,
  `waveshare-build logger`, `waveshare-build diagnostic`, and the generic
  `connectivity-build-test` and `logger-build-test` fixtures. The SDK comes
  from the same `setup-idf.sh` path developers use, so CI builds exactly the
  commit in `tools/idf-dependencies.lock`. The SDK and ccache caches are
  restored on every run but written only by pushes to `main`, and the SDK is
  saved from the cores3 leg alone, so pull requests never evict the `main`
  entries. `waveshare-psram-build` stays a manual SDK fixture.
- Each board job uploads its `idf.py size --format json2` report and
  `aq-build-sha256.json` as the `size-<board>` artifact, and the ELF, map and
  `flash_args` as `firmware-<board>`. Both are kept 90 days for `main` builds
  and 7 days otherwise. The job summary shows a size table. For a pull request
  the change is measured against the green build of its base commit, else
  against the newest green `main` build, and the summary names which commit
  was used. A malformed baseline prints a warning and the absolute table.
- Each board job then runs `ota-slot-check`, which fails when the app image
  uses more than half of the smallest app slot in that variant's
  `partitions.csv`. The share and the measurements behind it live in
  `tools/ota_slot_check.py`.
- **Reproducible image paths** runs `repro-paths-check waveshare logger`. It
  builds that image in two git worktrees of the commit at paths of different
  length, with `SOURCE_DATE_EPOCH` set to the commit time, and fails unless
  every file in the two `aq-build-sha256.json` manifests matches, the app,
  bootloader, partition table, OTA data, `sdkconfig` and flash arguments. The
  compiler database and project description hold absolute paths by design and
  are left out. On a failure it names the file, the count of differing bytes
  and whether a checkout path is in it. The job runs without ccache.
- **CI result** waits for every other job and fails unless each one passed.
  A skipped job passes only when its id is listed in `ALLOWED_SKIPS` in that
  job, which is empty today because no job has an `if:`. It is the single
  required check.
- Dependabot opens one grouped pull request a week for action pin updates,
  only for releases at least 7 days old (`.github/dependabot.yml`).

CI proves compilation and host logic only. It never flashes a board, and board
runs stay manual and dated in each bench record. Every action is pinned to a
full commit SHA. Check the latest stable release before changing a pin.

#### Required checks

Make only `CI result` required for `main`. In Settings, Rules, Rulesets, open
or create the `main` branch ruleset, enable Require status checks to pass, add
`CI result` with the source set to GitHub Actions, and save.

`CI result` needs every other job, so jobs and board matrix entries can be
added or renamed without a ruleset change. A new job must be added to its
`needs` list, or its failure will not block a merge. A job that gains an `if:`
must also be added to `ALLOWED_SKIPS`, or its skip blocks the merge. Keep the
job name `CI result` stable. If a merge queue is enabled later, add a
`merge_group` trigger to the workflow at the same time.

| Changed area | Additional Pixi tasks |
| --- | --- |
| Writer, codec, footer or schema | `parquet-test --sanitize`, `reader-test`, `waveshare-contract-test --sanitize` |
| Plantower, LTR553 or MAX17048 | `pms-frame-test`, `ltr553-test`, `max17048-test` |
| Battery health or logger power hooks | `battery-health-test`, `logger-power-test`, `waveshare-contract-test --sanitize` |
| Disconnects, LAN rings or BLE policy | `disconnect-test`, `frame-ring-test`, `ble-policy-test`, `ble-advert-test`, `ble-sync-test`, `lan-concurrency-bench` |
| Settings, time or location | `config-test`, `network-time-test`, `location-test`, `logger-provision-test` |
| Logs and persistence | `debug-log-test`, `disconnect-test` |
| Logger queues and lifecycle | `logger-build-test`, `logger-work-queue-test`, `logger-watchdog-test`, `logger-codec-store-test`, `logger-rotation-test`, `logger-directory-test`, `logger-archive-reset-test` |
| Storage governor, CRC cache or DMA buffer | `storage-governor-test`, `crc-cache-test`, `sd-transfer-buffer-test` |
| Backup tooling | `backup-test` with offline fake esptool |
| App signing or the tracked secrets guard | `release-sign-test`, `secrets-check-test` |
| CMake helpers, compile flags or `sdkconfig` defaults | `repro-paths-check`, `compile-dialect-test` |
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

## Local app signing

The app signing key lives only on the owner machine, in `secrets/` at the
repository root. It is never a GitHub secret and CI never sees it. `secrets/`
is ignored by git, and `pixi run lint` runs `tools/check_secrets.py`, which
fails when any path under `secrets/` is tracked or when a tracked file holds a
PEM private key header. `pixi run secrets-check` runs the same check alone.

Firmware does not check app signatures yet. Turning that on is #40, and it
waits on an owner-approved bootloader reflash. Until then a signed image boots
exactly like an unsigned one, so signing only proves the release came from the
owner key.

ESP32-S3 Secure Boot v2 verifies RSA-3072 signatures only, so that is the one
scheme the task accepts.

### One-time owner setup

Run these from the repository root.

1. Create the directory, readable only by you.

   ```sh
   mkdir -m 700 secrets
   ```

2. Generate the key with the pinned espsecure. It writes the file readable by
   everyone, so restrict it right away.

   ```sh
   pixi run espsecure generate-signing-key --version 2 --scheme rsa3072 secrets/app_signing_key.pem
   chmod 600 secrets/app_signing_key.pem
   ```

3. Write `secrets/.env`. The key path is relative to `secrets/`.

   ```sh
   printf 'AQ_APP_SIGNING_KEY=app_signing_key.pem\nAQ_APP_SIGNING_SCHEME=rsa3072\n' > secrets/.env
   chmod 600 secrets/.env
   ```

   | Variable | Meaning |
   | --- | --- |
   | `AQ_APP_SIGNING_KEY` | Required. File name of the PEM private key inside `secrets/` |
   | `AQ_APP_SIGNING_SCHEME` | Optional. `rsa3072`, the default and the only accepted value |

   The file takes `NAME=VALUE` lines, an optional `export` prefix, `#`
   comments and optional quotes. Nothing else is read from it.

4. Back up `secrets/app_signing_key.pem` offline, for example on two encrypted
   USB drives kept in different places. Once #40 enables signed app checks, a
   device only takes an OTA image signed by this key. Losing the key then means
   every deployed device needs a USB reflash to take an update. Never copy the
   key into the repository, a cloud drive or a chat.

### Sign a release

Build the board images as usual, then sign the app `.bin` files into a
directory outside `secrets/`.

```sh
pixi run release-sign --out exports/signed \
  firmware/esp-idf-waveshare-sim7670g/build/logger/aq_waveshare.bin
```

For each input it runs `espsecure sign-data --version 2` to
`<out>/<name>.signed.bin`, then `espsecure verify-signature --version 2` on the
result. It then writes `<out>/SHA256SUMS` in `sha256sum` format. It refuses to
run when

- `secrets/.env` is missing or does not set `AQ_APP_SIGNING_KEY`
- git tracks any path under `secrets/`
- `secrets/`, `secrets/.env` or the key is open to group or other
- the key is outside `secrets/`, password protected or not an RSA-3072 key
- the output directory or an input is inside `secrets/`

It never prints the key, the `.env` values or any file contents. It names the
key only as `secrets/<name>`. `release-sign-test` and `secrets-check-test`
use throwaway keys in a temp dir and never touch `secrets/`.

## Owner maintenance

Physical serial `parquet erase-archive CONFIRM` (CoreS3 v6.10 / Waveshare v2.4)
removes only `/sd/output` and historical `/sd/parquet`, including partials and
quarantine. It closes archive/partial handles, discards the RAM batch, pauses
acquisition, rejects further archive requests and remains paused until reboot,
even after a deletion error. It never formats the card or deletes unrelated
card content. Use only for an explicitly requested destructive reset. The
`PARQUET ERASE_ARCHIVE` response reports success and deletion counts; a host
fixture does not prove physical FAT durability.
