# Waveshare ESP32-S3-SIM7670G-4G V2 hardware

Source-checked 2026-09-28 for PCB revision **V2.0**, as confirmed by the owner. The board identification, flash capacity/type, PSRAM capacity, and eFuse status were read from the connected unit on 2026-09-28; wiring and peripheral function remain source-checked. See the [bench record](bench-verified.md). [Waveshare product documentation](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G), [Arduino guide with V1/V2 tables](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G/Arduino), [FAQ and recovery](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G/FAQ).

| Function | V2 pins / source status | Ownership rule |
| --- | --- | --- |
| PMS5003T UART | Host RX GPIO1, TX GPIO2, per owner's four-wire connection | Dedicated `HardwareSerial(1)`, 9600 8N1; sensor TX crosses to host RX |
| TF slot | SDMMC CLK GPIO5, CMD GPIO4, DATA0 GPIO6, card detect GPIO46 | One storage worker; use SDMMC one-bit mode, not CoreS3 SPI pins |
| Battery gauge MAX17048 | SDA GPIO15, SCL GPIO16 on V2 | Do not use V1 GPIO3/GPIO2 gauge mapping; one I²C owner |
| Camera | V2 mapping includes XCLK GPIO39 and PCLK GPIO46 | Camera and TF card detect need a conflict check before use together |
| RGB LED | GPIO38 | Optional diagnostic only |
| SIM7670G, camera, GNSS | Onboard | Not initialized by the first UART diagnostic trial |

Waveshare identifies the module as ESP32-S3R8 with **16 MB flash and 8 MB PSRAM**. Its FAQ gives conflicting PSRAM-mode advice. The first diagnostic build leaves PSRAM disabled and does not need it. Before a Parquet/PSRAM build or any flash write, read the connected board with `ports`, `chip`, `flash-id`, and `efuse`, make a full backup matching the verified flash capacity, then confirm the image byte count. Do not reuse the CoreS3 Quad-PSRAM setting by inference.

The board's UART download and BOOT+RESET recovery are described in the [Waveshare FAQ](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G/FAQ). Verify the actual port and recovery behavior on this V2 unit before treating CoreS3 serial-control observations as portable. Strapping pin GPIO46 is already used for card detect in the vendor example; do not add a pull or driver to it.

The pictured PMS power connection uses the exposed 5 V and GND. Confirm the selected 5 V rail is present under the board's chosen USB/battery supply mode before attributing absent UART frames to the sensor. No board power measurement has been made here.
