# Waveshare V2 software resources

Source-checked **2026-09-28** for ESP32-S3-SIM7670G-4G **V2.0**. This is a resource/adoption map; diagnostic/full logger boot, PMS5003T UART reception, TF mount and bounded Parquet write/serial readback, host-UTC behavior, OPI PSRAM and BLE advertising/discovery have run on our unit. BLE file transfer and Wi-Fi/LAN remain unverified; RGB visual behavior was not checked. The owner has since verified battery-only boot, and a 2026-09-30 USB-attached readback verified MAX17048 voltage/SOC rows. The Android app then showed 4,277 mV with board USB attached and 4,067 mV after it was unplugged. Percentage accuracy and charging behavior remain untested. Modem, GNSS and camera remain untested. Physical evidence belongs in the [bench record](bench-verified.md).

## Is there an M5Unified equivalent?

No matching unified board package was found in the [Waveshare organization](https://github.com/orgs/waveshareteam/repositories). The public GitHub API returned 166 repositories across two pages, with no SIM7670/A7670 product repository. The two plausible support repositories were also inspected:

| Official repository | Result for this board |
| --- | --- |
| [Waveshare-ESP32-components](https://github.com/waveshareteam/Waveshare-ESP32-components/tree/30d0ac3b8b6b27ebd402c5852819cbe0dab92749) | Apache-2.0, primarily display BSPs; no SIM7670G BSP or MAX17048/modem driver found. Sensor folders contain QMI8658, PCF85063A and a custom expander. |
| [waveshare_boards](https://github.com/waveshareteam/waveshare_boards/tree/37f2df77734b2f894c0935d9d2cf57ddf23e1079) | Apache-2.0 Board Manager definitions for four touch/display boards; no SIM7670G definition. `esp_board_manager_init()` does not configure this board. |

The practical source is the product's [V2 example archive](https://files.waveshare.com/wiki/ESP32-S3-A-SIM7670X-4G-HAT/Demo/ESP32-S3-A-SIM7670X-4G-V2.zip), linked by the [official resource page](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G/Resources-And-Documents). Inspected archive SHA-256: `a7e618a1cf226129d20450f4d3ce02e04be0b585f776673f07298dde320cc97a`. Internal root is `ESP32-S3-A-SIM7670X-4G-V2/`; Arduino examples are under `Arduino-V3.3.4/examples/`, IDF examples under `ESP-IDF/`. These are upstream reference examples, not installed dependencies; our diagnostic stays pinned to its existing toolchain.

## Useful examples and adoption boundaries

| Hardware | Verified upstream example/API | V2 compatibility and project status |
| --- | --- | --- |
| TF storage | Archive `SD/SD.ino`: `SD_MMC.setPins`, `begin`, `cardType`, `cardSize` | CLK5/CMD4/D0=6, one-bit mode. Diagnostic mounted the card; the logger's storage worker wrote/finalized bounded Parquet files fetched over serial and checked by two readers. Endurance/power-cut durability remain unmeasured. |
| Fuel gauge | Archive `bat/bat.ino`; [MAX17048 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/max17048-max17049.pdf) | Address 0x36, SDA15/SCL16. The sample reads VCELL register 0x02, calls it `soc`, and scales raw/65535×5; do not use that as battery percentage. The logger adapter uses VCELL 0x02 at 78.125 µV/LSB and SOC 0x04 at 1/256 %/LSB. A bounded USB-attached readback verified non-null rows, and the app later displayed battery-only live values; accuracy remains unverified. |
| Camera | Archive `CameraWebServer/`, `CAMERA_MODEL_WAVESHARE_7670_BOARD`; [Espressif camera driver](https://github.com/espressif/esp32-camera) `esp_camera_init`, `esp_camera_sensor_get` | V2 XCLK39, SCCB SDA15/SCL16, D0…D7=7…14, VSYNC42/HREF41/PCLK46, no reset/power GPIO. CAM DIP ON; V2 normally ships OV5640. Gauge shares SCCB pins; GPIO46 also appears as TF detect in the guide. Audit controller/detect ownership before combining. Camera remains disabled. |
| RGB indicator | Archive `RGB/RGB.ino`: `Adafruit_NeoPixel(1,38,NEO_GRB+NEO_KHZ800)` | WS2812B GPIO38. Current adapter uses Arduino `rgbLedWrite` for startup/card/sensor state; visual behavior has not been verified. |
| GNSS | Archive `ESP-IDF/ESP32-S3-XXX7670X-4G-GNSS/`: UART AT commands, then NMEA output | Example ESP RX17/TX18, 115200 8N1, UART1. PMS already owns UART1 in our diagnostic: choose a separate controller and verify UART direction against the V2 schematic before integration. GNSS antenna/fix required; no fabricated position/time. |
| Cellular sockets | Archive IDF `ESP32-S3-XXX7670X-4G-TCP/` and `...HTTP/` | Same UART mapping; modem-side TCP/HTTP AT flows, not an ESP IP/PPP interface. APN/server values are demo settings. Examples accept error strings in some success checks and require review before reuse. Not implemented. |

The [Arduino guide](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G/Arduino) confirms these V1/V2 pin changes. V1 gauge SDA3/SCL2 must not replace V2 SDA15/SCL16. The archive's `AP/AP.ino` is a plain Wi-Fi/HTTP LED example, not a cellular hotspot; it drives GPIO2, which conflicts with our PMS TX connection. The Arduino GNSS/cloud sketch also has a stray non-ASCII character after a return statement and cloud-specific MQTT configuration; use the IDF AT flow as reference instead of copying it into the sampler.

### USB modem / PPP

The [Waveshare FAQ](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G/FAQ) points directly to [Espressif's USB CDC 4G example](https://github.com/espressif/esp-iot-solution/tree/master/examples/usb/host/usb_cdc_4g_module). Its [USB PPP component](https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_host/usb_ppp.html) provides `usbh_modem_install`, `usbh_modem_get_netif`, `usbh_modem_ppp_start/stop` and AT-parser access. The inspected [component manifest](https://github.com/espressif/esp-iot-solution/blob/master/components/usb/iot_usbh_modem/idf_component.yml) reports 2.2.0 and IDF≥5.1; these are dated observations, not project pins.

This is the useful modem implementation candidate. The current compatibility table includes A7670E but not SIM7670G: confirm our modem firmware's PPP support and USB VID/PID plus AT/data interface numbers first. Waveshare specifies 4G ON and USB OFF for the modem-to-ESP hotspot path; this changes USB routing, so keep it a separate deliberate hardware step. Native USB belongs to this host stack while active. The [vendor hotspot](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G/Firmware) and archive `4gbin/` are binaries; the nested ZIP contains only a BIN. They are not reusable source modules and were not flashed. Cellular remains an optional later transport after the local archive.

### Gauge, charging and power

[Adafruit_MAX1704X](https://github.com/adafruit/Adafruit_MAX1704X) is a matching chip-level candidate with `begin(TwoWire*)`, `cellVoltage()`, `cellPercent()` and alert APIs ([header](https://github.com/adafruit/Adafruit_MAX1704X/blob/main/Adafruit_MAX1704X.h)); it is not a Waveshare BSP. Initialize one V2 bus in the board adapter and borrow that bus. Its BSD license and BusIO dependency need exact pins/versions when adopted; do not use reset/quick-start as routine reads.

The [V2 schematic](https://files.waveshare.com/wiki/ESP32-S3-A-SIM7670X-4G-HAT/ESP32-S3-A-SIM7670X-4G-V2.pdf) identifies an ETA6098 charger and MAX17048 gauge. ETA6098 `STAT` and solar-charger `CHRG` appear to drive indicators, with no verified route to an ESP32 input; no charger-management API was found in the examples. The MAX17048 measures cell voltage and estimates SOC, but does not establish charging state or USB input. Hardware switch/rail behavior is in [hardware.md](hardware.md). FAQ software modem power mentions GPIO33 or GPIO22 without a V2-specific mapping; do not treat that as a verified V2 output or substitute the Arduino GNSS sketch's GPIO21 write without a circuit audit.

## Module plan and unknowns

Keep V2 pins, SD mount, rail/USB routing and optional peripheral ownership in the Waveshare trial's board adapter. Candidate gauge/modem drivers may later be components with injected buses and explicit capabilities. Shared sampling, file contracts and BLE/LAN sync should consume those capabilities without initializing board pins.

Before adoption, pin source/dependencies and preserve licenses. The archive has no top-level license for Waveshare's sketches/IDF demos; bundled Adafruit licenses do not grant a blanket license over those files. Prefer documented pin facts and separately licensed upstream drivers (Espressif camera and ESP-IoT-Solution: Apache-2.0; individual dependency notices still apply).

Open measurements: encrypted BLE pairing/file transfer, Wi-Fi/LAN provisioning/file transfer, multi-group/LZ4/reset-partial behavior, sampling under radio load, storage/PSRAM endurance, modem identity/firmware/USB descriptors and PPP, UART AT/GNSS, gauge correctness on an explicitly installed battery, battery/solar charging and 5 V rail availability, and camera/gauge/TF coexistence. The bounded logger/PSRAM runs do not establish endurance or power-cut durability.
