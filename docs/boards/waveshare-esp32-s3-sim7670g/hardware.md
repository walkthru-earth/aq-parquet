# Waveshare ESP32-S3-SIM7670G-4G V2 hardware

The current board adapter uses native ESP-IDF 6.1 UART, I²C and SDMMC drivers,
with led_strip 3.1.0~1 for GPIO38. Native builds pass; physical native behavior
is unqualified. The source checks and measured runs below retain their dated
historical Arduino-image scope. [Current firmware](../../../firmware/esp-idf-waveshare-sim7670g/README.md).

Source-checked 2026-09-28 for PCB revision **V2.0**, as confirmed by the owner. Identification, flash capacity/type, PSRAM initialization/readback, eFuse status, PMS UART reception and bounded logger TF write/serial readback were measured on the unit. Remaining pin assignments and optional peripheral function are source-checked; RGB visual operation, encrypted BLE transfer and Wi-Fi/LAN remain unverified. See the [bench record](bench-verified.md). [Waveshare product documentation](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G), [Arduino guide with V1/V2 tables](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G/Arduino), [FAQ and recovery](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G/FAQ).

| Function | V2 pins / source status | Ownership rule |
| --- | --- | --- |
| PMS5003T UART | Host RX GPIO1, TX GPIO2, per owner's four-wire connection | Dedicated native `UART_NUM_1`, 9600 8N1; sensor TX crosses to host RX |
| TF slot | SDMMC CLK GPIO5, CMD GPIO4, DATA0 GPIO6, card detect GPIO46 | One storage worker; use SDMMC one-bit mode, not CoreS3 SPI pins |
| Battery gauge MAX17048 | SDA GPIO15, SCL GPIO16 on V2 | Logger adapter owns this I²C bus with explicit installed-battery configuration; V1 GPIO3/GPIO2 mapping is wrong here. A bounded USB-attached readback succeeded on 2026-09-30. |
| Camera | V2 mapping includes XCLK GPIO39 and PCLK GPIO46 | Camera and TF card detect need a conflict check before use together |
| RGB LED | GPIO38 | Logger implements startup/card/sensor state; visual operation unverified; no file-copy or durability claim |
| SIM7670G, camera, GNSS | Onboard | Not initialized by the diagnostic or current logger adapter |

Waveshare identifies the module as ESP32-S3R8 with **16 MB flash and 8 MB PSRAM**. Its FAQ gives conflicting PSRAM-mode advice. The first diagnostic leaves PSRAM disabled. A separate 2026-09-28 OPI build booted on this unit, reported initialized physical/heap capacity of **8,388,608 bytes**, and passed four patterns over a 512 KiB allocation. The logger consequently selects **Quad/QIO flash with OPI PSRAM** (native `CONFIG_SPIRAM_MODE_OCT`; historical Arduino `qio_opi` SDK), checks initialized physical capacity before starting, and puts its bounded writer workspace in PSRAM. The full logger subsequently booted and wrote/read Parquet files; this establishes bounded operation, not all-address or endurance coverage. Evidence and image identities belong in [bench-verified.md](bench-verified.md). Do not reuse CoreS3's Quad-PSRAM setting.

Before any first flash write on another unit, run the root `ports`, `chip`, `flash-id`, `efuse` and full verified-backup sequence. Firmware build wrappers verify the exact locked target and selected SDK; board identity and flash size still require physical readback.

The board's UART download and BOOT+RESET recovery are described in the [Waveshare FAQ](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G/FAQ). Verify the actual port and recovery behavior on this V2 unit before treating CoreS3 serial-control observations as portable. Strapping pin GPIO46 is already used for card detect in the vendor example; do not add a pull or driver to it.

The pictured PMS power connection uses the exposed 5 V and GND. Confirm the selected 5 V rail is present under the board's chosen USB/battery supply mode before attributing absent UART frames to the sensor. No board power measurement has been made here.

The current logger has no external RTC adapter. UTC stays null after boot until an explicit host anchor or successful native Wi-Fi SNTP response arrives; GNSS/modem time is not substituted. The owner subsequently installed an 18650 and verified battery-only boot. The logger explicitly configures it as installed and samples MAX17048 cell voltage and estimated SOC; an I²C acknowledgement alone still would not establish battery presence. The V2 schematic routes ETA6098 `STAT` and solar-charger `CHRG` to indicator circuits without a verified ESP32 input, so the logger does not infer charging from voltage or USB attachment.

## Power slider and DIP switches

The [product hardware description](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G#hardware-description) identifies the single ON/OFF slider beside the battery holder as the **18650 battery power switch**. Use ON for battery-powered operation. USB already powered the diagnostic during this board's tests; those tests do not establish battery operation or the sensor's 5 V supply under either slider position. The slider is separate from BOOT/RESET.

The [FAQ](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G/FAQ#hardware-functions) lists the separate DIP switches: **CAM** enables the camera, **HUB** powers the USB hub, **4G** powers the cellular module, and **USB** selects its USB path. None is documented as a PMS5003T enable. Keep the working USB/UART configuration for host diagnostics; switching HUB off may remove host USB access. Identify the switch by its position/label before applying these instructions.
