#pragma once

#include <cstddef>
#include <cstdint>

namespace aq {

struct Ltr553Reading {
  std::uint16_t als_ch0 = 0;
  std::uint16_t als_ch1 = 0;
  std::uint16_t proximity = 0;
  bool als_valid = false;
  bool proximity_valid = false;
  bool io_error = false;
  bool als_fresh = false;
  bool proximity_fresh = false;
  bool proximity_saturated = false;
};

// LTR-553ALS-WA. Raw ADC counts, not lux or distance.
// Register definitions: Lite-On Rev 1.0, pp. 14-25 (M5Stack board datasheet).
// https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/datasheet/core/K128%20CoreS3/LTR-553ALS-WA.PDF
// The caller owns the bus, power setup, scheduling and clock callbacks.
struct Ltr553Io {
  void *context = nullptr;
  bool (*read)(void *, std::uint8_t address, std::uint8_t reg,
               std::uint8_t *data, std::size_t size) = nullptr;
  bool (*write)(void *, std::uint8_t address, std::uint8_t reg,
                std::uint8_t value) = nullptr;
  void (*delay_ms)(void *, std::uint32_t) = nullptr;
  std::uint32_t (*millis)(void *) = nullptr;
};

class Ltr553 {
public:
  explicit Ltr553(const Ltr553Io &io) : io_(io) {}
  bool begin() {
    available_ = false;
    // Startup-only wait meets the datasheet's 100 ms VDD settling time.
    if (!io_.read || !io_.write || !io_.delay_ms || !io_.millis) {
      return false;
    }
    io_.delay_ms(io_.context, 100);
    std::uint8_t ids[2]{};
    if (!read_register(0x86, ids, sizeof(ids)) || ids[0] != 0x92 ||
        ids[1] != 0x05) {
      return false;
    }

    // Standby before configuration. Poll status; no interrupt pin ownership.
    // ALS: gain 1x, 100 ms integration, one conversion/second.
    // PS: gain 16x, 40 kHz LED, 50% duty, 20 mA peak, one pulse/second.
    // Enable the PS saturation indicator so saturated readings are invalid.
    const std::uint8_t settings[][2] = {
        {0x80, 0x00}, {0x81, 0x00}, {0x8f, 0x00}, {0x82, 0x2a}, {0x83, 0x01},
        {0x84, 0x05}, {0x85, 0x04}, {0x80, 0x01}, {0x81, 0x23},
    };
    // Ordered device writes need immediate rollback on the first I2C error.
    // cppcheck-suppress useStlAlgorithm
    for (const auto *setting : settings) {
      if (!write_register(setting[0], setting[1])) {
        // Best effort return to standby after a partially applied setup.
        write_register(0x80, 0x00);
        write_register(0x81, 0x00);
        return false;
      }
    }
    started_ms_ = io_.millis(io_.context);
    available_ = true;
    return true;
  }

  bool available() const { return available_; }

  // A snapshot, not an average over the caller's ten-second sample interval.
  // Never wait for a conversion or return the preceding call's cached values.
  // The validity flags are authoritative; encode invalid values as null.
  Ltr553Reading read() const {
    Ltr553Reading result;
    if (!available_ || static_cast<std::uint32_t>(io_.millis(io_.context) -
                                                  started_ms_) < 1100) {
      return result;
    }
    std::uint8_t status = 0;
    if (!read_register(0x8c, &status, 1)) {
      result.io_error = true;
      return result;
    }
    result.als_fresh = (status & 0x04) != 0;
    result.proximity_fresh = (status & 0x01) != 0;

    if (result.als_fresh) {
      // Read CH1 low/high, then CH0 low/high in one transaction. The chip
      // locks the four data registers until 0x8b is read (datasheet p. 21).
      std::uint8_t data[4]{};
      if (read_register(0x88, data, sizeof(data))) {
        result.als_ch1 = little_endian(data);
        result.als_ch0 = little_endian(data + 2);
        // Status bit 7 means INVALID, bits 6:4 report the conversion gain.
        // Only the configured 1x gain is accepted for comparable raw counts.
        result.als_valid = (status & 0xf0) == 0;
      } else {
        result.io_error = true;
      }
    }
    if (result.proximity_fresh) {
      std::uint8_t data[2]{};
      if (read_register(0x8d, data, sizeof(data))) {
        result.proximity = little_endian(data) & 0x07ff;
        result.proximity_saturated = (data[1] & 0x80) != 0;
        result.proximity_valid = !result.proximity_saturated;
      } else {
        result.io_error = true;
      }
    }
    return result;
  }

private:
  bool read_register(std::uint8_t reg, std::uint8_t *data,
                     std::size_t size) const {
    return io_.read(io_.context, kAddress, reg, data, size);
  }

  bool write_register(std::uint8_t reg, std::uint8_t value) const {
    return io_.write(io_.context, kAddress, reg, value);
  }

  static std::uint16_t little_endian(const std::uint8_t *data) {
    return static_cast<std::uint16_t>(
        data[0] | (static_cast<std::uint16_t>(data[1]) << 8));
  }

  static constexpr std::uint8_t kAddress = 0x23;
  Ltr553Io io_;
  std::uint32_t started_ms_ = 0;
  bool available_ = false;
};

} // namespace aq
