# Board options for a LoRa solar station

[App](mobile-app.md) · [Architecture](mobile-architecture.md) · [Board porting](../docs/shared/board-porting.md)

**Research date 2026-10-18.** Web research across LilyGO, Heltec, Waveshare, Elecrow, RAKwireless, M5Stack and Seeed Studio, checked against product pages, wikis, schematics and driver source where available. Prices are approximate single-unit USD and change often. Items marked *unverified* were not confirmed from a primary source. Nothing here was bought or bench tested. This is a proposal, not a qualified hardware choice.

## What we need

| Need | Why it matters |
| --- | --- |
| ESP32-S3 with **8 MB PSRAM** | The Parquet writer workspace (about 170 KB) lives in PSRAM with no internal RAM fallback ([AGENTS.md](../AGENTS.md)). The 64 KiB persistent log ring also uses PSRAM but falls back to a 4 KiB internal ring without it ([runtime AGENTS.md](../firmware/common/runtime/AGENTS.md)). 2 MB PSRAM is unproven. |
| **16 MB flash** | `partitions.csv` uses two 6 MB app slots, and the backup and flash helpers accept only 16 MiB targets. 8 MB flash means partition and tooling rework. |
| Screen | Any type. OLED and e-paper use the least power. |
| LoRa, **868 MHz high band** | Deployment is in Egypt. See [LoRa band in Egypt](#lora-band-in-egypt) before choosing channels. |
| Battery charging and **solar input** | Unattended outdoor station. |
| **RTC with backup** | The Waveshare logger loses UTC on every reboot today because it has no RTC. |
| microSD, **FAT32** | SDMMC or SPI both work, the mount code is per board. exFAT is disabled. |
| **5 V on battery** for PMS5003T | PMS5003T needs 4.5 to 5.5 V and up to 100 mA, with 3.3 V UART logic at 9600 baud. Most boards only give 5 V while USB is plugged in. |
| **3.3 V I²C** for SEN63C | SEN63C VDD is 3.15 to 3.6 V (absolute max 3.6 V), so a 5 V Grove port must not power it. Address **0x6B**, 100 kHz max, external pull-ups, JST GHR 6-pin cable. Measurement draws 75 mA typical with 200 mA max pulses. The repo has no SEN6x driver yet. |

## Ranked shortlist

Ranked by how little hardware and firmware work each needs to meet every row above. "Extra parts" is what you still buy or wire.

| # | Option | Price (board + extras) | Screen | LoRa | Solar | RTC | SD | 5 V on battery | SEN63C path | Flash / PSRAM | Extra parts | Firmware work |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **1** | **M5Stack CoreS3** + Module13.2 LoRa-1262 | about $60 + $22 + $8 solar | 2.0" IPS touch | SX1262 add-on module, 868 to 923 MHz | No. Add a 5 V solar manager into USB-C | BM8563, backup power *unverified* | Yes, SPI shared with LCD | **Yes**, SY7088 boost on Grove, enabled by M5Unified `setExtOutput()`. Measure first, one M5 page disagrees | Port A pins, but Grove VCC is 5 V. Take 3.3 V from the M-Bus | 16 / 8 | LoRa module, solar manager, 3.3 V SEN63C cable, bigger battery | **Lowest.** Adapter already exists. Add LoRa driver, a third device on the SPI2 lock, SEN6x driver. Move LoRa NSS/BUSY DIP switches off Port A |
| **2** | **RAK WisBlock** RAK3312(H) + RAK19007 + RAK12002 + RAK15002 + RAK1921 | about $15 + $12 + $10 + $10 + $5, plus $70 IP67 solar enclosure | 0.96" OLED | **SX1262 onboard** the core | **Yes**, onboard 5 V solar JST and Li-ion JST | RV-3028 with supercap | Yes, takes the only IO slot | **No.** Add an Adafruit MiniBoost 5 V from VBAT | J12 I²C header at 3.3 V, shared with the OLED via a splitter | 16 / 8 | MiniBoost, J12 splitter, cables | New adapter, RV-3028 and SX1262 drivers. All plug-in modules, the only real wiring is the boost |
| **3** | **Waveshare ESP32-S3-LR1121-OLED-1.54-HF** (not -LF, which is 410 to 510 MHz) | about $40 + $8 solar | 1.54" OLED | **LR1121 onboard**, 850 to 930 MHz plus 2.4 GHz. Not SX1262 | No. Add a 5 V solar manager into USB | PCF85063A, diode-backed from the main cell or a coin cell | Yes | **Yes, always on.** MP28164 buck-boost makes 5 V from the battery. Use the PMS SET pin to sleep it | No I²C connector. GPIO47/48 on header H2 as a second bus, 3.3 V from H1 | 16 / 8 | Solar manager, SH1.0 pigtails | New adapter, LR1121 driver (RadioLib supports it), SEN6x driver |
| **4** | **Seeed XIAO ESP32S3 & Wio-SX1262 kit** + XIAO Expansion Board + LiPo Rider Pro | about $12 + $19 + LiPo Rider | 0.96" OLED | SX1262 via B2B connector | Via LiPo Rider Pro (4.8 to 6.5 V panel) | PCF8563 + CR1220 | Yes, SPI shared with LoRa | Only through the LiPo Rider Pro 5 V boost. XIAO and expansion 5 V pins are USB only | Two Grove I²C ports, but check the supply voltage per port | **8** / 8 on the kit. The XIAO S3 **Plus** has 16 MB, compatibility with the expansion board *unverified* | LiPo Rider Pro (availability *unverified*), battery, case | New adapter plus partition work on 8 MB flash |
| **5** | **Elecrow CrowPanel Advance 3.5"** + SX1262 module | about $26 + $7 + boost + solar | 3.5" IPS touch | SX1262 plug-in module, band *unverified* (Elecrow publishes none) | No | BM8563 + CR1220 | Yes, muxed with the mic | **No.** "IOT_5V" is VBAT on battery | HY2.0 I²C header at 3.3 V, shared with RTC and touch | 16 / 8 | MiniBoost, solar manager | New adapter. Watch pin sharing between SD, mic and the wireless slot |
| **6** | **Waveshare ESP32-S3-SIM7670G-4G** (current board) + Core1262 + RTC breakout + display | $49 + $8 + $5 + display + boost | None onboard | Core1262 over SPI | **Yes**, CN3791 MPPT, 5 to 18 V terminal | None onboard | Yes, SDMMC | **No**, header 5 V is USB only | Header pins | 16 / 8 | Core1262, PCF85063 breakout, display, MiniBoost | Adapter exists. Most loose wiring of all options |
| **7** | **LilyGO T-LoRa Pager**, 868 MHz version | about $87 to $115 | 2.33" IPS | SX1262 onboard | No. USB-C only, feed **regulated** 5 V because VBUS goes straight to the header 5 V pin | PCF85063A + rechargeable coin cell | Yes, SPI shared with LoRa | Yes when firmware enables the BQ25896 OTG boost. Charging and boost cannot run together | **Blocked on the internal bus.** BQ25896 also uses address 0x6B | 16 / 8 | Solar manager, outdoor housing, header cable | New adapter, OTG switchover logic, a second I²C bus for SEN63C on free pins (*unverified* which) |
| **8** | **LilyGO T-Beam Supreme S3**, an 868 MHz option (L76K or u-blox GPS) | about $37 to $60 + boost | 1.3" OLED | SX1262 onboard, own SPI bus | No | PCF8563 + coin cell | Yes | **No**, AXP2101 gives VBUS only with USB | Header SCL/SDA | **8** / 8 | MiniBoost, solar manager | New adapter plus partition rework for 8 MB flash |

### Ruled out

| Board | Reason |
| --- | --- |
| Heltec WiFi LoRa 32 V4 / V4-R8, Wireless Tracker V2, Vision Master E290 | No SD and no RTC onboard, 5 V is USB only. V4 has 2 MB PSRAM. Good solar input on V4 though |
| Heltec Expansion Kit V2 | Has SD and a 2.8" screen but no RTC, and 5 V is still USB only |
| Seeed SenseCAP Indicator D1L / D1Pro | No battery at all. Grove ports and SD sit on the RP2040, so two firmwares |
| Seeed SenseCAP Solar Node P1, Elecrow ThinkNode M1/M3/M4/M6, Heltec T114, LilyGO T-Echo | nRF52840, not ESP32-S3 |
| Seeed reTerminal E1001/E1002 | No LoRa, header has 3.3 V only |
| M5Stack PaperS3, Cardputer ADV, M5Dial, Core2, Tough, Station | No 5 V output, one port, no SD, or not ESP32-S3 |
| LilyGO T-Deck Plus, T3-S3 | No RTC. T3-S3 also has 4 MB flash and 2 MB PSRAM |
| LilyGO T-Display S3 Pro LR1121 | Specs inconsistent across LilyGO pages, 5 V only on USB-C OTG |
| Waveshare ESP32-S3-Touch-LCD 3.5B / 4.3B, ePaper 1.54 | No LoRa, and the RGB panel on 4.3B leaves few free pins |
| Adafruit Feather ESP32-S3 #5477 / #5323 | 4 MB flash with 2 MB PSRAM, or no PSRAM |
| Unexpected Maker FeatherS3 + Adalogger + RFM95W + MiniBoost + bq24074 | Works on paper (16 / 8, about $70 to $80 before a display) but RFM95W is SX1276 and it is the most stacking of any option |

## Recommendation

1. **Fastest path, bench prototype.** M5Stack CoreS3 with Module13.2 LoRa-1262. The firmware adapter, RTC restore after a software reboot and SD path already exist and passed a native readback. RTC retention across a full power-off is unverified. Confirm Grove 5 V on battery with a meter before buying anything else.
2. **Outdoor solar station.** RAK WisBlock. It is the only option with solar charging, LoRa, RTC and SD all as plug-in modules plus an IP67 solar enclosure. The one wired part is a MiniBoost for the PMS5003T.
3. **Single PCB with a real 5 V rail.** Waveshare ESP32-S3-LR1121-OLED-1.54-HF. Everything except solar is onboard, at the cost of an LR1121 radio instead of SX1262.

No board checked here has all nine needs onboard. Every option needs at least one of a solar manager, a 5 V boost or a LoRa module.

## Universal add-on parts

| Part | Price | Use | Note |
| --- | --- | --- | --- |
| [Adafruit MiniBoost 5 V TPS61023 #4654](https://www.adafruit.com/product/4654) | $3.95 | PMS5003T 5 V from a Li-ion cell | EN low fully disconnects the output, so a GPIO can power the PMS down |
| [Pololu U3V16F5](https://pololu.com/product/4941/specs) | about $5 to $7 | Same | **Avoid for gating.** EN low passes VBAT through to the output |
| [DFRobot Solar Power Manager 5V DFR0559](https://uk.robotshop.com/products/dfrobot-sunflower-solar-power-manager-5v) | about $9 | 4.5 to 6 V panel, charges 1S Li-ion, 5 V 1 A out | Feeds a board's USB 5 V input |
| [Waveshare Solar Power Manager (C)](https://www.waveshare.com/solar-power-manager-c.htm) | $19.99 | 6 to 24 V panel, MPPT, 3x 18650, 5 V 3 A out | Bigger battery for cloudy weeks |
| [Adafruit bq24074 charger #4755](https://www.adafruit.com/category/575) | $14.95 | Solar to Li-ion | No 5 V output, needs a boost too |
| [Seeed LiPo Rider Pro](https://wiki.seeedstudio.com/Lipo_Rider_Pro/) | *unverified* | 4.8 to 6.5 V panel, 5 V 1 A boost on battery | Availability *unverified* |
| Sensirion SEN6x cable (JST GHR-06V-S) | a few dollars | SEN63C to header | Keep under 10 cm if possible, 50 cm max |

## Sensor facts

| Sensor | Supply | Current | Bus | Warm-up |
| --- | --- | --- | --- | --- |
| PMS5003T | 4.5 to 5.5 V | 100 mA max working, 200 µA standby with SET low | UART 9600, 3.3 V logic | About 30 s. The current warm-up gate counts uptime and needs revising if sleep is added |
| SEN63C | 3.15 to 3.6 V | 1.7 mA idle, 75 mA typical, 200 mA max pulses | I²C 0x6B, 100 kHz, 5 V tolerant logic, no clock stretching | PM stable after 30 s, CO₂ valid after 24 s, up to 1 h after more than 3 h off. Weekly 400 ppm exposure for auto calibration |

[SEN6x datasheet](https://sensirion.com/resource/datasheet/SEN6x) · SEN63C was about $35 at DigiKey with stock due early November 2026.

## LoRa band in Egypt

Egypt follows the EU plan, so buy **868 MHz / high band** variants of every board and module. The latest NTRA SRD guidelines revision (v1.3, November 2024, Annex 2 Table 1 for non-specific SRDs) list no maximum occupied bandwidth for 865 to 868 MHz, 868 to 868.6 MHz or 869.4 to 869.65 MHz, so a standard 125 kHz LoRa channel fits. All three sub-bands allow **25 mW ERP**. Duty cycle is 1% or LBT+AFA in 865 to 868.6 MHz, and 10% or LBT+AFA in 869.4 to 869.65 MHz. That covers the EU868 LoRaWAN default channels (868.1, 868.3, 868.5 MHz) and the Meshtastic EU_868 slot (869.525 MHz). Set radio power so ERP stays at or under 25 mW (about 14 dBm ERP after antenna gain), well below the 22 dBm an SX1262 can output. TTN lists no Egypt plan. [NTRA SRD guidelines v1.3 PDF](https://www.tra.gov.eg/wp-content/uploads/2024/11/EGY-NTRA-November-2024-SRD_English-1.pdf)

## Deal-breakers checklist

1. Not ESP32-S3, no PSRAM, or flash under 16 MB without accepting partition rework.
2. No microSD slot.
3. No regulated 4.5 to 5.5 V rail on battery and no room to add a gated boost.
4. SEN63C forced onto 5 V supply, or onto a bus that already has a device at 0x6B.
5. Raw solar panel voltage reaching a header 5 V pin that powers the PMS.
6. SD sharing SPI with LoRa or the display without a `lock_bus` / `unlock_bus` pair in the adapter.

## Still unverified

- CoreS3 Grove 5 V on battery (code says yes, the CoreS3 SE page and one ESPHome report disagree).
- CoreS3 BM8563 retention across a full power-off. Only a software reboot restore is bench recorded ([CoreS3 bench record](../docs/boards/m5stack-cores3/bench-verified.md)).
- Module13.2 LoRa-1262 official CoreS3 support and SPI sharing in practice.
- T-LoRa Pager OTG current limit, battery size (500 vs 1500 mAh reported) and the battery to USB switchover.
- Whether the XIAO Expansion Board works with the XIAO ESP32S3 Plus (16 MB flash).
- Heltec V4 SD and RTC, ThinkNode M5 specs, PMS5003T peak current.
- No board in this list has a measured current draw in this repo, so battery and panel sizing is still open.

## Sources

- **LilyGO.** [T-LoRa Pager](https://lilygo.cc/products/t-lora-pager), [Pager hardware doc](https://github.com/Xinyuan-LilyGO/LilyGoLib/blob/master/docs/hardware/lilygo-t-lora-pager.md), [Pager schematics](https://github.com/Xinyuan-LilyGO/LilyGoLib/tree/master/schematic), [T-Beam Supreme](https://lilygo.cc/products/t-beam-supreme), [T-Beam Supreme hardware](https://github.com/Xinyuan-LilyGO/LilyGo-LoRa-Series/blob/master/docs/en/t_beam_supreme/t_beam_supreme_hw.md), [T3-S3](https://lilygo.cc/products/t3s3-v1-0), [T-Display S3 Pro LR1121](https://wiki.lilygo.cc/products/t-display-series/t-display-s3-pro-lr1121/)
- **Heltec.** [WiFi LoRa 32 V4](https://heltec.org/project/wifi-lora-32-v4/), [V4 datasheet](https://s.heltec.cn/download/WiFi_LoRa_32_V4/datasheet/WiFi_LoRa_32_V4.3.1_Datasheet.pdf), [Wireless Tracker V2](https://wiki.heltec.org/docs/devices/open-source-hardware/esp32-series/lora-32/wireless-tracker-v2/), [Vision Master E290](https://heltec.org/project/vision-master-e290/)
- **Waveshare.** [ESP32-S3-LR1121-OLED-1.54](https://www.waveshare.com/product/iot-communication/long-range-wireless/nb-iot-lora/esp32-s3-lr1121-oled-1.54.htm), [its schematic](https://github.com/waveshareteam/esp32-s3-lr1121-oled-1.54/blob/main/hardware/schematics/esp32-s3-lr1121-oled-1_54_schematics.pdf), [SIM7670G docs](https://docs.waveshare.com/ESP32-S3-SIM7670G-4G), [Core1262](https://www.waveshare.com/core1262-868m.htm), [Touch-LCD-4.3B](https://www.waveshare.com/esp32-s3-touch-lcd-4.3b.htm)
- **Elecrow.** [CrowPanel Advance 3.5"](https://www.elecrow.com/crowpanel-advance-3-5-hmi-esp32-ai-display-480x320-artificial-intelligent-ips-touch-screen.html), [CrowPanel Advance schematics](https://github.com/Elecrow-RD/CrowPanel-Advance-HMI-ESP32-AI-Display), [ThinkNode on Meshtastic](https://meshtastic.org/docs/hardware/devices/elecrow/thinknode/)
- **RAKwireless.** [RAK3312](https://docs.rakwireless.com/product-categories/wisblock/rak3312/datasheet/), [RAK19007](https://docs.rakwireless.com/product-categories/wisblock/rak19007/datasheet/), [RAK19001](https://docs.rakwireless.com/product-categories/wisblock/rak19001/datasheet/), [RAK12039 PMSA003I](https://docs.rakwireless.com/product-categories/wisblock/rak12039/datasheet/), [Unify solar enclosure](https://store.rokland.com/collections/solar/products/rak-wireless-solar-unify-enclosure-ip67-150x100x45mm-pre-mounted-m8-5-pin-rp-sma-connector-pid-910421)
- **M5Stack.** [CoreS3](https://docs.m5stack.com/en/core/CoreS3), [CoreS3 SE](https://docs.m5stack.com/en/core/M5CoreS3%20SE), [Module13.2 LoRa-1262](https://docs.m5stack.com/en/module/Module13.2_LoRa-1262), [M5Unified](https://github.com/m5stack/M5Unified)
- **Seeed Studio.** [XIAO ESP32S3 & Wio-SX1262 kit](https://wiki.seeedstudio.com/wio_sx1262_with_xiao_esp32s3_kit/), [XIAO Expansion Board](https://wiki.seeedstudio.com/Seeeduino-XIAO-Expansion-Board/), [XIAO ESP32S3](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/), [SenseCAP Indicator](https://wiki.seeedstudio.com/Sensor/SenseCAP/SenseCAP_Indicator/Get_started_with_SenseCAP_Indicator/), [Solar Node P1](https://wiki.seeedstudio.com/meshtastic_solar_node/), [Meshtastic XIAO S3 pins](https://github.com/meshtastic/firmware/blob/master/variants/esp32s3/seeed_xiao_s3/variant.h)
- **Other.** [Unexpected Maker FeatherS3](https://www.lectronz.com/products/feathers3), [TTN frequencies by country](https://www.thethingsnetwork.org/docs/lorawan/frequencies-by-country/), [NTRA SRD guidelines v1.3](https://www.tra.gov.eg/wp-content/uploads/2024/11/EGY-NTRA-November-2024-SRD_English-1.pdf)
