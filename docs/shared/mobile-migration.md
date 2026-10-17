# Mobile app migration notes

Companion app builds may break. Released firmware wire contracts stay readable,
because boards in the field keep speaking them. Each section below records one change
the Android and iOS apps (opensensor-space-mobile) can observe, newest first,
with what changed, why, and what the app must do. The wire contract itself
lives in [ble-sync-protocol.md](ble-sync-protocol.md). Files already on a phone
or SD card are never rewritten by the firmware, so the app must keep reading
the older shapes it has stored, or the owner clears them.

## READ after a storage remount can answer bad handle (fix/34-remount-archive-handles)

**What changed.** When the device remounts the SD card while a phone holds an
open handle, the next `READ` on that handle runs the finalized check again. The
same size and CRC-32 keep the handle. A deleted, edited or swapped file now
answers `ERROR` 5 (bad handle) for that `READ` and every later `READ` or
`CLOSE` on the handle. Before this change the device reopened the path and
served the new bytes under the old handle. No frame layout or opcode changed.

**Why.** A card can be pulled, edited on a PC and put back, or swapped, while
the phone keeps its handle. The old reopen mixed bytes from two files under
one size and CRC, so the phone only caught it at its own CRC check.

**What the app must do.** Nothing. `SyncEngine` already turns a READ
`ERROR` 5 into `FileOutcome.DeviceError` for that file without reopening or
looping, logs the refused `CLOSE` and moves on to the next file. The next run
lists again and resumes the partial only if `OPENED` size and CRC still match.

## Smaller resident writer workspace (feat/multi-chip-portability-and-watchdog)

- `PARQUET BEGIN psram_workspace_bytes=` drops by about 51 KB because the
  codec-test benchmark file is no longer resident. Waveshare V2 at schema v4
  measured 162520 bytes on 2026-10-16. Value change only.
- LOG_TAIL `PARQUET ERROR operation=codec-test reason=no-psram` is replaced by
  `PARQUET ERROR operation=allocate mem=psram bytes=N purpose=codec-test`.
- A failed workspace allocation at boot now logs
  `PARQUET ERROR operation=allocate mem=psram bytes=N purpose=writer-state
  logging=false`. Other boot allocation failures keep the old line.
- The app does not parse these lines, so no app change is required.

## Storage worker watchdog (feat/multi-chip-portability-and-watchdog)

- LOG_TAIL `PARQUET ERROR operation=worker-stall` gains a trailing
  `watchdog=armed|warn|off` field. Additive.
- New LOG_TAIL lines `PARQUET WATCHDOG mode=... timeout_s=... consecutive=...`
  at boot and `PARQUET ERROR operation=worker-watchdog-reset consecutive=N`
  after a stall reset. Additive.
- A stalled storage worker now resets the device after about 60 s instead of
  staying wedged. The app sees a normal BLE/LAN disconnect and reconnect.
- New LOG_TAIL line `PARQUET WATCHDOG mode=armed consecutive=N
  reason=recovered` when a stalled worker moves again before the reset. The
  device keeps running and a later stall can trip again. Additive.
- `PARQUET WATCHDOG ... supervised=0` now comes with `mode=warn`, since a
  worker without its supervisor timer is never left armed.
- A serial `parquet get` and its CRC pass count as worker progress, so a
  large USB export no longer logs `worker-stall` or trips the watchdog.
- No STATUS, LIVE, GET_DIAG, GET_CONFIG or advertising field changes. The app
  does not parse these log lines today, so no app change is required.

## CoreS3 acquisition configuration v3 (refactor/board-neutral-logger-and-sensors)

- The CoreS3 file metadata `configuration_id` changes from
  `cores3-acquisition-v2` to `cores3-acquisition-v3`, and the `configuration`
  JSON now records `"m5unified":"0.2.26"`, the version the firmware is built
  with. It said 0.2.25 since the dependency moved in #19.
- Schema, dictionary, columns and firmware identity are unchanged.
- The app does not need to change unless it pins `configuration_id`. If it
  does, accept `cores3-acquisition-v3`.

## LIVE keys come from each board's field table

**What changed.** The `live` notification and the LAN `LIVE` push no longer
use one hardcoded list of 22 keys. Each board declares a `live_key` next to the
fields it wants on LIVE, and the shared logger emits exactly those keys, in
the board's field-table order, for values that are valid in the queued row.

- `t` now always means ambient air temperature. CoreS3 has no resolved ambient sensor,
  so `t` stays absent there.
- CoreS3 sends its IMU die temperature as the new key `imu_t`. Before this
  change it was sent as `t` whenever ambient temperature was null.
- Key order follows the board dictionary. For example `c1`, `c25`, `c10` now
  come before `pm1`, `pm25`, `pm10` on both boards.
- Non-finite float values are omitted like any other null.

**Why.** The shared logger carried board knowledge (which fields exist and the
`t` fallback rule) that belongs in the board. A key with two meanings also
made the LIVE temperature tile depend on the board type.

**What the app must do.**

- Treat `t` as ambient air temperature only. Show `imu_t` as board or die
  temperature, never as air temperature.
- Parse LIVE as an unordered JSON object and look keys up by name.
- Ignore keys it does not know, and treat a missing key as null.
- Use `info.schema` and the dictionary to decide which tiles a board can
  ever show, instead of a fixed key list.

## UTC columns in microseconds and Parquet field ids (CoreS3 v6.12, Waveshare v2.6)

**What changed.**

- `event_time_utc_ns` is now `event_time_utc_us` and `clock_anchor_utc_ns` is
  now `clock_anchor_utc_us`. Values are INT64 microseconds since the Unix
  epoch, annotated `TIMESTAMP(MICROS, isAdjustedToUTC=true)`. The dictionary
  unit is `us_since_unix_epoch` (was `ns_since_unix_epoch`).
- Every leaf column now carries a Parquet `field_id`. The id is the column's
  1-based position in the board's `telemetry_fields.inc`, and dictionaries are
  append only, so an id never moves to another column.
- CoreS3 writes schema `cores3-telemetry-v5` (row value 5), dictionary
  `cores3-telemetry-v4`, firmware `idf-cores3-parquet-v6.12`. Waveshare writes
  schema and dictionary `waveshare-sim7670g-telemetry-v4` (row value 4),
  firmware `idf-waveshare-parquet-v2.6`. Both dictionary SHA-256 values change.
- Unchanged. The `utc` key in the LIVE document is still nanoseconds. The
  footer key `reconciliation_anchor_utc_ns` is still nanoseconds. Column count,
  column order and all other names and types are unchanged.

**Why.** The phone or host registers device files in Apache Iceberg v3 tables.
`TIMESTAMP(MICROS, UTC)` is Iceberg `timestamptz` in every format version, so
the files go into a v3 table (or an older v2 table) without rewriting. The
device clock is microsecond, so NANOS would add no precision, would end in
2262, and would map only to the v3 type `timestamptz_ns`, which Spark, Athena
and Redshift cannot read. Iceberg identifies columns by field id, not by name,
so every leaf now carries one.

**What the app must do.**

- Read the UTC instant from `event_time_utc_us` when present and fall back to
  `event_time_utc_ns` for older files. Convert by unit, never by name alone.
  Check the Parquet logical type (`MICROS` or `NANOS`).
- Accept schema versions 5 (CoreS3) and 4 (Waveshare) and the new dictionary
  digests. Do not reject a file only because its schema string is newer.
- Create Iceberg tables as format v3. When building a manifest or schema, use
  each leaf's `field_id` as the Iceberg field id. Leave row lineage fields to
  the committing library, the device does not write them.
- Older files have no ids and store UTC as plain INT64 or NANOS. A name mapping
  cannot change a column's type, so keep them out of the Iceberg table or
  rewrite them on a host.
- Commit with a library that supports v3 `add_files` style registration, for
  example iceberg-go 0.7.0 or later, or iceberg-rust 0.9.0 or later with data
  file entries built from the footer metrics. PyIceberg 0.12 cannot write v3
  tables yet.
- When reconciling unsynchronized files from footer anchors, compute
  microseconds as `reconciliation_anchor_utc_ns / 1000 + (monotonic_us -
  reconciliation_anchor_mono_us)`.
- Keep treating LIVE `utc` as nanoseconds.
