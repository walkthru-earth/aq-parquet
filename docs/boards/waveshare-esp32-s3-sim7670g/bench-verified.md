# Waveshare V2 bench record

## Board identification, 2026-09-28

The owner identified the PCB as V2.0. With the board connected by its USB serial bridge at `/dev/cu.wchusbserial5B901533371`, `pixi run ports` listed the WCH USB serial interface separately from four Qualcomm modem ports. Read-only `pixi run chip --port ...`, `flash-id --port ...`, and `efuse --port ...` returned:

- ESP32-S3, silicon revision v0.2; eFuse identifies 8 MB embedded AP_3v3 PSRAM.
- Flash ID manufacturer `85`, device `2018`, capacity **16 MB**, quad flash wiring at 3.3 V.
- `SECURE_BOOT_EN=False`, `SPI_BOOT_CRYPT_CNT=0`, `DIS_DOWNLOAD_MODE=False`; the normal download path remains available.

The chip command's esptool exit toggled RTS to reset the board. These commands did not establish which PSRAM mode an Arduino firmware should select, whether the PMS5003T is producing valid frames, whether the TF card mounts, or whether a new image boots. Those require a separate diagnostic run. No physical BOOT/RESET action or switch change was needed for these reads.

## Full flash backup, 2026-09-28

`pixi run backup --board waveshare-sim7670g-v2 --port /dev/cu.wchusbserial5B901533371` read the complete image at the default serial speed in **1496.4 seconds**. The hash command from the already-running script encountered an old Pixi interpreter path; the completed file was independently checked with `ls -l` and `/usr/bin/shasum -a 256` before any firmware write.

- Local ignored file: `backup/waveshare-sim7670g-v2-flash-20260927T222602Z.bin` (the name uses UTC).
- Exact size: **16,777,216 bytes**.
- SHA-256: `d88ddcd6bbbc69972396df9a1d67d75b006f5058d000e07cd850717fffd07a45`.
- `pixi run backup-partitions <file>` decoded the original partition table: NVS, OTA data, two 1280 KiB applications, SPIFFS and coredump. Its highest partition end was 4 MiB, despite the physical 16 MiB flash. This is a readback of the saved image, not evidence of its application's capabilities.

## Diagnostic build

The Arduino-ESP32 3.3.11 / Arduino CLI 1.5.1 diagnostic builds with PSRAM disabled, UART0 console and a one-bit SDMMC probe. Compiler output: **353,853 bytes** program storage, **22,288 bytes** static RAM. Disabling PSRAM here avoids guessing a mode before a separate boot/readback measurement.

Retained files under the trial's ignored `artifacts/firmware/`:

| File | SHA-256 |
| --- | --- |
| `waveshare-v2-pms5003t-sdprobe.bin` | `d64b70c46971efe2689e5e9af99de2e753eeb64d8dadcfc128a9f25900834300` |
| `waveshare-v2-pms5003t-sdprobe.elf` | `eb6528c06c2f9c0a64f4172d8107f9d7d4bffceed1c3208f1163d471051b3fe1` |

## Diagnostic flash and boot, 2026-09-28

After verifying the backup, `pixi run waveshare-flash <checked-port> <backup>` uploaded the bootloader, partition table, OTA initialization and application. Esptool verified the written hashes and reset via RTS; no physical button or switch action was required.

`pixi run capture --port /dev/cu.wchusbserial5B901533371 --reset --seconds 45 --out <trial-artifacts-log>` recorded the diagnostic boot and four ten-second reports:

- `flash_bytes=16777216`, `psram_bytes=0` (PSRAM is intentionally disabled in this image), `rx=1`, `tx=2`.
- `AQ TF pins_ok=1 mounted=0`, with SDMMC error `0x107` during card initialization. The owner confirmed **no TF card was inserted**; this does not measure a card or prove the slot's operation.
- `AQ PMS frames=0`, checksum/length error counters both zero, and `values=null`, including after the 30-second warm-up gate. The owner subsequently reported that the sensor fan was not running; this is an owner observation, not a supply measurement. Sensor wiring/power and valid PMS5003T measurements remain unverified; zero parser errors does not establish that bytes reached the UART. The [wiring guide](pms5003t.md#silent-fan--no-frames) records the next checks.

Capture: ignored `artifacts/waveshare-v2-pms5003t-sdprobe-20260928.log`, **705 bytes**, SHA-256 `02ef690bf8c65c37df83ded11ec0801624249e57c3b55b7e58f1b3f699a1241e`.

No TF file was created, deleted or formatted. This diagnostic does not measure Parquet logging, storage durability, enabled PSRAM, BLE/LAN sync or modem operation.

## TF card inserted: mount and capacity, 2026-09-28

After the owner inserted a TF card, the same diagnostic image was restarted with `pixi run capture --port /dev/cu.wchusbserial5B901533371 --reset --seconds 45 --out <trial-artifacts-log>`. It returned `AQ TF pins_ok=1 mounted=1 card_type=3 size_bytes=31457280000`: the V2 one-bit SDMMC mapping mounts this card and reads its reported capacity (**31,457,280,000 bytes**). No file was created, deleted or formatted; file read/write performance and durability remain untested.

The four PMS reports still showed zero frames and null values, including after warm-up. Inserting the card did not resolve the sensor power/wiring issue. No slider position or sensor supply voltage was measured during this capture.

Capture: ignored `artifacts/waveshare-v2-sd-inserted-20260928.log`, **588 bytes**, SHA-256 `d27a2a949fbf4ac734b45419211f574eb220345c04fd5cabe53fb77960676bbe`.

## PMS5003T reception after cable replacement, 2026-09-28

The owner replaced the cable and reported the fan running. Without changing the deployed diagnostic image, another bounded 45-second capture with `--reset` returned:

- TF mounted again, with the same reported **31,457,280,000-byte** capacity.
- PMS frame counters **11, 22, 33, 45**, all fresh, with **zero checksum and length errors** and sensor error code zero.
- The first two reports correctly withheld values during the warm-up gate. At 30/40 seconds, PM1/PM2.5/PM10 were **42/70/78** and **43/66/76 µg/m³**, temperature **23.6 °C**, and RH **47.2/47.3%**. Frame ages were 901/139 ms for those two snapshots.

This verifies valid PMS5003T UART reception on GPIO1 and the model-specific PM/temperature/RH decoding. It does not establish environmental accuracy, calibration, sensor identity/serial number, hardware UTC, sensor RX command operation, storage write/read durability, or Waveshare Parquet/BLE/LAN integration. The supplied physical wiring and cable replacement were owner observations; no rail voltage was measured.

Capture: ignored `artifacts/waveshare-v2-pms-working-20260928.log`, **668 bytes**, SHA-256 `4d40b18462b57afe84a0e9a164d4af96d7befd393ee48c2452bdbafb87883135`. The diagnostic binary/ELF identities remain the ones recorded above; later shared-module builds were not flashed during this run.

## Both boards attached, continued reception, 2026-09-28

A bounded 35-second capture without requesting a reset received four fresh, warmed reports with cumulative frame counts **1,035 / 1,046 / 1,057 / 1,069**, zero checksum/length/sensor errors, PM2.5 **70 / 71 / 73 / 70 µg/m³**, temperature **23.5–23.6 °C** and RH **43.9–44.6%**. CoreS3 was attached on a separate identified port during the capture. No TF mount was reprobed and no diagnostic firmware changed in this check.

Capture: ignored `artifacts/waveshare-v2-dual-connected-20260928.log`, **544 bytes**, SHA-256 `aaa0ece2d8aacd160b76c9660185b1c8cf0c5d1f78c1baff61c94ec7b896622f`. This adds continued UART reception evidence, not calibration or cross-sensor agreement.

## OPI PSRAM qualification

**2026-09-28.** The ESP32-S3R8/revision-0.2 chip identity selects 8 MiB OPI PSRAM with 16 MiB quad flash. A separate pinned-Arduino probe (`tools/fixtures/waveshare_psram_probe/`) booted with `qio_opi`, reported physical and heap PSRAM of 8,388,608 bytes, and tested four address-varying patterns over a 524,288-byte allocation. All patterns passed and the allocation was released. This checks initialization and that allocated region; it is not a destructive whole-memory or endurance test.

Probe BIN SHA-256 `56cb4bc284d20e6892216981b6b7871e6ba406e326d64c4fef0de261f5c0336d`; retained runtime log `artifacts/psram-probe-runtime.log`, 338 bytes, SHA-256 `ff486b9b38da75362b9ab6e05a44fa3a17fcdec6194f13ea9aa1f23f568456e4`. The actual compiler SDK header is `qio_opi/include/sdkconfig.h` with `CONFIG_SPIRAM_MODE_OCT`; Arduino's copied `build/sdkconfig` described QUAD and was misleading. `tools/check_arduino_target.py` now checks the actual compiler-selected SDK.

## Shared logger v1: card, measurements, clock and codec

**2026-09-28, V2.0, USB power, no battery, PMS5003T connected, original inserted TF card.** First logger BIN SHA-256 `c8de9791e6caa3b987c8743988b8e9e54bbbfa08f308830dd7c0ddee0489f75f`, ELF `b4a70a91e4fb1714a51323d44d69694a59eb8838e5c107bdadf10ae3e7a68b8d`, build 1,258,779 program bytes / 84,412 static RAM bytes. One upload was interrupted before verification while a capture was opened too soon; that incomplete application did not boot. Repeating the upload with exclusive port ownership completed and verified its hash. This was a host sequencing error; no button, DIP switch, partition erase or eFuse change was needed.

The trial uses AQCommon/AQRuntime/AQConnectivity/AQLogger with a board-owned 49-field `waveshare-sim7670g-telemetry-v1` dictionary (SHA-256 `e840c946c10378d28831b38f71daf66981d2ea99cf47ccba80fcd2e64f920ffd`). Device `a4cb8fd77500`, station `7abe1b6b-e014-42e7-b259-bf61cee9f288`, boot `b9f01bc95ba6bba3098e2137571ffd4b`. SDMMC one-bit uses CLK5/CMD4/D0=6; UART1 uses RX1/TX2 at 9600 8N1. No gauge/RTC/modem/GNSS/camera driver was initialized. RGB behavior was not visually inspected.

| Readback | Rows / bytes | CRC32 | SHA-256 |
| --- | --- | --- | --- |
| `data_unsynced_b9f01bc95ba6bba3098e2137571ffd4b_0-15-0.parquet` | 16 / 12,060 | `102bc7ff` | `af6a1edb994cda3f1690e8964ae7b71becb02d57637196f6c69449703e39aca1` |
| `data_0015_b9f01bc95ba6bba3098e2137571ffd4b_41-43-2.parquet` | 3 / 8,557 | `2787faa4` | `735e6110de4ca01e06879d01e21429a9d4370e6bf880277d4edbe767360412f6` |
| Codec duplicate: uncompressed sequences 44–46 | 3 / 8,579 | `28985c87` | `9d275f6feab6a1de57cd618215501f8b47df608de2bd6af72dad6d2f0c060798` |
| Codec duplicate: LZ4_RAW sequences 44–46 | 3 / 8,436 | `fb24f3d7` | `711616fdc148763856e7134f654144a1ca9c4dd8547a58de6235360317d06bcf` |

All four serial readbacks passed the helper's exact size/CRC/structure checks and PyArrow/DuckDB comparison. Codec copies had identical values and nulls and remain separately labeled diagnostic duplicates. On this three-row comparison, finalization was 64,120 µs uncompressed / 70,402 µs LZ4; LZ4 CPU 783 µs. This tiny file establishes codec interoperability, not a speed or endurance advantage.

- The first two rows were warm-up status 1 with particulate/temperature/RH null. Remaining rows had PMS status 4; sensor-reported ambient temperature 24.8 °C and RH 41.9–42.2%. Gauge status was 0 and all battery measurements null. Sensor accuracy, calibration and rail voltages were not measured.
- Before host time, all event UTC/anchor fields were null, `clock_status=0`, `clock_epoch=0`, and the file used the station's `unsynced/boot=…` path. Stored 16-row acquisition intervals were 9,999,000–10,001,000 µs (mean 10,000,000); jitter 326–1,326 µs. Drop/storage-error counters were zero.
- An explicit host UTC command anchored epoch seconds `1790555133` at monotonic `415489682` µs. The worker first finalized sequences 16–40 in the old unsynced tree, then wrote sequences 41–43 in the UTC day/window tree with `clock_status=1` and `clock_epoch=1`; anchor fields and nanosecond UTC annotations were present. Earlier rows were not rewritten.
- BLE scan found `AQ-7500` and decoded no-UTC/new-file/card flags. A connection negotiated a 517-byte ATT MTU (Bleak reported usable 515), but the first encrypted INFO read timed out without a completed Mac bond. Fixed-PIN callback handling was corrected and deployed: serial then reported `BLE PAIR passkey=fixed`; a subsequent attempt still ended with ATT insufficient encryption. No secure BLE file transfer is claimed. The CLI now honors a bounded overall GATT-read deadline while waiting for pairing.
- Wi-Fi was left unconfigured. Automatic approval review rejected copying the CoreS3 SSID/PSK to this device because that credential export lacked specific authorization. The copy was not performed. LAN file transfer remains unmeasured until owner-approved provisioning. The shared service remains implemented and the CoreS3 LAN result is separate evidence.

Evidence is retained in `firmware/arduino-waveshare-sim7670g/artifacts/logger-v1/`: firmware/ELF images, upload logs, serial readbacks, BLE scans/attempts and codec files. No fixed PIN or Wi-Fi password was retained. `codec-test.log` is 1,177 bytes with SHA-256 `9d9d368422cb7edaff09bafc18bd248beb23ce4ddae55f320905d21ef639d6e4`. Each RAM batch was explicitly flushed before subsequent firmware writes. Multi-group automatic rotation, interrupted-partial recovery, card endurance, radio-load timing and power-cut durability are not established by these bounded runs.

### Final shared-engine image, reset and retained files

After fixing the shared fixed-PIN callback and MTU guard, an intermediate image (`50d3811e913e80efa0f18d9627b082edb3d44da88206bedc57d6ce0f4b02f798`) booted as `88e137231bf2d3dd7811c22d8075d173`. Its 14-row null-UTC file passed both readers (11,476 bytes, CRC32 `b0257d27`, SHA-256 `d440c4ea52bffea5ae2d8f59dc97fdca80722858ffe0af6bb57d35f537614386`). Re-fetching the original 16-row file after these resets produced the unchanged `af6a1edb…e39aca1` hash, establishing retention and immutable bytes across normal firmware resets. This does not establish power-cut durability.

The CoreS3 no-reader check subsequently identified the common startup filename-dump problem. The final Waveshare v1 image also contains the bounded startup scan, with BIN SHA-256 `241862228dd9f6a31fc2f9910dcf670b3c966b0ba10463567aa6eb6d131cb8ae`, ELF `984b8b494a107eb6bdd639689267c7a0288a92593f4f2f3148dd9f4a56a7a3df`; build 1,259,027 program bytes / 84,412 static RAM bytes. Every prior RAM batch was flushed first.

Final boot `5bd8d6e02d728897b07c4b462dfc1bc9` preserved device/station identity and again started without an RTC/UTC anchor. `data_unsynced_5bd8d6e02d728897b07c4b462dfc1bc9_0-15-0.parquet` contains 16 rows / 49 columns, 12,060 bytes, SHA-256 `2bfa7f3939845e4d3cccfd15302ae80de7e8ce3af334d2d0e8e49597e9ab5649`, and passed CRC/structure/PyArrow/DuckDB checks. Sequence 0–15 is complete; all drop/error/missed-deadline counters are zero. Warm-up statuses are 1,1 then valid 4; battery/gauge behavior remains null/0. UTC and anchors stayed null until an explicit host time command. Jitter was 605–1,605 µs; intervals 9,999,000–10,001,000 µs.

The board was then explicitly anchored from the host at Unix seconds `1790556502`, monotonic `166340683` µs, and left running the final logger for continued recording. Firmware reset will require another host anchor. First secure Mac pairing and Wi-Fi/LAN remain uncompleted; the security requirement was not lowered to obtain a test. No battery, modem, GNSS or camera was enabled.


## Android installation and erased-bond diagnosis (2026-09-29)

OnePlus 7 Pro (GM1911), Android 16 / API 36, USB ADB; Android app commit
`06ab6ccd97869ddd40b48e858ba081b254fa2ef3`, debug APK SHA-256
`5d135ca063ce1a4c326d14fa52e032cc68c5e74408210601d5e8b2eb420cf524`.
Updated the existing app with the matching debug signing certificate and `adb install -r`,
without uninstalling or clearing data. Home opened, no AndroidRuntime crash was observed,
and existing archive summaries remained visible (AQ-7500: 99 files; AQ-6b40: 1,151).
This does not establish a controlled Android schema-migration path or a new file transfer.

The phone's automatic AQ-7500 BLE reconnect reached connected state and then disconnected
with GATT status 5 before service discovery. Android Bluetooth diagnostics recorded
`HCI_ERR_AUTH_FAILURE` with `encryption_change:key_missing` both before and after this app
update. The owner confirmed this board had been reflashed/erased after pairing with the phone.
The app initially masked that callback with a generic service-discovery error and required
Ready before opening the device screen; those UI/diagnostic defects are being corrected in
this app, rather than weakening BLE authentication or silently deleting bonds.

The identified WCH console `/dev/cu.wchusbserial5B901533371` answered a read-only
`parquet status` without a reset: station `81c0da75-009b-4922-94f9-64e310f006b8`,
interval 900 s, buffered 22, finalized 5, dropped 0, errors 0, failed false,
`waveshare-sim7670g-telemetry-v1`. The current firmware-image SHA-256 was **not measured**;
prior image identities above must not be assumed for this erased/reflashed deployment.
No image, clock, settings, PIN or bond was changed or saved during this diagnosis.
Foreground Android pairing repair and authenticated collection after it remain unverified.

## Battery gauge adapter readback (2026-09-30 Cairo; 2026-09-29 UTC)

The owner installed a correctly oriented 18650, observed the reverse-battery LED off, and reported that the board stayed on after USB was unplugged. Those are owner observations, not a measured battery rail or capacity test. For the following readback the V2.0 board was attached to USB at checked WCH port `/dev/cu.wchusbserial5B901533371` with the battery installed. The prior verified 16,777,216-byte backup `backup/waveshare-sim7670g-v2-flash-20260927T222602Z.bin` was present. Each running logger was flushed before reflashing so completed rows remained on the TF card. Flash writes verified their hashes; no eFuses or TF files were erased.

The initial adapter image, SHA-256 `6429adfb1b42ace7a71e6e3a53dc5b60becbb7332f62d59a214d830c26b88d59`, booted and wrote two rows, but both had `gauge_status=1` and null battery fields. A bounded diagnostic image, SHA-256 `5c8672b0bd501871d1192d5679261d6f1a429aeba4474d4204353ce635fe8f84`, then showed successful MAX17048 VCELL/SOC I²C transactions on SDA15/SCL16: raw VCELL 54,704–54,720 (about 4.274–4.275 V) and raw SOC 27,593 (107.8%). The first adapter had rejected the above-100% estimate. This is a gauge estimate while USB was attached, not a charger-state or percentage-accuracy measurement. The corrected adapter truncates whole percent, caps reported values at 100, and rejects raw SOC above 120% as a project plausibility heuristic.

Final logger image SHA-256 `63951c913b0995ee7d2180a68fb4fc995202b48cfe6741655909e2b420febc0c` (retained in trial `artifacts/`) booted and the serial logger status reported no drop/error/failure. Its serially fetched unsynced file `data_unsynced_e0a27366d5d4fdf895042322232bf1d2_0-2-0.parquet` was 8,480 bytes, CRC-32 `7f4fefd4`, SHA-256 `bb6400653823db0d64ea611ee529f66c7cd929e34e784e4fb93a0af55289790d`, three rows in one row group, and passed the helper's size/CRC/structure and PyArrow/DuckDB comparison. All three rows had `gauge_status=2`, `battery_mv=4275`, `battery_percent=100`, with zero dropped rows, missed deadlines and storage errors. Adjacent sampling intervals were 10,000,000 and 10,001,000 microseconds. The raw SOC was 27,755 (108.4%) in the observed final serial samples; the stored 100% is a capped display estimate. The file and all three image copies live in the trial's git-ignored `artifacts/`, outside `build/`.

This short USB-attached Parquet run does not test battery-only gauge readings, charging detection, percentage calibration or endurance. The existing shared live serializer maps valid row fields to `bat`/`pct`; the phone observation below separately verifies their display. The V2 schematic provides no verified ESP32 charger-status input, so `chg`/`vbus` remain absent rather than inferred from voltage.

### Android live battery display (2026-09-30 Cairo)

OnePlus 7 Pro (GM1911), Android 16, AQ app installed APK SHA-256 `105783e2c2655fea0a9c83cff0fcf577aee3fab17f7d7937899a103f2c26c9b7` (local companion commit `5df7aa86171fc72e94a0bd72bfda75cb2f3fc056`) was connected to AQ-7500 over Bluetooth while the final battery-gauge image above was running. ADB opened the existing app without reinstalling, found **Simple** selected, and switched the reversible display preference to **Detailed**. On Today, the battery card showed **100%** and **4,277 mV** while current PM/ambient readings continued updating. The screenshot is retained at `firmware/arduino-waveshare-sim7670g/artifacts/battery-android-live-20260930.png` (310,269 bytes; SHA-256 `8080782f9cc55761599a8dd92354a05c93c43ec7c972f66bc88734fdb61bc988`). No Android source or APK change was required for `bat`/`pct`; the companion parser and card already accepted them. This is a visible app readout over the connected link, not a captured GATT packet, battery-only test, capacity calibration, charging-state validation or file-transfer test. The app showed no charging or USB value, as those fields are not produced by this board adapter.

The owner then disconnected **the Waveshare board's USB cable** while leaving its battery switch on; the phone stayed USB-connected for ADB observation. AQ-7500 remained Bluetooth-connected and the Android Battery card subsequently showed **100%** and **4,067 mV**, down from 4,277 mV with board USB attached. Particle/ambient readings changed and the device status age was 4 seconds at capture, with uptime 12 minutes 50 seconds. This verifies a fresh battery-only voltage and SOC display over the live phone link; it does not calibrate the 100% estimate or measure load current, battery capacity, run time or charging state. The battery-only screenshot is retained at `firmware/arduino-waveshare-sim7670g/artifacts/battery-android-live-battery-only-20260930.png` (322,123 bytes; SHA-256 `70250dffca9867bc284612c9e0a89a6d47e1569a789bd75810856b84a102d78f`).
