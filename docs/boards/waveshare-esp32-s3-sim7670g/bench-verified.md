# Waveshare V2 bench record

**Framework scope, 2026-10-09:** Current source is native ESP-IDF 6.1 and its
build/host gates pass. No native hardware run is claimed here. The dated
physical observations, images, old API/tool commands and performance figures
below belong to historical Arduino builds; retain those identities when using
this evidence. Native qualification must add a separately dated image/readback.


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

Evidence is retained in `firmware/esp-idf-waveshare-sim7670g/artifacts/logger-v1/`: firmware/ELF images, upload logs, serial readbacks, BLE scans/attempts and codec files. No fixed PIN or Wi-Fi password was retained. `codec-test.log` is 1,177 bytes with SHA-256 `9d9d368422cb7edaff09bafc18bd248beb23ce4ddae55f320905d21ef639d6e4`. Each RAM batch was explicitly flushed before subsequent firmware writes. Multi-group automatic rotation, interrupted-partial recovery, card endurance, radio-load timing and power-cut durability are not established by these bounded runs.

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

OnePlus 7 Pro (GM1911), Android 16, AQ app installed APK SHA-256 `105783e2c2655fea0a9c83cff0fcf577aee3fab17f7d7937899a103f2c26c9b7` (local companion commit `5df7aa86171fc72e94a0bd72bfda75cb2f3fc056`) was connected to AQ-7500 over Bluetooth while the final battery-gauge image above was running. ADB opened the existing app without reinstalling, found **Simple** selected, and switched the reversible display preference to **Detailed**. On Today, the battery card showed **100%** and **4,277 mV** while current PM/ambient readings continued updating. The screenshot is retained at `firmware/esp-idf-waveshare-sim7670g/artifacts/battery-android-live-20260930.png` (310,269 bytes; SHA-256 `8080782f9cc55761599a8dd92354a05c93c43ec7c972f66bc88734fdb61bc988`). No Android source or APK change was required for `bat`/`pct`; the companion parser and card already accepted them. This is a visible app readout over the connected link, not a captured GATT packet, battery-only test, capacity calibration, charging-state validation or file-transfer test. The app showed no charging or USB value, as those fields are not produced by this board adapter.

The owner then disconnected **the Waveshare board's USB cable** while leaving its battery switch on; the phone stayed USB-connected for ADB observation. AQ-7500 remained Bluetooth-connected and the Android Battery card subsequently showed **100%** and **4,067 mV**, down from 4,277 mV with board USB attached. Particle/ambient readings changed and the device status age was 4 seconds at capture, with uptime 12 minutes 50 seconds. This verifies a fresh battery-only voltage and SOC display over the live phone link; it does not calibrate the 100% estimate or measure load current, battery capacity, run time or charging state. The battery-only screenshot is retained at `firmware/esp-idf-waveshare-sim7670g/artifacts/battery-android-live-battery-only-20260930.png` (322,123 bytes; SHA-256 `70250dffca9867bc284612c9e0a89a6d47e1569a789bd75810856b84a102d78f`).

### Android History after station reset (2026-09-30 Cairo)

OnePlus 7 Pro (GM1911, Android 16 / API 36), Android app commit `dba3e69`, debug APK SHA-256 `9c14cec1db961f842f570f99095db414558f19cd0247e62f01678564001be051`, installed in place with `adb install -r`. The phone archive and Bluetooth enrollment remained. The board was battery powered and connected as AQ-7500 over BLE; its firmware was previously read as image SHA-256 `63951c913b0995ee7d2180a68fb4fc995202b48cfe6741655909e2b420febc0c` but was not read again for this phone run.

History displayed two retained station generations after the earlier reflash/NVS reset: current `81c0da75-009b-4922-94f9-64e310f006b8` and earlier `7abe1b6b-e014-42e7-b259-bf61cee9f288`. The current station's 30 September PM2.5 Day chart showed 23.3 µg/m³ at 2% coverage. Selecting Earlier opened its latest local day, 29 September, and showed 36.5 µg/m³ at 18% coverage; switching to Week retained that date and showed the 23–29 September chart. The two short generation chips and one-line horizontally scrollable metric chips fitted the phone screen. Screenshots are retained in ignored trial artifacts: `history-android-current-20260930.png` (SHA-256 `1832c216861e926198ba91aec625018439368b66d195b466f06887ffff9cd6c7`) and `history-android-earlier-20260930.png` (SHA-256 `0e80bc7ea09de10c87b146bb45120290e74d5df10484a2c8fa60f89d7b105634`).

Before the app fix, History presented 99 retained SD copies with the earlier Hive producer station under the current INFO-station archive as “still indexing” and described them all as untimed. The fixed page excludes those copies from the current station's timeline counts and explains that they remain unchanged in the archive. After an automatic collection of four more files, the current namespace held 20 own files (three with real UTC, two estimated) plus 99 retained earlier-producer copies. This is a phone UI/history observation, not a controlled reset/reflash, full sync, firmware time-setting, or station-provenance migration test. Android `./gradlew --no-daemon verify` passed for the fix; file bytes and outer archive namespaces were not changed.

## Shared archive payload fix on Waveshare (2026-09-30 UTC)

The checked WCH port `/dev/cu.wchusbserial5B901533371` identified the same
ESP32-S3 revision 0.2 / MAC `a4:cb:8f:d7:75:00`: 16 MB quad flash, 8 MB
embedded PSRAM, secure boot disabled and flash encryption disabled. The prior
full 16,777,216-byte backup
`backup/waveshare-sim7670g-v2-flash-20260927T222602Z.bin` was rechecked at
SHA-256 `d88ddcd6bbbc69972396df9a1d67d75b006f5058d000e07cd850717fffd07a45`.
An attempted fresh full read was stopped because default-speed readback takes
about 25 minutes on this CH343 link; no new backup was published. The earlier
verified full image remained available before the write.

The running logger first finalized its buffered nine rows to the SD card.
`pixi run waveshare-contract-test --sanitize`, the shared archive sanitizer,
format/lint gates and both board builds passed. The logger build used
1,283,515 program bytes and 85,804 static RAM bytes. The retained application
image is `artifacts/firmware/waveshare-archive-mtu-ce220d12.bin` (1,283,664
bytes; SHA-256
`ce220d128f4cedff3a55e3ec18f23f0c6427af59873dc08eff13d88d14fe9707`).
`waveshare-flash` verified the written application hash and reset the board;
it did not format the TF card or write eFuses. The first read-only status after
boot reported the same station, two buffered rows, zero drops/errors, and
`failed=false`.

The pre-flash file
`data_unsynced_7368afc74a14c8e944ce810b3b29d8c2_0-8-0.parquet` remained
fetchable after the flash: 9 rows, 49 columns, 9,432 bytes, CRC-32 `7ee9aaa3`,
SHA-256 `a26d382e374a51490084bbd8416269c7ca3c377d70202ad19a32a377c84b2bf0`.
The serial helper verified size, CRC and Parquet structure; PyArrow and DuckDB
agreed. Its sampling intervals were 9,999,000–10,001,000 µs, with no missing
sequence; PM and ambient fields were null in all nine rows. This establishes
normal-reset SD retention and serial readback on this image. The low-MTU
LIST/OPEN error behavior has a host sanitizer test; this serial readback phase
did not negotiate a small BLE payload or complete a phone file transfer.

The new image also finalized
`data_unsynced_d0c6a561200d839339e76198a17faa7c_0-68-0.parquet`:
69 rows in one row group, 22,131 bytes, CRC-32 `9d3a871c`, SHA-256
`abb028ceff1ad338b8e3ce895de3c771dbbd061354619367e23da0274e1af9aa`.
Serial fetch verified its size, CRC and `PAR1` structure, and PyArrow/DuckDB
agreed. Sequence 0–68 was contiguous; acquisition intervals were
9,999,000–10,001,000 µs, mean 10,000,015 µs. The following status showed one
finalized file, zero dropped rows/errors and `failed=false`. All 69 PM and
ambient fields were null in this run; sensor attachment or cause was not
diagnosed here. UTC remained unanchored.

### Foreground Android sync after the shared-engine flash (2026-09-30 Cairo)

The connected OnePlus 7 Pro (GM1911, Android 16 / API 36) installed Android
app commit `d1f91e4` in place; debug APK SHA-256
`4f00537c996d77a39bea9a09f322b62309d1e955a3868ba05d3dfbb46206de7f`.
The Waveshare was still running the image flashed above; no further flash or
eFuse operation was performed. The app connected to AQ-7500 over BLE, read
station `81c0da75-009b-4922-94f9-64e310f006b8`, and Today showed zero
dropped rows and storage errors. The owner-facing **Finalize buffered rows**
action was confirmed once, making a new short Parquet file available without
changing the rotation interval.

Files then showed 150 finalized files with one pending transfer. Foreground
**Sync all** completed **1/1 file, 16.8 KiB displayed** over Bluetooth, and
the app reported that the card's listing was archived and verified. The phone
archive count rose from 1,399 to 1,400. An explicit LIST refresh updated the
listing age; History retained its current-station PM2.5 day chart after tab
navigation and app background/resume. This verifies one foreground BLE file
transfer against the reflashed board and one lifecycle path. The file's exact
byte count and digest were not separately measured on the phone; the app's
verified indicator is its size/CRC/PAR1 check. Full archive, low-MTU phone,
LAN, screen-off and multi-device transfers remain unmeasured here. The board
still had no UTC anchor, so new rows stayed untimed.


## 2026-10-11 — main 7384e03 native logger USB reflash and readback

Waveshare V2.0 MAC `a4:cb:8f:d7:75:00`, checked CH343 port
`/dev/cu.wchusbserial5B901533371`. In order, ports/chip/flash-ID/eFuse checks
confirmed ESP32-S3 rev0.2, 8 MB embedded PSRAM, 16 MB quad flash and an
available download path with secure boot and flash encryption disabled. The
existing board backup `backup/waveshare-sim7670g-v2-flash-20260927T222602Z.bin`
was reverified at 16,777,216 bytes and SHA-256
`d88ddcd6bbbc69972396df9a1d67d75b006f5058d000e07cd850717fffd07a45`.
A fresh default-speed CH343 backup was not taken; the recorded full read takes
about 25 minutes.

Pulled `main` at `7384e03`, regenerated the disposable build sdkconfig from
current defaults after the earlier generated FAT lock limit failed compilation,
and built the native logger through `pixi run waveshare-build`. The checked
v2.5 app BIN SHA-256 is
`e8bb3a1cd3b6f74fdf207f274f9053d90d0f28f755d504ae4b5545b01bc1a193`.
Buffered samples were flushed before the backup-gated flash wrapper; esptool
verified all written regions. NVS was outside the flash plan, no card format or
archive erase was performed, and the observed station ID stayed unchanged.
Images, configuration, build/flash logs, status and reader evidence are retained
in ignored `firmware/esp-idf-waveshare-sim7670g/artifacts/reflash-20261011/`.
Only the generated dependency-lock manifest hash was normalized by the build;
pinned dependency versions/source identities were unchanged, and the tracked
lock was restored after retaining the generated copy in artifacts.

A manual-flush USB export contained five rows, 56 columns and one row group,
10,735 bytes, CRC32 `058e4136`, SHA-256
`f20df0d7cb272740960d2469686b271c8479559d79b83fdee024f9cdfde649a1`.
Transfer integrity passed and PyArrow/DuckDB agreed. Sequence 0–4 was complete,
with exactly 10,000,000 us between acquisition timestamps. Drops, storage errors
and missed deadlines were zero. PMS warm-up nulls preceded checksum-valid
frames; gauge status was valid, at 4,275–4,276 mV and capped 100%. Ambient
readings of 0.4–1.0 C and RH 0–0.1% were implausible for the bench and are
recorded as a measurement limitation, not qualified environmental values.
Stored clock status was host estimate (1); the bench helper did not set time,
the external anchor source was not investigated. This short native
logging/USB readback does not qualify environmental accuracy, battery accuracy,
phone reconnect/transfer, BLE/LAN load, full-window rotation, endurance or
power-cut durability. Both board builds, format/lint and 18 additional relevant
host gates passed before flashing; gate logs are retained with CoreS3 artifacts.


## iPad bonded reconnect, controller assert and link sequencing (2026-10-12)

Symptom. With phone Wi-Fi off, the iPad synced 1391 files over BLE, then the
link dropped. The passive capture of the next reconnect showed
`BLE BONDED authenticated=true bonds=3`, then
`BLE assert lld_con.c 3275, param 00000002 00000000` and
`Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on CPU0)`, and
the next boot reported `boot_seq=3 reset=5`. The backtrace, symbolized with the
matching `artifacts/reflash-20261011/aq_waveshare.elf`, stays inside the closed
ESP32-S3 controller (`r_llc_loc_con_upd_proc_continue_hack`,
`ll_connection_update_ind_handler_hack`, `r_lld_llcp_rx_ind_handler_hack`,
`rw_schedule`, `btdm_rw_run`). The controller asserted when the central's
connection update arrived while a local link procedure was still open. At
connect, before encryption, the firmware requested data length, 2M PHY and an
active connection interval together.

Fix (shared `ble_sync.cpp`, both boards). Connect requests only the data length
change, which has no instant. After encryption succeeds the firmware waits 2 s,
requests 2M PHY, and waits for PHY completion or a 2 s fallback. Only then does
it request the active interval, and only when the central has not already chosen
one in range. A refused request is retried after 1 s, and the idle relax step
waits until the sequence is done. `docs/shared/ble-sync-protocol.md` describes
the new order. Frame layouts, opcodes and UUIDs are unchanged. Host format,
lint, BLE policy tests and both board builds passed.

Buffered rows were flushed with `parquet flush`, then the image was flashed with
the backup gate against `backup/waveshare-sim7670g-v2-flash-20260927T222602Z.bin`.
esptool verified all written regions and NVS was kept. In the first minutes of
the after-fix capture the iPad reconnected and bonded
(`BLE CONNECT`, `BLE BONDED authenticated=true bonds=3`, `BLE MTU mtu=517`,
host time accepted) with no assert or panic. The full 1800 s capture then
showed that single connection held with no disconnect, no assert, no panic and
no reconnect loop, while logging continued (two Parquet files of 27 and 68 rows
became ready, row 179 at 1800 s, `dropped=0`). This is one reconnect and one
idle bonded link, so repeated app reopen cycles and a full iPad archive
transfer on the new image are not established by this entry. The CoreS3 has
not received this image yet. Logs are in ignored
`firmware/esp-idf-waveshare-sim7670g/artifacts/ble-ipad-20261012/`.

## Integration branch reflash, archive reset and schema v4 readback (2026-10-16)

Image `idf-waveshare-parquet-v2.6`, app version 285cf22 on branch
`integration/waveshare-bench` (merges of #22, #20 and #21). Board ESP32-S3 MAC
a4:cb:8f:d7:75:00, 16 MB flash, 8 MB PSRAM, no security eFuses. Full flash
backup `waveshare-sim7670g-v2-flash-20261016T114219Z.bin`, 16,777,216 bytes,
SHA-256 `c55e75174914270dac199f4634ba6e6e3b6206c83a6171d446a654541b62612b`.
Flashed with `pixi run waveshare-flash`, NVS untouched, hash verified.

| Check | Result |
| :--- | :--- |
| Boot | `PARQUET WATCHDOG mode=armed timeout_s=60 consecutive=0 max_consecutive=3 healthy_s=3600 supervised=1`, `PARQUET BEGIN schema=waveshare-sim7670g-telemetry-v4 columns=56 psram_workspace_bytes=162520` |
| Archive reset (owner requested) | `parquet erase-archive CONFIRM` returned `ok=1 files=1589 directories=71 reboot_required=1`. After reboot `PARQUET SCAN files=0`. Card not formatted |
| Readback | Host time sync, 4 rows, manual flush, serial fetch. File `data_1215_07f07ae4..._1-4-1.parquet`, 10,782 bytes, SHA-256 `c3a1e7f76b41fdde1aa7142883c6ce3565e566dce3932311a982b2fed359a8c2`, CRC-32 `768fa99e`. Cadence deviation at most 1,007 us |
| Schema | PyArrow 25 reads `event_time_utc_us` and `clock_anchor_utc_us` as `timestamp[us, tz=UTC]`. All 56 leaves carry field ids 1 to 56 (DuckDB `parquet_schema`), for example `sequence`=5, `event_time_utc_us`=9, `clock_anchor_utc_us`=52. DuckDB reads `TIMESTAMP WITH TIME ZONE`, values 2026-10-16 12:17:47 UTC onward, matching host UTC |
| Not verified | LIVE keys over BLE (macOS pairing needs the owner to enter the device passkey, the attempt ended `BLE AUTH result=failed`, logging continued). Forced worker stall and watchdog reset not exercised |

## Main 8344adc bench for the watchdog, remount and battery health merges (2026-10-17)

Image `idf-waveshare-parquet-v2.7`, app version 8344adc on `main` (contains
#21, #41 and #24). Board ESP32-S3 QFN56 rev v0.2, MAC a4:cb:8f:d7:75:00, flash
manufacturer 85 device 2018 16 MB, 8 MB AP_3v3 PSRAM, WCH bridge 1A86:55D3.
`pixi run ports`, `chip`, `flash-id` and `efuse` ran before each of the three
flashes with the same result, `SECURE_BOOT_EN=False`, `SPI_BOOT_CRYPT_CNT=0`,
`DIS_DOWNLOAD_MODE=False`. The owner waived a new flash backup, so the flash
gate reused `waveshare-sim7670g-v2-flash-20261016T114219Z.bin` from the
2026-10-16 entry. Every flash used `pixi run waveshare-flash`, wrote only
bootloader, partition table, otadata and app, kept NVS, and verified every
region hash. No eFuse write, card format, NVS erase or full chip erase.

Part of [#27](https://github.com/walkthru-earth/aq-parquet/issues/27), with
evidence for [#18](https://github.com/walkthru-earth/aq-parquet/issues/18),
[#28](https://github.com/walkthru-earth/aq-parquet/issues/28) and
[#29](https://github.com/walkthru-earth/aq-parquet/issues/29).

| Image | App SHA-256 | Use |
| :--- | :--- | :--- |
| main 8344adc | `c00749278dcfb8987f46918a142872f146dabe48eb62fb68aa4cd80cfd0c4ce3` | Boot checks |
| Temporary stall build, app version `8344adc-dirty` (8344adc plus a bench patch) | `d83a8fc06c23400fb147f1f3959623e59eabaa0dcc9ae15faced57d89e10e2c4` | Watchdog test only, then replaced |
| main 8344adc, final | `9b7df8ef78d821120bdd1501b5246783337cd05beb566f4cc255e89e36ca92d9` | Left on the board. Same source as the first row, the hash differs only by the embedded build time |

| Check | Result |
| :--- | :--- |
| Boot | `AQ BOARD waveshare-sim7670g-v2 firmware=idf-waveshare-parquet-v2.7 ... battery=installed`, `AQ MEMORY psram_physical_bytes=8388608 psram_heap_bytes=8321872 psram_ok=1`, `PARQUET WATCHDOG mode=armed timeout_s=60 consecutive=0 max_consecutive=3 healthy_s=3600 supervised=1`, `PARQUET BEGIN schema=waveshare-sim7670g-telemetry-v5 columns=62 ... psram_workspace_bytes=162520 row_bytes=880`. `parquet schema` printed `sha256=67688d0fd2e86053c1c66b961a1446ad3c225ee71823e0fadb771b645c7183ff`. Build sdkconfig has `CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`, TWDT panic off at boot, both idle tasks checked, interrupt watchdog 800 ms. TF mounted at 31,457,280,000 bytes and rows landed on the card |
| Core dump on main | **Not in the main image.** Both board projects use `set(COMPONENTS ...)`, and the logger does not require `espcoredump`, so the component is not built. The build prints `unknown kconfig symbol 'ESP_COREDUMP_ENABLE_TO_FLASH'`, the generated sdkconfig has no core dump entry and the ELF has no `esp_core_dump` symbols. `firmware/common/logger/README.md` says the shared defaults enable the flash core dump. That holds only once `espcoredump` is in the build. The CoreS3 project uses the same `set(COMPONENTS ...)` pattern and needs the same check |
| Watchdog, temporary build | The bench build added `espcoredump` to the logger `PRIV_REQUIRES` and a serial `parquet bench-stall <s>` command that blocks the worker (0 means forever). A forever stall logged `worker-stall seconds=5`, 35 and 65 with `watchdog=armed`, then `task_wdt:  - parquet-sd (CPU 0/1)` about 66 s after the stall began, `Core dump used 1520 bytes on stack. 368 bytes left free` and `rst:0xc (RTC_SW_CPU_RST)` with logged `reset=6` (task watchdog). The next boot printed `Core dump data checksum is correct`, `Found core dump 24992 bytes in flash @ 0xc10000` and `PARQUET ERROR operation=worker-watchdog-reset consecutive=1` |
| Core dump size | 24,992 bytes, 38% of the 64 KiB `coredump` partition, 14 threads, with Wi-Fi unprovisioned and the modem idle. `esp_coredump info_corefile` decoded the panic reason naming `parquet-sd`, with the worker in `vTaskDelay(1000)` and 4256 of 20304 stack bytes used. The 368 byte free margin on the dump stack is small |
| Recovery | A 64 s returning stall logged `PARQUET WATCHDOG mode=armed consecutive=1 reason=recovered` with no reset |
| Boot loop guard | Two more forever stalls reset with `consecutive=2` and then `consecutive=3`, after which boot printed `PARQUET WATCHDOG mode=warn ... consecutive=3`. A 90 s stall in warn mode logged `watchdog=warn` warnings and no reset. `parquet status` then reported `dropped=1 queue_peak=8`, so a stall longer than about 80 s overflows the row queue by design |
| Restore | The temporary patch was reverted and the final main image flashed. Its power on boot showed `mode=armed consecutive=0`. The `coredump` partition still holds the dump from the temporary build, which main cannot read |
| Battery, USB with an 18650 installed | In the final run the first serial gauge read is `vcell_raw=54736` (4276 mV), `soc_raw=28524`, `crate_raw=0`, `status=0x00ff`, `mode=0x1000`. `POWER STATE state=unknown mv=4276` then, with `vcell_raw=54864`, `external_power_likely mv=4286` about 20 s later. In the Parquet file `gauge_status=2` (valid), `battery_mv` 4271 to 4286, `battery_percent=100`, `battery_soc_raw_percent=111.42` (uncapped), `battery_charge_rate_percent_per_h=0`, `gauge_hibernating=1`, `battery_health_state` 0 for two rows then 1 (external power likely), `battery_minutes_to_empty` null. The cell was not discharged and the external rail was not measured, so the 4240 mV threshold stays uncalibrated for #28 |
| 20 minute run | The final image logged 131 rows over 1311 s with `dropped=0`, `errors=0` and `queue_peak=0`. One file was ready at 902 s (`rows=89 bytes=38225 crc32=330b5098`). `parquet flush` closed the remaining 42 rows into a second file. Neither BLE pairing nor watchdog events occurred. An earlier 321 s run was cut short for the owner's BLE PIN change and is kept as a partial log. Its 32 buffered rows were lost on the reset because the CH343 port cannot be reopened without resetting the board |
| Readback | `parquet-device fetch` of `data_unsynced_233c0e2d..._0-88-0.parquet`, 38,225 bytes, SHA-256 `5058c8b9fc647547ce2ba9f13cf52e2643303228a6e3f20c669a9a10e00e0294`, CRC-32 `330b5098`, `readers_match=true`, cadence deviation at most 1,015 us. PyArrow 25 reads 62 leaves with field ids 1 to 62 in order. `event_time_utc_us` (id 9) and `clock_anchor_utc_us` (id 52) are `timestamp[us, tz=UTC]` with Parquet logical type `Timestamp(isAdjustedToUTC=true, timeUnit=microseconds)`. Values are null because this boot had no host time sync. The battery columns are ids 33 to 35 and 57 to 62. After the fetch reset the board booted the same image and resumed rows |
| BLE dev PIN | At the owner's request `parquet config-hex` set `ble.pin=<dev pin>`. The board answered `CONFIG SET pair=fixed pin_default=false ... rotate_token=<redacted> reboot_required=true` and `AQ CONFIG ok=1 reboot_required=1`, and the board was rebooted so the PIN applies. `pin_default=false` shows the stored PIN is no longer the default. The line carries no PIN digits. The value is kept out of every log and this record |
| Not run | BLE or LAN `LIST`, `OPEN`, `READ`, `ABORT` and the remount recheck. The Mac had no bond. `ble-sync list` failed with `Insufficient Encryption`, and `ble-sync live` connected (MTU 515) and then timed out after 20 s on its first characteristic read. Pairing needs the owner. Wi-Fi is not provisioned, so LAN is unavailable. Also not run are the one hour healthy clear of the guard, a power pull or brownout with a pending stall, OTA, and battery discharge |

Logs, images, the raw dump, its decode and the readback are in ignored
`firmware/esp-idf-waveshare-sim7670g/artifacts/bench-20261017/`.

## Main 67f25dc USB install and owner archive reset (2026-10-18)

Board ESP32-S3 QFN56 rev v0.2, MAC a4:cb:8f:d7:75:00, flash manufacturer 85
device 2018 at 16 MB, 8 MB AP_3v3 PSRAM, WCH bridge 1A86:55D3 on
`/dev/cu.wchusbserial5B901533371`. The four `QualComm Compo` 05C6:9330 ports on
the same hub are the SIM7670G modem, not an ESP32 console. `pixi run ports`,
`chip`, `flash-id` and `efuse` showed `SECURE_BOOT_EN=False`,
`SPI_BOOT_CRYPT_CNT=0` and `DIS_DOWNLOAD_MODE=False`. The full backup
`backup/waveshare-sim7670g-v2-flash-20261018T072052Z.bin` is 16,777,216 bytes,
SHA-256 `45efeebc0147313d33de2f02a2324e75ff5bff218e07fc86df5fc9476057cc02`, read
in about 25 minutes at the default baud. Its partition table matches the current
build, with NVS at `0x9000` and `coredump` at `0xc10000`.

The board already ran app version `67f25dc`, compiled 2026-10-17 21:17. This
install replaced it with a clean `pixi run waveshare-build logger` of the same
commit, app BIN SHA-256
`fe123b93fff432f29b7b6e4a6704be3a3902d68748d744b5ab836820003807e0`.
`pixi run waveshare-flash` wrote only bootloader, partition table, otadata and
app0, and esptool verified every region hash. No eFuse write, NVS erase, full
chip erase or card format.

| Check | Before | After |
| :--- | :--- | :--- |
| Image | `idf-waveshare-parquet-v2.7`, app version `67f25dc`, compiled 2026-10-17 21:17 | `idf-waveshare-parquet-v2.7`, app version `67f25dc`, compiled 2026-10-18 10:15, schema v5, 62 columns |
| Protocol | Not read over radio | Proto 3 from `kProtocolVersion` in the flashed source. Not read over BLE or LAN in this run |
| Settings and NVS | `CONFIG LOADED first_boot=false pair=fixed pin_default=false wifi_on=false ssid_set=false lan_on=true` | Same line, same station `5272567d-356b-489d-9f60-c24812540bbf`. A bonded phone reconnected with `BLE BONDED authenticated=true bonds=3` |
| TF and archive | `AQ TF mounted=1 ... format_attempted=false`, `PARQUET SCAN files=36` | Mounted the same way, `PARQUET SCAN files=38`. The two extra files are normal rotations by the old image before the backup, one `PARQUET READY` is in the pre-flash log |
| Sensors | PMS5003T UART ready | PMS5003T and MAX17048 readings, see the readback row |
| Watchdog | `mode=armed consecutive=0` | `mode=armed consecutive=0` |

After the flash and verification the owner asked for a fresh archive.
`parquet erase-archive CONFIRM` returned `PARQUET ERASE_ARCHIVE ok=1 files=38
directories=27 reboot_required=1`. No copy was kept, at the owner's request.
After a reset the board printed `PARQUET SCAN files=0` with the same config
line. The phone then sent host time (`PARQUET TIME ... source=host`) and the
logger closed the first new unsynced file.

| Readback | Result |
| :--- | :--- |
| File | `data_unsynced_7948c748..._0-3-0.parquet`, 4 rows, 62 columns, 12,057 bytes, CRC-32 `c6b201a0`, SHA-256 `68de6b280f77c3ca0c2490dbb6444e1f1655aa0d682909e9e39e8757f07d9b16`, `readers_match=true`, cadence deviation at most 1,017 us |
| Values | Two PMS warm-up rows are null, then PM1 17 and 16, PM2.5 22, PM10 22 ug/m3 (CF1 and atmospheric), ambient 23.8 C, RH 44.3%, `pms_error=0`, PMS firmware 152. `battery_mv` 4,275 to 4,277 |

Limits. No phone transfer, LAN or Wi-Fi run, no endurance run. The USB fetch
reset the board through the CH343 bridge, so a few buffered rows after the
fetch were lost. Logs, image and readback are in ignored
`firmware/esp-idf-waveshare-sim7670g/artifacts/install-20261018/`.
