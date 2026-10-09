# Waveshare V2 logger storage and local sync

**Historical Arduino hardware status, 2026-09-28:** full logger boot, bounded SD Parquet write/finalization/serial readback, unsynced/host-UTC behavior, PMS UART reception and OPI PSRAM operation were verified. BLE advertising/macOS discovery worked; encrypted pairing/file transfer and Wi-Fi/LAN remain unverified. The first macOS encrypted-pairing attempt timed out; the fixed-PIN callback fix was deployed and emitted its pairing marker, but secure first pairing remains incomplete. Wi-Fi was not configured. See [bench-verified.md](bench-verified.md) for physical evidence and [trial commands](../../../firmware/esp-idf-waveshare-sim7670g/README.md).

Current source uses pure ESP-IDF 6.1. Native logger/diagnostic builds and host
contracts pass; physical native SD, sensor, pairing and radio qualification is
pending. Historical readbacks below remain scoped to their original images.

## Controller and worker ownership

The board initializes native SDMMC once with CLK GPIO5, CMD GPIO4 and DATA0 GPIO6 in one-bit mode, mounts at `/sd` without formatting, and supplies capacity callbacks to AQLogger. UART1 belongs exclusively to PMS5003T reception at GPIO1/2. The one filesystem worker owns directory enumeration, Parquet writes, flushing, quarantine and archive reads after startup. There is no display/SPI sharing assumption on this board. Card-detect GPIO46 remains un-driven; camera use needs its own pin/controller audit.

The card is the origin of the owner's data. Serial, BLE and token-authenticated local LAN can copy finalized files to a phone/laptop archive without internet. Cellular/GNSS are not initialized and do not replace local copies. Wi-Fi stays off until configured; USB provisioning copies only Wi-Fi credentials/enable flags between checked owner-controlled consoles, leaving per-device identities, PINs and LAN tokens intact.

## Files and measurements

The trial's `logger/telemetry_fields.inc` now defines the 52-column `waveshare-sim7670g-telemetry-v2` dictionary; the physical readbacks below used v1's 49 columns. The build verifies the v2 SHA-256 and writes a repository URI, dictionary/firmware identities, acquisition configuration, V2 board identity and unknown deployment/calibration metadata in the footer. PMS5003T bytes 24/26 become signed deci-°C temperature and deci-percent RH, with four particle bins only. Three added v2 leaves keep provisioned sensor serial digits, an opt-in AirGradient batch candidate and its gate status per row; the full serial is not written as static file metadata because it can change while a file is open. The candidate is not Cairo-reference-validated, and raw PM remains separate. Warm/stale/error/model-mismatch raw measurements stay null. Explicitly absent battery produces `gauge_status=0` and null battery fields; installed battery with a failed/implausible MAX17048 read gives status 1/null, and a valid read gives status 2 with cell mV and estimated whole SOC percent capped at 100. A bounded USB-attached board readback verified status 2 and non-null battery fields on v1; see [bench evidence](bench-verified.md). No CoreS3-only measurements appear.

The engine captures one snapshot per ten-second monotonic deadline and reports missed deadlines, dropped rows, queue peak and storage errors. UART receipt time and collection completion bound acquisition; neither claims the sensor's phenomenon time or uncertainty. An explicit host UTC anchor or successful native SNTP callback starts a new epoch. There is no RTC restore; unsynchronized UTC stays null, with files under `station=<uuid>/unsynced/boot=<id>/`. Synchronized files use UTC `station/year/month/day` Hive partitions. Existing captured rows are never assigned a later anchor.

RAM batches hold at most 90 rows. Rotation supports 600/900/1800/3600 seconds; completed row groups are `fsync`ed and a file may hold up to eight groups. The open file stays `.partial` until its window closes, an explicit flush occurs or the group limit is reached. Finalization writes/syncs the footer, validates structure/CRC and renames to immutable `.parquet`. Interrupted partials are retained in quarantine, never silently repaired, deleted or formatted. A reset can lose unfinished RAM rows and leave footerless completed groups. Normal reset and `fsync` do not establish power-cut durability.

## Readback and remaining measurements

One unsynced SD file fetched over serial held **16 rows × 49 columns**, **12,060 bytes**, CRC-32 **`102bc7ff`**, with matching PyArrow/DuckDB values/nulls. The first two rows correctly withheld PMS measurements during warm-up; later rows carried valid PMS values, temperature **24.8 °C** and RH **41.9–42.2%**. `gauge_status=0`, battery fields and UTC/anchor fields stayed null. Adjacent sample intervals were within **1 ms** of ten seconds; no errors/drops were observed in this bounded run. A subsequent host-UTC change/readback verified synchronized behavior separately. These results do not establish UTC accuracy, longer endurance or power-cut durability.

Use one serial owner for subsequent checks:

```sh
pixi run parquet-device command --port <checked-port> "parquet status"
pixi run parquet-device command --port <checked-port> "parquet schema"
pixi run parquet-device capture --port <checked-port> --seconds 45 --out firmware/esp-idf-waveshare-sim7670g/artifacts/logger-capture.log
```

`parquet time`, `flush`, interval/codec changes and provisioning write device state; invoke them explicitly. Use `parquet-device fetch` to retain finalized files under this trial's `artifacts/`, then `pixi run python tools/inspect_parquet.py <fetched-file>` for both-reader validation. The CLI's `--help` gives exact command/fetch/bench arguments. Do not use `chip` as a live-status probe because exiting can reset the application.

Next measure encrypted BLE pairing/file transfer, Wi-Fi/LAN provisioning/file transfer, multi-group/rotation boundaries, reset-partial retention and real LZ4 readback. Check sampling/drop/deadline counters under radio transfers before quoting loaded timing or performance. Background triggers, RGB visual behavior and environmental accuracy need their own board/phone measurements. Retain captures and image/ELF hashes outside `build/`; record evidence in the bench file.

The existing Android companion has no schema-count whitelist in file sync/history and already registers ambient temperature/RH history columns. Its current LIVE parser discards `rh`, and its `t` widget says “board temp”; archived environmental history support does not prove complete live UI compatibility.
