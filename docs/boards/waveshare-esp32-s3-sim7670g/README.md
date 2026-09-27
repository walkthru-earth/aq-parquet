# Waveshare ESP32-S3-SIM7670G-4G V2

Board reference for the owner's V2.0 board. On 2026-09-28, identification, full backup, diagnostic boot, TF mount/capacity and valid PMS5003T UART reception were verified. Storage writes, Parquet/sync, modem, GNSS, camera, battery gauge and charging remain unverified or unimplemented. Source checks and physical measurements are recorded separately.

- [Hardware and pin ownership](hardware.md)
- [PMS5003T wiring and UART contract](pms5003t.md)
- [Hardware evidence and limitations](bench-verified.md)
- [Official software resources and V2 adoption limits](software-resources.md)
- [Arduino trial](../../../firmware/arduino-waveshare-sim7670g/README.md)

The board's product name includes a modem, but local sampling and SD copies must continue to work without a SIM or internet. A later cellular upload path cannot replace the card or local archive.
