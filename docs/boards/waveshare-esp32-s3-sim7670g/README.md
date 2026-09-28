# Waveshare ESP32-S3-SIM7670G-4G V2

Board reference for the owner's V2.0 board. On 2026-09-28, identification, full backup, diagnostic/full logger boot, TF mount/capacity, PMS5003T reception, OPI PSRAM and bounded 49-field Parquet serial readbacks with unsynced/host-UTC behavior were verified. BLE advertising/discovery worked; the first macOS encrypted-pairing attempt timed out, so secure BLE transfer and Wi-Fi/LAN operation remain unverified. The owner uses USB only with no installed battery; modem, GNSS, camera, gauge and charging are unimplemented/unverified. RGB visual behavior was not checked. Source checks, host tests and physical measurements are recorded separately.

- [Hardware and pin ownership](hardware.md)
- [PMS5003T wiring and UART contract](pms5003t.md)
- [Logger storage, measurement contract and local sync](storage.md)
- [Hardware evidence and limitations](bench-verified.md)
- [Official software resources and V2 adoption limits](software-resources.md)
- [Arduino trial](../../../firmware/arduino-waveshare-sim7670g/README.md)

The board's product name includes a modem, but local sampling and SD copies must continue to work without a SIM or internet. A later cellular upload path cannot replace the card or local archive.
