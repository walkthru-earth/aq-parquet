# Waveshare V2 bench record

## Board identification, 2026-09-28

The owner identified the PCB as V2.0. With the board connected by its USB serial bridge at `/dev/cu.wchusbserial5B901533371`, `pixi run ports` listed the WCH USB serial interface separately from four Qualcomm modem ports. Read-only `pixi run chip --port ...`, `flash-id --port ...`, and `efuse --port ...` returned:

- ESP32-S3, silicon revision v0.2; eFuse identifies 8 MB embedded AP_3v3 PSRAM.
- Flash ID manufacturer `85`, device `2018`, capacity **16 MB**, quad flash wiring at 3.3 V.
- `SECURE_BOOT_EN=False`, `SPI_BOOT_CRYPT_CNT=0`, `DIS_DOWNLOAD_MODE=False`; the normal download path remains available.

The chip command's esptool exit toggled RTS to reset the board. These commands did not establish which PSRAM mode an Arduino firmware should select, whether the PMS5003T is producing valid frames, whether the TF card mounts, or whether a new image boots. Those require a separate diagnostic run. The full flash backup read, exact size and SHA-256 are recorded below when complete.
