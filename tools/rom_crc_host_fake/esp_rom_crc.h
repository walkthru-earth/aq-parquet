#pragma once

// Host stand-in for the ESP32-S3 ROM CRC-32 routine. Like the ROM and the
// ESP-IDF table fallback (components/esp_rom/patches/esp_rom_crc.c), it
// inverts the running value on entry and exit, so chained calls take and
// return final CRC values. Only tools/test_common.py builds with it.
#include <cstdint>

inline std::uint32_t esp_rom_crc32_le(std::uint32_t crc,
                                      std::uint8_t const *buf,
                                      std::uint32_t len) {
  static const auto table = [] {
    struct Table {
      std::uint32_t values[256];
    } result{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t value = i;
      for (unsigned bit = 0; bit < 8; ++bit)
        value = (value >> 1) ^ (0xedb88320U & (0U - (value & 1U)));
      result.values[i] = value;
    }
    return result;
  }();
  crc = ~crc;
  for (std::uint32_t i = 0; i < len; ++i)
    crc = table.values[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
  return ~crc;
}
