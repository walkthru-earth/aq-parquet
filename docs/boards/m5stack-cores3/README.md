# M5Stack CoreS3 board references

The current [CoreS3 application](../../../firmware/esp-idf-cores3/README.md)
uses native **ESP-IDF 6.1** with M5Unified **0.2.25** and M5GFX **0.2.31**
components. Native hardware qualification is pending. Recorded SD, sensor,
RTC, display and radio results belong to their identified historical Arduino
images and do not establish native behavior. The optional M134 contains
PMSA003; its wiring is distinct from Waveshare/PMS5003T.

Use the [native build/flash workflow](../../../firmware/esp-idf-cores3/README.md#current-build-and-flash-workflow)
and [shared development guide](../../shared/development.md) for current source.
The development guide describes the native workflow; dated Arduino API/build commands and resource figures remain historical.

| Topic | File |
| --- | --- |
| Wiring, power, onboard peripherals, boot | [Hardware](cores3-hardware.md) |
| Current native build and component pins | [Application workflow](../../../firmware/esp-idf-cores3/README.md), [shared development](../../shared/development.md) |
| Native drivers/runtime and historical toolchain notes | [Development](cores3-development.md) |
| Wi-Fi/BLE/ESP-NOW | [Wireless](cores3-wireless.md) |
| microSD, SPI and recovery | [Storage](cores3-storage.md) |
| Optional add-ons | [Add-ons](addons.md), [M134/PMSA003](addon-air-quality.md) |
| Historical image measurements and limits | [Bench record](bench-verified.md), [compression experiment](compression-benchmark.md) |

The [shared contracts](../../README.md#shared-contracts) apply to both board targets. Results in the CoreS3 bench record do not establish Waveshare behavior.
