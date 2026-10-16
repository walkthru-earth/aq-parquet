# Porting an ESP32-S3 board

The [firmware epic](https://github.com/walkthru-earth/aq-parquet/issues/6) uses
board-neutral services in `firmware/common`. A board adapter owns pins,
controller initialization, power, mount policy, measurements and capabilities.
Extend hooks or configuration for a new capability. Never branch on a board
name inside common code or include a sibling trial.

## Logger hooks and capabilities

The source contracts are [`aq_logger.h`](../../firmware/common/logger/src/aq_logger.h)
and [`sync_service.h`](../../firmware/common/connectivity/src/sync_service.h).
Descriptors, identity strings and callback contexts must outlive the services.

| Interface | Board supplies | Ownership and limits |
| --- | --- | --- |
| `aqlogger::Config` | Static field dictionary, schema/dictionary identities, digest, firmware, `created_by`, acquisition metadata and archive roots | Required runtime fields and types are in the [adapter contract](../../firmware/common/logger/README.md#adapter-contract). Measurements remain null when unavailable. Benchmark output stays outside synced archives |
| `Hooks::collect` | Sensor acquisition into a zero-initialized row | Main loop only. The runtime supplies clock, identity and standard counters |
| Storage hooks | `storage_total_bytes`, `storage_used_bytes`, `storage_free_bytes`, `storage_cluster_bytes` | Worker only with bus lock held. Free-space failure returns false and unknown cluster size returns zero |
| `Hooks::storage_remount` | Optional unmount/remount of the board-owned card | Worker only with bus lock held and no file open. Never format. Without this hook automatic remount recovery is unavailable |
| `Hooks::lock_bus`, `unlock_bus` | Both callbacks or neither | Borrow an existing bus mutex or let AQLogger create one. Complete display DMA before releasing a shared bus |
| `Hooks::rtc_read`, `rtc_write` | Optional UTC-second read/write with validation and verified write readback | Main loop only. Reject unset, invalid or voltage-low RTC data. Without RTC, UTC stays null until a real host/network anchor |
| `report_battery(state, mv)` | Optional battery state from `aq::battery::Monitor` in `firmware/common/power` | Main loop only, normally from `collect`. Prints one `POWER STATE` line per change and queues one finalize of the open file per critical entry through the storage worker. Never resets or sleeps |
| `Hooks::power_shed` | Optional load shedding on a battery state change | Main loop only, after the `POWER STATE` line. Default is no action. Must not block, reset or sleep |
| `start_links(display_detected)` | Actual display capability | Selects the first-boot pairing default. No display requires physical owner access to the per-device PIN |
| `Config::sync` | Optional `aqsync::ServiceConfig` containing BLE/LAN options and command/sample queue depths | Shared defaults own ring/credit sizes, timeouts, link parameters and advertising policy. No board-name conditionals |
| Board sdkconfig defaults | Flash size, PSRAM mode, console, DFS and hardware-specific deltas | Use the shared SDK/connectivity defaults and pinned target checker. CoreS3 Quad PSRAM and Waveshare OPI PSRAM are different capabilities |

Mount storage before `begin(config, hooks, sd_mounted)`. Use the shared
[`archive_mount_config()`](../../firmware/common/src/fat_volume.h) and
[`archive_files.h`](../../firmware/common/src/archive_files.h) descriptor budget,
including four persistent archive readers. Reserve the 4 KiB internal DMA SD
transfer buffer before radios and keep it alive while mounted. Then call
`start_links(display_detected)` once and `poll()` on every main-loop pass.
AQLogger alone accesses files after startup. AQConnectivity alone initializes
radios, the LAN task writes sockets, and the NimBLE host sends BLE responses.

## Porting sequence

1. Add one justified native `firmware/esp-idf-<board>/` application with board
   docs, exact PCB revision, upstream wiring sources, pinned manifests,
   partition table and setup/build/flash wrappers. Keep source checks and bench
   results separate. Do not copy another board's GPIO or controller policy.
2. Register common component directories with CMake `EXTRA_COMPONENT_DIRS` and
   declare the required components. A logger uses
   `REQUIRES common runtime connectivity logger location`. Select the Plantower
   model explicitly and preserve each board's schema and provenance.
3. Add Pixi tasks, target checks and board contract fixtures. Build both existing
   boards plus the new target using the [development gates](development.md#verification).
   A generic compile fixture never gets flashed.
4. Audit strapping pins, peripheral ownership, flash capacity, PSRAM, console
   and recovery. Before any physical write follow the full checked-port
   [identification and backup sequence](../../AGENTS.md#before-any-physical-flash-write).
   Current backup/flash helpers accept only the two existing 16 MiB targets.
   Extend them deliberately for a different capacity. Preserve NVS identity,
   credentials, keys, types and partition offsets.
5. Qualify a small diagnostic, sensor byte contract, card logging and immutable
   readback on the exact board. Keep captures and image hashes in ignored trial
   `artifacts/` and owner copies in `exports/`. Record only observed results in
   that board's bench record. Host tests do not prove SD timing, coexistence,
   reconnect, power or durability. Add the board to the docs router and README.
