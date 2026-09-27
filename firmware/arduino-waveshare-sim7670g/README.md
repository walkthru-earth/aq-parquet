# Arduino-ESP32 Waveshare V2 / PMS5003T trial

**Status: active diagnostic; not flashed or verified on hardware.** Framework: Arduino-ESP32 **3.3.11**, Arduino CLI **1.5.1**, with the shared `firmware/common` library. The separate trial is justified by a concrete hardware change: Waveshare V2 uses GPIO1/2 for the owner's PMS5003T and a one-bit SDMMC TF slot, while the existing CoreS3 trial uses M5Unified board services, GPIO18/17 for PMSA003 and SPI SD. The PMS5003T also changes the meaning of two frame words to temperature and humidity. These cannot be addressed by changing a build target alone.

The first image is intentionally a **UART-only diagnostic**. It prints chip/flash/PSRAM facts and every 10 seconds reports frame count, frame age, parser errors, PM, temperature and humidity after a 30-second warm-up gate. It does not initialize the modem, camera, battery gauge, TF slot, BLE or Parquet writer. It never formats or writes to the TF card. Pin assumptions and the source references are in [Waveshare hardware](../../docs/boards/waveshare-esp32-s3-sim7670g/hardware.md) and [PMS5003T](../../docs/boards/waveshare-esp32-s3-sim7670g/pms5003t.md).

```sh
pixi install
pixi run waveshare-setup
pixi run waveshare-build
```

**Before the first flash:** confirm the physical board is V2.0 and the attached serial port with `pixi run ports`; read `pixi run chip`, `pixi run flash-id`, and `pixi run efuse` on that port; confirm 16 MB flash and recoverable security fuses; take a complete backup and check its exact byte size. Waveshare's [FAQ](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G/FAQ) documents UART download and BOOT+RESET recovery. Confirm these on the actual board. After that evidence, flash the built UART diagnostic with `pixi run waveshare-flash <checked-port> backup/waveshare-sim7670g-v2-flash-<timestamp>.bin`. The script refuses an absent or wrong-size backup. It writes the ESP32-S3 flash and requires deliberate use; it does not touch eFuses or TF data. See [root safety rules](../../AGENTS.md#do-not-brick-the-board).

**Next required gates:** real UART capture identifying valid PMS5003T frames and revision, PSRAM-mode readback, then SDMMC one-bit read-only mount and a bounded write/readback experiment. Only after those results should the board get a versioned Parquet measurement contract and the existing local BLE/LAN sync path. The CoreS3 77-column schema and its bench figures cannot be relabeled as Waveshare results.

**Last verified on real hardware:** never. Build verification is recorded separately in this README when run.
