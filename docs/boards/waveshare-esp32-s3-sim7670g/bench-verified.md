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
