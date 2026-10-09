# Waveshare ESP32-S3-SIM7670G-4G V2

The current [V2.0 application](../../../firmware/esp-idf-waveshare-sim7670g/README.md)
is native **ESP-IDF 6.1**, with logger and UART/read-only TF diagnostic variants.
The logger retains UART1 PMS5003T, one-bit SDMMC, OPI PSRAM, UART0 console,
MAX17048 and the 52-column v2 dictionary. **Native hardware qualification is
pending.** Use the [native build/flash workflow](../../../firmware/esp-idf-waveshare-sim7670g/README.md#current-build-and-flash-workflow)
and [shared development guide](../../shared/development.md).

The following hardware observations describe historical Arduino images.
On 2026-09-28, identification, full backup, diagnostic/full logger boot, TF mount/capacity, PMS5003T reception, OPI PSRAM and bounded 49-field Parquet serial readbacks with unsynced/host-UTC behavior were verified. BLE advertising/discovery worked; the first macOS encrypted-pairing attempt timed out, while a later Android foreground bond repair succeeded. The owner has since installed an 18650 and verified battery-only boot. A USB-attached 2026-09-30 readback verified MAX17048 voltage/SOC rows; the connected Android app then displayed 4,277 mV on USB and 4,067 mV after board USB was unplugged, both with capped 100%. BLE file transfer and Wi-Fi/LAN remain unverified. Modem, GNSS, camera and charging status remain unimplemented/unverified. RGB visual behavior was not checked. Source checks, host tests and physical measurements are recorded separately.

- [Hardware and pin ownership](hardware.md)
- [PMS5003T wiring and UART contract](pms5003t.md)
- [Cairo PM2.5 publication and field-quality contract](cairo-pm25-publication.md)
- [Logger storage, measurement contract and local sync](storage.md)
- [Hardware evidence and limitations](bench-verified.md)
- [Official software resources and V2 adoption limits](software-resources.md)
- [Native ESP-IDF application and historical image notes](../../../firmware/esp-idf-waveshare-sim7670g/README.md)

The board's product name includes a modem, but local sampling and SD copies must continue to work without a SIM or internet. A later cellular upload path cannot replace the card or local archive.
