# LAN transfer scheduling and qualification

The 2026-10-08 change removes fixed scheduling delays from the shared firmware
used by both phone apps. In [wifi_link.cpp](../../firmware/common/connectivity/src/wifi_link.cpp),
the server waits for socket readiness instead of polling and then sleeping
100 ms after each pass. Incoming requests wake the wait immediately; its
100 ms timeout bounds radio/settings, expiry and snapshot housekeeping. With
no listening socket, the task still sleeps; socket errors also back off.

In [aq_logger.cpp](../../firmware/common/logger/src/aq_logger.cpp), the storage
worker checks one sample before one command per pass instead of waiting
50 ms for a sample before each command. Successful sample, serial and radio
enqueues copy into their bounded queue and then signal a binary semaphore,
created before task startup. [The wakeup helper](../../firmware/common/logger/src/aq_logger_work_queue.h)
waits only when no queued sample remains and no command was received. Signals
remain pending across the idle-check/wait race and can coalesce because the
queues retain every item. A one-second idle timeout maintains the worker
heartbeat. Neither queue is drained in an unbounded loop.

This preserves the wire contract, frame and READ bounds, token authentication,
session generations, multi-client isolation, Wi-Fi modem sleep and BLE
coexistence policy. The single storage worker remains the only filesystem
owner. Sampling stays on its 10-second monotonic cadence; finalized files
remain immutable and downloads require size, CRC-32 and `PAR1` checks. No
credentials are added to diagnostics. The change removes avoidable waits;
it does not establish that they explain every observed slow transfer.

## Validation on 2026-10-08

The pinned [Pixi tasks](../../pixi.toml) passed: `wifi-link-test`,
`logger-work-queue-test`, `archive-sync-test`, `control-sync-test`, `common-test`,
`logger-status-test`, `logger-provision-test`, `fmt-check` and `lint`. The host
regressions cover incoming data interrupting an idle socket wait, bounded idle
waiting, authentication and session isolation, plus deterministic worker
queue-check/wait races, pre-start signals, coalescing and saturation.

Both `arduino-build` (CoreS3) and `waveshare-build`, and the generic
`logger-build-test`, compile successfully. These builds use the current working
copy, which also contains unrelated Waveshare configuration/schema work; their
binary hashes must not be described as isolated scheduling-only images.
`waveshare-contract-test --sanitize` currently fails at
[its firmware identity assertion](../../tools/test_waveshare_contract.py): it
expects `arduino-waveshare-parquet-v2`, while the pre-existing working-copy
identity differs. The scheduling change does not alter that identity.

No board was flashed for this change. Host socket timing and compilation are
not measured Wi-Fi throughput, coexistence, sampling or SD results on a board.
Keep Android, iPhone and host-collector results separate.

## Read-only qualification after an authorized deployment

Follow the root and selected board's flash/backup instructions before any
firmware deployment. Retain the exact deployed app/image identity and a
baseline result from the previous image. Never rotate the token, change radio
settings, flush, delete or rewrite sensor archives for a speed comparison.

1. Stop all other collectors. Use a discovered `_aqsync._tcp` endpoint and an
   already enrolled token file in ignored private artifacts. Pass only its file
   path, never a literal token in a shell command or retained report. Run
   `pixi run ble-sync --lan <discovered-host> --token-file <private-path> info`
   and `list`. Verify the advertised `max_read` is 16384; select one finalized
   immutable file large enough for several READ windows.
2. Fetch that exact relative Hive path with one collector into a fresh ignored
   output root:
   `pixi run ble-sync --lan <discovered-host> --token-file <private-path> fetch '<relative-path>' --out <artifacts-root>/one`.
   This client uses the advertised READ limit. Retain its nonsecret JSON result
   (file bytes, windows, elapsed seconds, bytes/s and CRC-32), along with file
   SHA-256 and reader validation. The transfer timer includes OPEN through CLOSE
   and excludes subsequent local reader inspection.
3. Repeat the same fetch concurrently in two terminals, with separate fresh
   roots `<artifacts-root>/two-a` and `two-b`. Keep only these two sessions
   active. Compare each collector's speed and aggregate throughput with the
   single-collector run, and confirm identical size, CRC-32 and SHA-256. Repeat
   each scenario three times on the same network and sensor position. Record
   RSSI, board/image identity and whether BLE was connected; never record a PIN,
   Wi-Fi PSK or token. The existing `lan-concurrency-bench` uses 4 KiB windows
   and a full three-slot pool; it is a different isolation test, not this
   one-versus-two 16 KiB comparison.
4. Repeat on Android and iPhone with the same file and network conditions, first
   each alone and then both together. Confirm progress, cancellation/reconnect,
   archive validation and continued sampling without new deadline misses or
   dropped rows. A host collector result does not qualify either phone.
5. Record dated actual board results and deployed-image hashes in that board's
   `bench-verified.md`, and phone observations in the mobile repository's
   `docs/status.md`. Report the measured improvement or remaining bottleneck;
   do not infer an on-board speed gain from the host scheduling regression.

## Radio diagnostics and console backpressure

The CoreS3 build identity for this follow-up is `arduino-cores3-parquet-v6.7`;
its measurement schema and dictionary remain unchanged, and older finalized
files retain their original provenance. The Waveshare working-copy identity
belongs to unrelated configuration/schema work and is unchanged by this fix.

High-rate Wi-Fi/BLE command and archive OPEN/READ/CLOSE diagnostics now use
[DebugLog's `record_only()` facade](../../firmware/common/runtime/src/debug_log.h).
It stores the complete diagnostic write in the existing bounded `LOG_TAIL`
ring and never calls the borrowed console sink. This prevents a blocked USB
console from delaying request enqueueing or READ_END delivery. The archive
worker's serial `get`, `list`, `status` and physical owner/provisioning replies
retain their previous console path; no serial TX timeout is changed. Normal
boot and sampling diagnostics retain their console behavior. Radio diagnostic
lines are retrieved through LOG_TAIL rather than a serial capture. Pairing
diagnostics describe random/fixed mode and never record the returned PIN.

The cached Arduino-ESP32 3.3.11 `HWCDC::write` uses a default 100 ms timeout
and up to 20 consecutive TX-ring retries when the USB host stops draining.
Two synchronous log writes previously gated each READ window. Availability
clipping alone would leave a check/write race, so these diagnostic sites use
no console IO at all. `debug-log-test` compiles the real implementation and
checks blocked-sink isolation, complete newest-ring retention, successful
serial forwarding and ring-only behavior even with a writable sink. Normal
console diagnostics can still encounter SDK backpressure; this change is
limited to the radio/archive request hot paths.

The follow-up passes `debug-log-test`, `wifi-link-test`, `config-test`,
`control-sync-test`, `logger-provision-test`, `telemetry-contract-test --sanitize`,
`fmt-check` and `lint`, and both board builds. The prior Waveshare contract
identity assertion remains a separate working-copy limitation. Build/test
results alone do not establish a post-deployment throughput gain. Record actual
old/new-image measurements separately in the selected board's bench record.
