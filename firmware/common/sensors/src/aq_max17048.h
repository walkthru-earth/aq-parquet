#pragma once

#include <cstddef>
#include <cstdint>

namespace aq {

// One MAX17048 VCELL/SOC snapshot. Raw words are kept for diagnostics.
// `valid()` is authoritative; encode an invalid reading as null.
struct Max17048Reading {
  std::uint16_t raw_vcell = 0;
  std::uint16_t raw_soc = 0;
  std::int32_t millivolts = 0; // cell voltage, truncated to whole mV
  std::int32_t percent = 0;    // whole percent, truncated and capped at 100
  bool io_error = false;       // a bus transaction failed or IO is unset
  bool plausible = false;      // decoded values pass the range check
  constexpr bool valid() const { return !io_error && plausible; }
};

// Bus-agnostic register read with the same shape as Ltr553Io::read. The
// caller owns the I2C controller, the device handle, bus locking and speed.
// Return false on any transaction error; `data` is then ignored.
struct Max17048Io {
  void *context = nullptr;
  bool (*read)(void *context, std::uint8_t address, std::uint8_t reg,
               std::uint8_t *data, std::size_t size) = nullptr;
};

// Analog Devices MAX17048 1-cell fuel gauge, read-only use.
// Register map: Analog Devices MAX17048/MAX17049 datasheet. Registers are
// 16-bit big-endian. VCELL (0x02) is 78.125 uV/LSB (5/64 mV). SOC (0x04) is
// 1/256 %/LSB. The gauge can estimate above 100% near full charge, so the
// percent is capped while a clearly implausible raw word is rejected
// (outside 2500..4500 mV or above 120% raw SOC). The 120% threshold is a
// project heuristic, not a datasheet limit. Battery presence is a board
// policy: an I2C ACK alone cannot prove that a cell is installed.
// No heap, no blocking waits, no controller initialization.
class Max17048 {
public:
  static constexpr std::uint8_t kAddress = 0x36; // 7-bit
  static constexpr std::uint8_t kRegVcell = 0x02;
  static constexpr std::uint8_t kRegSoc = 0x04;
  static constexpr std::uint8_t kRegVersion = 0x08;
  static constexpr std::int32_t kMinMillivolts = 2500;
  static constexpr std::int32_t kMaxMillivolts = 4500;
  static constexpr std::uint16_t kMaxRawSoc = 120 * 256;

  explicit constexpr Max17048(const Max17048Io &io) : io_(io) {}

  // Reads VCELL then SOC. SOC is not attempted after a failed VCELL read.
  Max17048Reading read() const {
    Max17048Reading result;
    if (!read_word(kRegVcell, result.raw_vcell) ||
        !read_word(kRegSoc, result.raw_soc)) {
      result.io_error = true;
      return result;
    }
    return decode(result.raw_vcell, result.raw_soc);
  }

  bool read_version(std::uint16_t &version) const {
    return read_word(kRegVersion, version);
  }

  static constexpr Max17048Reading decode(std::uint16_t vcell,
                                          std::uint16_t soc) {
    Max17048Reading result;
    result.raw_vcell = vcell;
    result.raw_soc = soc;
    result.millivolts = static_cast<std::int32_t>(vcell) * 5 / 64;
    result.percent =
        soc >= 100 * 256 ? 100 : static_cast<std::int32_t>(soc) / 256;
    result.plausible = result.millivolts >= kMinMillivolts &&
                       result.millivolts <= kMaxMillivolts && soc <= kMaxRawSoc;
    return result;
  }

private:
  bool read_word(std::uint8_t reg, std::uint16_t &value) const {
    std::uint8_t data[2]{};
    if (!io_.read || !io_.read(io_.context, kAddress, reg, data, sizeof(data)))
      return false;
    value = static_cast<std::uint16_t>((data[0] << 8) | data[1]);
    return true;
  }

  Max17048Io io_;
};

} // namespace aq
