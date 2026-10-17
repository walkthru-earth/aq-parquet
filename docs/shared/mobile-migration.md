# Mobile app migration notes

Companion app builds may break. Released firmware wire contracts stay readable,
because boards in the field keep speaking them. Each section below records one change
the Android and iOS apps (opensensor-space-mobile) can observe, newest first,
with what changed, why, and what the app must do. The wire contract itself
lives in [ble-sync-protocol.md](ble-sync-protocol.md). Files already on a phone
or SD card are never rewritten by the firmware, so the app must keep reading
the older shapes it has stored, or the owner clears them.

## BLE auth refusal answers ERROR 14 (fix/51-ble-auth-state)

**What changed.** On some bonded reconnects the board never saw the encryption
event, so its cached "authenticated" flag stayed false while the link was in
fact encrypted. The first `LIST` after such a reconnect was refused with
`ERROR` 7 and that `ERROR` was dropped too, so the app saw nothing. The board
now reads NimBLE's live security state, so that `LIST` is served. A Control
write on a link that really is not encrypted and authenticated now gets a
visible `ERROR` with code 14 and `detail="auth"`. Code 7 now only means busy.
Serial and `LOG_TAIL` gain a line `BLE BONDED authenticated=true bonds=N
via=access`, `via=connect` or `via=subscribe` when the board learns the link
is secure without the encryption event. No frame layout changed.

**Why.** Issue #51, a reconnect looked like a hung sync.

**What the app must do.** Treat `ERROR` 14 on BLE as "pair again", the same
as a bad LAN token and the same as an ATT Insufficient Authentication or
Encryption error on a Control write. Never retry it as busy. Handle 7 as
before, by its `detail`. A plain busy 7 can be retried later, while
`detail="mtu"` needs a larger MTU and `detail="count"` will not succeed by
repeating the same request.

## ABORT stops a LIST within one step (fix/33-abort-list-walk)

**What changed.** The device walks the card for `LIST` in steps of at most 32
directory entries and checks for ABORT before each step and each `FILE`. An
ABORT now stops a `LIST` within one step, even on a large card. Before, the
walk only stopped at the next `FILE`, and the whole walk ran first. The reply
is unchanged, `FILE` frames already sent, then `ERROR` 16, then `ABORTED`
naming `LIST`. Other links and LAN peers are served between steps. A file
finalized during the walk may be missing from that listing, and `new_files`
then stays set in the advert. `LIST_END.partials` is approximate when a
`.partial` file is finalized during the walk. Only one `LIST` runs at a time,
and a `LIST` from another link or LAN peer now gets `ERROR` 7 `detail="list"`
at once instead of waiting for the running one. A full card no longer
remounts during a `LIST`, so a long walk is not cut off every 60 s. A remount
that still ends a `LIST`, for example after `FLUSH` or a config change, now
answers `ERROR` 7 `detail="remount"` instead of `ERROR` 8, because the card
comes back and the app should retry. An erase or a directory read failure
still answers `ERROR` 8. When the device needs its storage worker for
something more urgent, such as a critical battery close, the walk ends early
with `ERROR` 7 `detail="preempted"` instead of after the whole walk, and a
`LIST` sent while that close is due gets the same reply at once. No frame
layout, opcode or error code changed.

**Why.** On a large card the abort drain could outrun the app's bound of 4096
frames, 4 MiB or 30 s and close the link with `DrainExceeded`. A long walk
also held the storage worker away from samples and the watchdog heartbeat.

**What the app must do.** Nothing. `LinkSession` already accepts any number of
`FILE` frames, then `ERROR` 16 and `ABORTED` in either order, and only checks
that the `FILE` count matches `LIST_END.count`. The drain now ends sooner.
A preempted `LIST` is `ERROR` 7, which `DeviceSession` already maps to
`DeviceBusyException`, and `CollectionController` retries it after its
backoff like any busy reply. The same holds for the `detail="list"` busy
reply, which an app syncing over BLE and LAN at once can see on its second
link, and for `detail="remount"`. Before, a `LIST` cut by a remount ended the
collection as a device failure with no retry. The `DeviceBusyException` KDoc
should list the LIST details `preempted`, `list` and `remount`. On a critical
battery the device makes up to 8 close attempts with about 91 s of backoff
between them, and each retry can preempt a `LIST` again, so the app can spend its 5 attempt
retry budget and end with link busy. That is accepted on a dying battery and
needs no app change.

## CoreS3 LIVE keys covered by the dictionary digest (CoreS3 v6.13, issue #31)

- CoreS3 LIVE keys now live in `telemetry_fields.inc` as the last `FIELD`
  argument, the same as Waveshare, so the dictionary SHA-256 covers them.
- CoreS3 writes dictionary `cores3-telemetry-v5`, firmware
  `idf-cores3-parquet-v6.13`. Schema stays `cores3-telemetry-v5` (row value 5)
  with the same 81 columns, order, types and field ids. Configuration stays
  `cores3-acquisition-v3`.
- The new digest is
  `e5060fb7fedbcabf665b1c8a159a77f5fe9283ac34bd2d89f1bc98b0f027d38b`. Its LIVE
  keys, in order, are `seq`, `mono`, `utc`, `pms`, `c1`, `c25`, `c10`, `pm1`,
  `pm25`, `pm10`, `n03`, `n05`, `n1`, `n25`, `n5`, `n10`, `imu_t`, `als`,
  `vbus`, `bat`, `pct`, `chg`, `t`, `rh`. These are the same keys as the
  previous build. `t` and `rh` are mapped but always null on CoreS3.
- The legacy digest
  `22291d3aab63365404394f30c1549fb18bde0699df9b2de863de70bb3f4edf65` covers
  two released LIVE key sets that both report `idf-cores3-parquet-v6.12`. The
  build at e166978 sends the IMU die temperature as `t`. The build at d2a840c
  sends it as `imu_t`. Nothing in `info` tells them apart.
- The app must map `e5060fb7…` to the existing `LiveProfiles.CORES3` profile
  from opensensor-space-mobile #87, unchanged. That profile leaves out `t` and
  `rh` on purpose because they are always null on CoreS3, so do not copy the
  key list above into it. It reads `imu_t` as board temperature, never as air
  temperature. Tracked in opensensor-space-mobile #90.
- Until the app knows `e5060fb7…`, `LiveProfiles.resolve` returns null for it
  and the app shows "Live readings need newer firmware". Synced files still
  work.
- Release order. Ship the app release that maps `e5060fb7…` first, then flash
  v6.13 to field boards. Bench boards may run v6.13 earlier.
- Suggested mobile follow up for `22291d3a…` only. Read `imu_t`, and when it is
  missing read `t` as board temperature. Both carry the IMU die temperature on
  CoreS3, because ambient temperature is never valid there.
- Frozen key sets for every digest since CoreS3 `f35d7398…` (v6.9 to v6.11)
  and Waveshare `0a024b85…` (v2.6) are in `tools/fixtures/live_keys/`,
  including the first Waveshare v2.7 build `d0d01294…`. `f35d7398…` sends the
  IMU die temperature as `t`, like the e166978 build, so it matches
  `LiveProfiles.CORES3_LEGACY` in #87. The contract tests fail when a board's LIVE keys
  change without a new digest, or when a new digest has no fixture.
- The digest is the SHA-256 of the raw `telemetry_fields.inc` bytes, so a
  comment, whitespace or line ending edit also moves it and needs an app
  release.
- Phone side code may break freely. Firmware keeps every released wire contract
  readable, and any new LIVE key set gets a new dictionary digest.

## Battery health, cell voltage without SOC and the emergency close (Waveshare v2.7, PR #24, issues #28, #29 and #30)

This one entry describes the final v2.7 contract. CoreS3 is unchanged.

- Schema and dictionary. Waveshare writes `waveshare-sim7670g-telemetry-v5`
  (row value 5), firmware `idf-waveshare-parquet-v2.7`, configuration
  `waveshare-sim7670g-acquisition-v3`, 62 columns. The v5 dictionary digest is
  `67688d0fd2e86053c1c66b961a1446ad3c225ee71823e0fadb771b645c7183ff`.
- Six columns are appended, field ids 57 to 62.
  `battery_soc_raw_percent` (FLOAT, uncapped gauge SOC),
  `battery_charge_rate_percent_per_h` (FLOAT, MAX17048 CRATE, negative while
  discharging), `gauge_status_flags` (INT32, bit0 RI, bit1 VH, bit2 VL, bit3 VR,
  bit4 HD, bit5 SC, bit6 EnVr), `gauge_hibernating` (INT32, 0 or 1),
  `battery_health_state` (INT32 code) and `battery_minutes_to_empty` (INT32,
  null unless the SOC is trusted and discharging within 7 days).
- `battery_health_state` and the LIVE key `bstate` share the codes 0 unknown,
  1 external power likely, 2 ok, 3 low, 4 critical, 5 settling after a gauge
  reset. External power is inferred from cell voltage only, and the
  thresholds are uncalibrated. `bstate` is absent when no battery is
  configured. At boot a cell resting on the critical threshold still leaves
  unknown after three reads, as critical when at least half of them read
  critical, otherwise as low.
- Cell voltage without SOC. A failed or implausible SOC no longer hides a good
  cell voltage. Rows can have `battery_mv` set while `battery_percent`,
  `battery_soc_raw_percent` and `battery_minutes_to_empty` are null, with
  `gauge_status=1`, and the reverse is possible. LIVE follows the row, so
  `bat` can be present without `pct` and the reverse. `bstate` can reach 4
  critical from voltage alone. The `gauge_status` footer metadata text now
  reads `...2=valid; gauge_status stays 1 when only VCELL is valid,
  battery_mv is still written`. The codes 0, 1 and 2 keep their meaning, and
  2 still needs both words. This text is file metadata, not part of the
  digest.
- LIVE keys live in `telemetry_fields.inc` as the last `FIELD` argument, so
  the digest covers them. The keys and their order are `seq`, `mono`, `utc`,
  `pms`, `c1`, `c25`, `c10`, `pm1`, `pm25`, `pm10`, `n03`, `n05`, `n1`, `n25`,
  `t`, `rh`, `bat`, `pct`, `bstate`. Any change to a board's
  `telemetry_fields.inc`, including a live key, changes that digest, so it
  needs a matching app release before boards are reflashed.
- LOG_TAIL power lines. `POWER STATE state=<name> mv=<n|null>` on each state
  change, with ` action=finalize` once per critical entry. Boot adds `AQ GAUGE
  status=...`, after a gauge reset `AQ GAUGE ri_clear=<result>`, and `AQ GAUGE
  read` carries `vcell_ok`, `soc_ok`, `vcell_plausible` and `soc_plausible`.
- LOG_TAIL emergency close lines. The close runs on the storage worker with
  retries. `files=<f>` counts telemetry files finalized during the attempt,
  including one a storage probe closed before the close itself wrote.
  - `PARQUET FLUSH reason=power-critical result=closed attempt=<n> files=<f>`
  - `PARQUET FLUSH reason=power-critical result=<busy-retry|failed>
    storage=<state> attempt=<n> files=<f> retry_ms=<ms>`
  - `PARQUET FLUSH reason=power-critical result=nothing-open attempt=<n>
    files=<f>`, only after a retry. A first attempt with nothing open prints
    no line.
  - `PARQUET ERROR operation=power-close reason=<outcome> attempts=<n>
    files=<f> closed=0` when every retry failed.
- Transfers during a close. A BLE or LAN `LIST` or `READ` running when a close
  is due ends early with the existing busy error (`ERROR busy` for LIST,
  `READ_END` with the busy status for READ), also while it waits for a slow
  peer. A serial `parquet get` ends with `PARQUET ERROR operation=get
  reason=power-critical offset=<n>`, where `offset` is the next byte not sent.
  The app retries as it already does for busy. A close in storage Retry can
  remount the card, so an open READ handle may then answer bad handle, as in
  the #41 entry below.
- One discharge closes once. `bstate` can go 4 critical, 3 low and back to 4
  critical without a second close. Only 2 ok or 1 external power re-arms it,
  and not while 5 settling.
- No STATUS, GET_DIAG, GET_CONFIG, frame layout or advertising change.
- The app must accept schema 5 and dictionary v5 for Waveshare files, keep
  reading v4 files already stored, must not assume `battery_percent` or `pct`
  is present whenever `battery_mv` or `bat` is, and may show `bstate` as a
  battery badge.

## READ after a storage remount can answer bad handle (fix/34-remount-archive-handles)

**What changed.** When the device remounts the SD card while a phone holds an
open handle, the next `READ` on that handle runs the finalized check again. The
same size and CRC-32 keep the handle. A deleted, edited or swapped file now
answers `ERROR` 5 (bad handle) for that `READ` and every later `READ` or
`CLOSE` on the handle. A card that has not mounted again yet, or an I/O error
during the check, answers `READ_END` status 4 and keeps the handle, so a later
`READ` can still succeed. Before this change the device reopened the path and
served the new bytes under the old handle. No frame layout or opcode changed.

**Why.** A card can be pulled, edited on a PC and put back, or swapped, while
the phone keeps its handle. The old reopen mixed bytes from two files under
one size and CRC, so the phone only caught it at its own CRC check.

**What the app must do.** Nothing. `SyncEngine` already turns a READ
`ERROR` 5, and any non-zero `READ_END` status, into `FileOutcome.DeviceError`
for that file without reopening or looping, logs a refused `CLOSE` and moves
on to the next file. The next run
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
