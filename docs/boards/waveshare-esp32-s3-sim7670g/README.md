# Waveshare ESP32-S3-SIM7670G-4G V2

Board reference for the owner's V2.0 board. Source-checked 2026-09-28; read-only chip, flash and eFuse results from the connected unit are recorded separately. PMS and storage have not been tested yet. The SIM7670G modem, camera, battery gauge and TF slot are onboard capabilities, not assumed active firmware services.

- [Hardware and pin ownership](hardware.md)
- [PMS5003T wiring and UART contract](pms5003t.md)
- [Read-only board evidence](bench-verified.md)
- [Arduino trial](../../../firmware/arduino-waveshare-sim7670g/README.md)

The board's product name includes a modem, but local sampling and SD copies must continue to work without a SIM or internet. A later cellular upload path cannot replace the card or local archive.
