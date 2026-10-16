#pragma once

// Waveshare V2 sensor identity rules for the wired Plantower PMS5003T. The
// shared runtime only stores an owner-provisioned identity; this board decides
// which sticker serials and which batch opt-in are acceptable and installs the
// rules through config::set_sensor_policy. Pure and host-testable.

#include <cstdint>
#include <cstring>

namespace pms5003t {
constexpr const char *kVendor = "Plantower";
constexpr const char *kModel = "PMS5003T";
constexpr const char *kSerialPrefix = "PMS5003T-";
constexpr std::size_t kSerialPrefixLength = 9;
// The one unvalidated batch whose PM2.5 candidate formula this firmware ships.
constexpr const char *kBatchPrefix = "PMS5003T-20260408";
constexpr std::size_t kBatchPrefixLength = 17;

// `PMS5003T-` + a real YYYYMMDD (year >= 2000) + 1..9 unit digits.
inline bool valid_serial(const char *serial) {
  if (!serial)
    return false;
  const auto length = std::strlen(serial);
  if (length < 18 || length > 26 ||
      std::strncmp(serial, kSerialPrefix, kSerialPrefixLength) != 0)
    return false;
  for (const char *p = serial + kSerialPrefixLength; *p; ++p)
    if (*p < '0' || *p > '9')
      return false;
  const int year = (serial[9] - '0') * 1000 + (serial[10] - '0') * 100 +
                   (serial[11] - '0') * 10 + serial[12] - '0';
  const int month = (serial[13] - '0') * 10 + serial[14] - '0';
  const int day = (serial[15] - '0') * 10 + serial[16] - '0';
  if (year < 2000 || month < 1 || month > 12)
    return false;
  constexpr int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  return day >= 1 && day <= days[month] + (month == 2 && leap ? 1 : 0);
}

inline bool wired_identity(const char *vendor, const char *model) {
  return vendor && model && std::strcmp(vendor, kVendor) == 0 &&
         std::strcmp(model, kModel) == 0;
}

// All sticker digits as one integer, or 0 when the identity is not a valid
// PMS5003T profile. At most 17 digits, so it always fits in int64.
inline std::int64_t serial_code(const char *vendor, const char *model,
                                const char *serial) {
  if (!wired_identity(vendor, model) || !valid_serial(serial))
    return 0;
  std::int64_t code = 0;
  for (const char *p = serial + kSerialPrefixLength; *p; ++p)
    code = code * 10 + (*p - '0');
  return code;
}

inline bool batch_eligible(const char *vendor, const char *model,
                           const char *serial) {
  return serial_code(vendor, model, serial) != 0 &&
         std::strncmp(serial, kBatchPrefix, kBatchPrefixLength) == 0;
}
} // namespace pms5003t
