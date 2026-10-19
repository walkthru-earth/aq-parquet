# Mobile app migration notes

Companion app builds may break. Released firmware wire contracts stay readable,
because boards in the field keep speaking them. Each section below records one change
the Android and iOS apps (opensensor-space-mobile) can observe, newest first,
with what changed, why, and what the app must do. The wire contract itself
lives in [ble-sync-protocol.md](ble-sync-protocol.md). Files already on a phone
or SD card are never rewritten by the firmware, so the app must keep reading
the older shapes it has stored, or the owner clears them.

## PARQUET BEGIN first in each boot (fix-35-begin-order, issue #35)

**What changed.** The storage worker now waits until the logger has printed
`PARQUET BEGIN`, so in `LOG_TAIL` each boot's `PARQUET BEGIN` comes before its
worker lines, such as `PARQUET RECOVERY`, `PARQUET QUARANTINE`, `PARQUET SCAN`
and the mount error. Before, a fast card could print those first. The text of
every line is unchanged, only the order is now guaranteed.

**Why.** Tools that split a log into boots at each `PARQUET BEGIN` could file a
new boot's recovery lines under the boot before it.

**What the app must do.** Nothing. An app that groups `LOG_TAIL` lines by boot
may now start a boot at its `PARQUET BEGIN`. Lines from older firmware may
still show the old order.

## Quarantine walk error lines (fix/iterative-dir-walk, issue #37)

**What changed.** The startup partial quarantine can print three new lines in
`LOG_TAIL`. `PARQUET ERROR operation=quarantine-depth dir=<relative path>
max_depth=6` names a directory too deep to enter. `PARQUET ERROR
operation=quarantine-entry name=<relative path> errno=<n>` names one entry
that could not be inspected or opened, which the walk skips before going on.
`PARQUET ERROR operation=quarantine-walk errno=<n>` reports a directory read
error that ended the walk early. All three raise the existing error count.
Before, those cases were skipped with no line, and one bad entry stopped the
rest of the walk.

**Why.** The walk is now iterative so its stack is fixed, and a skipped tree
must be visible.

**What the app must do.** Nothing. It may show these as storage errors.

## LIVE key stability rule (docs only, opensensor-space-mobile #153)

**What changed.** [ble-sync-protocol.md](ble-sync-protocol.md#live-key-stability)
now states that a LIVE key never changes meaning once released, and that a new
meaning gets a new key. Firmware releases that change the LIVE layout say
**needs app update** in their release notes and in their entry here. No
firmware, key, digest or identity string changes.

**Why.** A digest that moves only because of a comment or whitespace edit used
to stop live readings on every app that did not know it yet.

**What the app must do.** Keep mapping known digests to their reviewed
profiles. For an unknown digest, read only the keys in the LIVE table, by
name, and ignore the rest. `imu_t` stays board temperature and `t` stays
ambient air.

## Longer DIAG tasks and DIAG heap lines (fix-37-stack-budgets, issue #37)

**What changed.** The hourly `DIAG tasks` line in `LOG_TAIL` gains
`esp_timer=<bytes>` and `sys_evt=<bytes>` after `uptime_s`, and `DIAG heap`
gains `psram_largest=<bytes>` after `psram_min`. The board also prints both
lines when the owner sends `parquet mem` on the USB console. Existing fields
keep their names, order and meaning.

**Why.** Bench runs size task stacks and check heap fragmentation from these
lines, next to the static budgets in `tools/stack_budget.json`.

**What the app must do.** Nothing. The app does not parse these lines. A
reader that splits `key=value` pairs should ignore keys it does not know.

## Incremental LIST_SINCE with a saved cursor (fix-38-list-since, issue #38)

**What changed.** A new control opcode `LIST_SINCE` (`0x10`) lists only the
files of this station that are new since a 32-byte cursor. It answers `FILE`
frames, then a 48-byte `LIST_SINCE_END` (`0x4E`) whose first 15 bytes match
`LIST_END`, plus a `flags` byte and the next cursor. STATUS JSON gains
`"ls":<u32>`, the FAT volume serial of the mounted card, 0 when unknown. To
keep STATUS under 480 bytes, `utc_ms` is clamped to 13 digits and `clk_age_s`
and `miss` to the u32 range, which no valid clock reaches. Plain `LIST`,
`LIST_END` and every other frame are unchanged. The layouts, goldens and
errors are in
[ble-sync-protocol.md](ble-sync-protocol.md#list_since).

**Why.** Plain `LIST` reads every directory entry on the card each sync, which
grows with the archive. On a 5161-file test tree the incremental walk read 107
entries against 6688 for the full walk.

**What the app must do.**

- Store the cursor per station, keyed by station id plus the cursor's
  `mount_epoch`, as the 32 opaque bytes from `LIST_SINCE_END`. Never build or
  edit one. Send the all-zero cursor when none is stored, or when STATUS `ls`
  differs from the stored epoch.
- Save the new cursor only after a valid `LIST_SINCE_END`. An `ERROR`, a lost
  link or `ABORT` keeps the previous cursor, and the files already received
  are still worth downloading.
- On `ERROR` 11 with op `0x10`, fall back to plain `LIST` for that board and
  stop sending `LIST_SINCE` until the firmware version changes.
- Merge results into the local archive and dedupe by relative name plus CRC
  from `OPENED`. A `full` or `unsynced_full` listing repeats files the phone
  already holds, which is expected.
- Run a periodic audit, a plain `LIST` or `LIST_SINCE` with the all-zero
  cursor, for example once a day or every tenth sync. It catches a file
  written below the floor after a clock moved back across a reboot, and the
  unsynced files of a boot whose NVS ring write and `ringless=` marker
  folder both failed. A boot with only a failed ring write leaves the marker,
  and every later listing repeats its files in full until a later append in
  that boot removes it or the archive is erased, so dedupe
  them as above. Other stations,
  `legacy-parquet/` and `benchmarks/` also appear only in a full listing.
- Read `ls` as optional, absent on older firmware.

## One BLE HCI fail line per refused interval run (fix/52-followup-nits, after #52)

**What changed.** The board retries a connection interval request that is
refused while the central runs a procedure of its own. The link setup and the
idle timer retry every second, and a refused request from a `control` write
is retried by the next write. Each retry used to add `BLE HCI fail cmd=conn op=0x2013 rc=15 timeout=0` to serial and
`LOG_TAIL`. Now only the first refusal of a run logs it. A different `rc`, a
success, a new connection or a host reset starts a new run. The
`BLE CONN request=` line in `LOG_TAIL` still records every attempt with its
`rc`.

**Why.** Serial showed the same failure every second for as long as the
central's procedure ran, which buried the lines around it (PR #52 review).

**What the app must do.** Nothing. The app does not parse these lines.

## LZ4_RAW by default and a saved codec choice (feat/25-persist-codec, issue #25)

**What changed.** `parquet codec none` and `parquet codec lz4` now save the
choice in NVS (u8 key `codec` in the existing `parquet` namespace), so it
survives a reboot. A board with nothing saved, which is every board updating
from an earlier image, now writes LZ4_RAW files instead of UNCOMPRESSED.
What the phone can see.

- New files carry LZ4_RAW column chunks and footer `compression=LZ4_RAW`
  unless the owner chose `codec none`. Columns, field ids, types, the schema
  string, the dictionary digest and file names are unchanged.
- STATUS JSON `codec` reads `"LZ4_RAW"` right after boot instead of
  `"UNCOMPRESSED"`. The key and its two values are unchanged.
- LOG_TAIL gains a boot line `PARQUET CODEC codec=<name> source=nvs`, or
  `source=default reason=missing|open|read|corrupt` when nothing usable is
  saved. `PARQUET BEGIN codec=` now names the boot codec instead of always
  `UNCOMPRESSED`. The codec command reply is
  `PARQUET CONFIG codec=<name> persistent=true`, or `persistent=false` after
  `PARQUET ERROR operation=codec-store reason=open|write|commit`.
- No firmware identity string moves in this change, the next release image
  carries it.

**Why.** LZ4_RAW halved complete files on the CoreS3 bench (28,537 to
14,080 B) and finalized faster, and the host survey shows about 59 % fewer
data bytes on a 90 row file. Before this change every reboot fell back to
UNCOMPRESSED, so the saving was lost after the first reset.

**What the app must do.** Nothing required. `:core:parquet` already decodes
LZ4_RAW on Android and iOS, and files already stored keep their own codec. The
app does not parse the LOG_TAIL lines. An app that shows the STATUS `codec`
value should expect `LZ4_RAW` on most boards.

## COREDUMP line in the LOG ring (fix/49-coredump-report, part of issue #49)

**What changed.** The first boot after a new core dump is saved to flash
writes one additive line to the LOG ring, which `LOG_TAIL` returns.

```
COREDUMP present=1 valid=1 bytes=24992 sha=0708090a0b0c0d0e task=parquet-sd pc=0x4037a1b2 exc_cause=1 exc_vaddr=0x00000000 elf=3f2a1b0c prior=none saved=1
```

`valid` is `1` when the stored checksum matches, `0` when the dump is
corrupt and `unknown` when the flash could not be read. `bytes` is the image
size, `sha` the first 8 bytes of its stored SHA-256 in hex, `task` the
crashed task, `pc`, `exc_cause` and `exc_vaddr` the exception registers, and
`elf` a prefix of the crashed image's ELF SHA-256. Each of `task`, `pc`,
`exc_cause`, `exc_vaddr` and `elf` is `-` when the summary is not available.
`prior` is `none`, `other` (an older dump was reported before), `corrupt` or
`unknown` (the saved record could not be read). `saved` is `1` once the
device has recorded the dump as reported. When the dump cannot be identified
the line is `COREDUMP present=unknown valid=0 error=read-failed` or
`COREDUMP present=1 valid=0 error=invalid-size`. Nothing is saved for such a
dump, so that line repeats on every boot until the partition changes. Later
boots stay quiet for an identified dump unless the record could not be read or
saved. The dump is never
erased. No frame layout, opcode, error code, LIVE key or column changed.

**Why.** ESP-IDF printed the saved dump on the console only, so a crash in the
field left no trace in what the app collects.

**What the app must do.** Nothing. The line is plain LOG text. An app that
shows device logs may flag a `COREDUMP` line, and parse it as space separated
`key=value` pairs that may grow new keys. The values hold no secrets.

## Bonded reconnect serves the first LIST (fix/51-ble-auth-state)

**What changed.** On some bonded reconnects the board never saw the encryption
event, so its cached "authenticated" flag stayed false while the link was in
fact encrypted. The first `LIST` after such a reconnect was refused with
`ERROR` 7 and that `ERROR` was dropped too, so the app saw nothing. The board
now reads NimBLE's live security state, so that `LIST` is served. Code 7 is
never sent for an authentication reason any more. An unpaired phone still
sees what it saw before. NimBLE refuses its unencrypted Control write with an
ATT Insufficient Encryption, Encryption Key Size or Authentication error, which
starts pairing. It can still subscribe to `response`, because the CCCD has
no security flag, but receives nothing there except a possible `ERROR` 14.
`ERROR` 14 with `detail="auth"` over BLE is only a guarded fallback, for a
Control write that passes ATT but fails the firmware check. In this
configuration both apply the same rule, so an app should not expect to see it.
Serial and `LOG_TAIL` gain a line `BLE BONDED authenticated=true bonds=N
via=access`, `via=connect` or `via=subscribe` when the board learns the link
is secure without the encryption event. A later encryption event on that
link logs no second `BLE BONDED` line. A bonded central that encrypts
before the board processes the connection (macOS does) now keeps the
`response` subscription NimBLE restores from the bond. Before, that
subscription was lost, so every frame of the first `LIST` was refused and
its closing `ERROR` 7 never arrived. The board logs such a restore as
`BLE SUBSCRIBE early=N response=1`. A response frame refused because
`response` is not subscribed logs `BLE REFUSE reason=unsubscribed`, and one
refused because the link is not secure logs `BLE REFUSE reason=insecure`,
each once per session. No frame layout changed.
The board also stopped rebooting on some bonded reconnects. It used to
request the LE data length while handling the connection, and when that
request got no answer while the phone started encryption, the BLE host reset
and the board crashed. The data length request now goes once the link is
secure, before the 2M PHY request, and is skipped when the phone already set
251 octets. Serial and `LOG_TAIL` gain `BLE HCI fail cmd= op= rc= timeout=`
when a link request fails (`timeout=1` means the BLE host resets), and
`BLE LINK wait phy after=mtu`, `after=conn` or `after=phy`,
`BLE LINK dle=N by central` and `BLE LINK phy=2m by central` while the link
sequence runs. The app does not parse any of these lines.

**Why.** Issue #51, a reconnect looked like a hung sync.

**What the app must do.** Keep treating an ATT Insufficient Authentication,
Encryption or Encryption Key Size error on a Control write as "pair again".
That is the normal unpaired case. Should `ERROR` 14 ever arrive on BLE, treat it the same way,
like a bad LAN token. Never retry either as busy. Handle 7 as
before, by its `detail`. A plain busy 7 can be retried later, while
`detail="mtu"` needs a larger MTU and `detail="count"` will not succeed by
repeating the same request. Nothing for the new link lines. An app that shows
device logs may flag `BLE HCI fail`.

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
  release until the unknown digest fallback ships (opensensor-space-mobile
  #153). The LIVE key stability rule above supersedes this for apps that have
  the fallback.
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
