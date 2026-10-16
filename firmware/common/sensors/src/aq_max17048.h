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

// One 16-bit register read with its own success flag. `raw` is only
// meaningful when `ok` is true.
struct Max17048Word {
  std::uint16_t raw = 0;
  bool ok = false;
};

// Diagnostic registers read after VCELL and SOC. Every register has its own
// flag, so one failed transaction never hides the others.
struct Max17048Extended {
  Max17048Word mode;    // 0x06, HibStat is bit 12
  Max17048Word version; // 0x08, 0x001x on production parts
  Max17048Word hibrt;   // 0x0A, hibernate thresholds, POR 0x8030
  Max17048Word config;  // 0x0C, RCOMP high byte, alert config low byte
  Max17048Word valrt;   // 0x14, voltage alert window
  Max17048Word crate;   // 0x16, signed charge rate, 0.208 %/h per LSB
  Max17048Word status;  // 0x1A, alert and reset flags in the high byte

  // Charge rate in thousandths of a percent per hour. Positive is charging,
  // negative is discharging. 0x8000 decodes to -6815.744 %/h.
  constexpr std::int32_t crate_milli_percent_per_hour() const {
    return static_cast<std::int32_t>(static_cast<std::int16_t>(crate.raw)) *
           208;
  }
  constexpr bool hibernating() const {
    return mode.ok && (mode.raw & kModeHibStat) != 0;
  }
  constexpr bool status_bit(std::uint16_t mask) const {
    return status.ok && (status.raw & mask) != 0;
  }
  // STATUS alert and reset flags shifted down to one byte.
  constexpr std::uint8_t status_flags() const {
    return static_cast<std::uint8_t>(status.raw >> 8);
  }

  static constexpr std::uint16_t kModeHibStat = 0x1000;
  static constexpr std::uint16_t kStatusRi = 0x0100;   // reset indicator
  static constexpr std::uint16_t kStatusVh = 0x0200;   // voltage high alert
  static constexpr std::uint16_t kStatusVl = 0x0400;   // voltage low alert
  static constexpr std::uint16_t kStatusVr = 0x0800;   // voltage reset alert
  static constexpr std::uint16_t kStatusHd = 0x1000;   // SOC low alert
  static constexpr std::uint16_t kStatusSc = 0x2000;   // 1% SOC change alert
  static constexpr std::uint16_t kStatusEnVr = 0x4000; // voltage reset enable
};

// Outcome of the one supported register write.
enum class Max17048ClearResult : std::uint8_t {
  Cleared = 0,      // RI was set, cleared and read back clear
  AlreadyClear = 1, // RI was already clear, nothing was written
  ReadFailed = 2,   // STATUS could not be read, nothing was written
  WriteFailed = 3,  // the write transaction failed
  WriteUnset = 4,   // no write callback, no bus traffic at all
  VerifyFailed = 5, // the readback failed or RI is still set
};

constexpr const char *clear_result_name(Max17048ClearResult result) {
  switch (result) {
  case Max17048ClearResult::Cleared:
    return "cleared";
  case Max17048ClearResult::AlreadyClear:
    return "already-clear";
  case Max17048ClearResult::ReadFailed:
    return "read-failed";
  case Max17048ClearResult::WriteFailed:
    return "write-failed";
  case Max17048ClearResult::WriteUnset:
    return "write-unset";
  case Max17048ClearResult::VerifyFailed:
    return "verify-failed";
  }
  return "unknown";
}

// Model-gauge calibration entry point, documented only. The values match
// the Analog Devices default model and are not tuned to any cell. No write
// path exists in this driver. Loading a custom model needs the unlock,
// model write, verify and lock sequence from the Analog Devices
// application note for the MAX17048 and a characterized cell, so it stays
// future work. Temperature coefficients are stored in thousandths.
struct Max17048ModelCalibration {
  std::uint8_t rcomp0 = 0x97;
  std::int32_t temp_co_up_milli = -500;
  std::int32_t temp_co_down_milli = -5000;
  std::uint16_t ocv_test = 0;
  std::uint8_t soc_check_a = 0;
  std::uint8_t soc_check_b = 0;
  std::uint8_t bits = 18;
  std::uint16_t rcomp_seg = 0x0100;
  std::uint8_t model[64]{};
  bool loaded = false; // always false until a characterized model exists
};

// Bus-agnostic register access with the same shape as Ltr553Io::read. The
// caller owns the I2C controller, the device handle, bus locking and speed.
// Return false on any transaction error; `data` is then ignored. `write`
// is optional and only used by clear_reset_indicator().
struct Max17048Io {
  void *context = nullptr;
  bool (*read)(void *context, std::uint8_t address, std::uint8_t reg,
               std::uint8_t *data, std::size_t size) = nullptr;
  bool (*write)(void *context, std::uint8_t address, std::uint8_t reg,
                const std::uint8_t *data, std::size_t size) = nullptr;
};

// Analog Devices MAX17048 1-cell fuel gauge.
// Register map: Analog Devices MAX17048/MAX17049 datasheet. Registers are
// 16-bit big-endian. VCELL (0x02) is 78.125 uV/LSB (5/64 mV). SOC (0x04) is
// 1/256 %/LSB. CRATE (0x16) is signed, 0.208 %/h per LSB. The gauge can
// estimate above 100% near full charge, so the percent is capped while a
// clearly implausible raw word is rejected (outside 2500..4500 mV or above
// 120% raw SOC). The 120% threshold is a project heuristic, not a datasheet
// limit. Battery presence is a board policy: an I2C ACK alone cannot prove
// that a cell is installed.
// The only write is clear_reset_indicator(). Quick start, power-on reset,
// model, RCOMP, HIBRT and alert thresholds are never written.
// No heap, no blocking waits, no controller initialization.
class Max17048 {
public:
  static constexpr std::uint8_t kAddress = 0x36; // 7-bit
  static constexpr std::uint8_t kRegVcell = 0x02;
  static constexpr std::uint8_t kRegSoc = 0x04;
  static constexpr std::uint8_t kRegMode = 0x06;
  static constexpr std::uint8_t kRegVersion = 0x08;
  static constexpr std::uint8_t kRegHibrt = 0x0A;
  static constexpr std::uint8_t kRegConfig = 0x0C;
  static constexpr std::uint8_t kRegValrt = 0x14;
  static constexpr std::uint8_t kRegCrate = 0x16;
  static constexpr std::uint8_t kRegStatus = 0x1A;
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

  // Reads MODE, VERSION, HIBRT, CONFIG, VALRT, CRATE and STATUS in that
  // order. Every register is attempted even when an earlier one failed.
  Max17048Extended read_extended() const {
    Max17048Extended result;
    read_into(kRegMode, result.mode);
    read_into(kRegVersion, result.version);
    read_into(kRegHibrt, result.hibrt);
    read_into(kRegConfig, result.config);
    read_into(kRegValrt, result.valrt);
    read_into(kRegCrate, result.crate);
    read_into(kRegStatus, result.status);
    return result;
  }

  bool read_version(std::uint16_t &version) const {
    return read_word(kRegVersion, version);
  }

  // Clears only the STATUS reset indicator with a read-modify-write that
  // keeps every other bit. Writes nothing when RI is already clear. An alert
  // flag raised between the read and the write is cleared too. Call it once at
  // boot, where the flags are only logged.
  Max17048ClearResult clear_reset_indicator() const {
    if (!io_.write)
      return Max17048ClearResult::WriteUnset;
    std::uint16_t status = 0;
    if (!read_word(kRegStatus, status))
      return Max17048ClearResult::ReadFailed;
    if ((status & Max17048Extended::kStatusRi) == 0)
      return Max17048ClearResult::AlreadyClear;
    const auto next =
        static_cast<std::uint16_t>(status & ~Max17048Extended::kStatusRi);
    const std::uint8_t data[2]{static_cast<std::uint8_t>(next >> 8),
                               static_cast<std::uint8_t>(next & 0xff)};
    if (!io_.write(io_.context, kAddress, kRegStatus, data, sizeof(data)))
      return Max17048ClearResult::WriteFailed;
    std::uint16_t readback = 0;
    if (!read_word(kRegStatus, readback) ||
        (readback & Max17048Extended::kStatusRi) != 0)
      return Max17048ClearResult::VerifyFailed;
    return Max17048ClearResult::Cleared;
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

  // Uncapped SOC in percent, 1/256 % resolution.
  static constexpr double raw_soc_percent(std::uint16_t soc) {
    return static_cast<double>(soc) / 256.0;
  }

private:
  bool read_word(std::uint8_t reg, std::uint16_t &value) const {
    std::uint8_t data[2]{};
    if (!io_.read || !io_.read(io_.context, kAddress, reg, data, sizeof(data)))
      return false;
    value = static_cast<std::uint16_t>((data[0] << 8) | data[1]);
    return true;
  }

  void read_into(std::uint8_t reg, Max17048Word &word) const {
    std::uint16_t value = 0;
    word.ok = read_word(reg, value);
    word.raw = word.ok ? value : 0;
  }

  Max17048Io io_;
};

} // namespace aq
