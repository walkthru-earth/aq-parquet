#pragma once

#include <cstdint>

namespace aq {
namespace utc {
// Codes shared by sync/status consumers. The board supplies any RTC adapter;
// these helpers neither invent an anchor nor change a captured row's epoch.
enum Source : std::int32_t { None = 0, Host = 1, Rtc = 2, Network = 3 };
constexpr std::int64_t kMinEpochSeconds = 1577836800LL; // 2020-01-01 UTC
constexpr std::int64_t kMaxEpochSeconds = 4102444800LL; // 2100-01-01 UTC

constexpr bool supported_epoch(std::int64_t seconds) {
  return seconds >= kMinEpochSeconds && seconds <= kMaxEpochSeconds;
}

// Validate before multiplying; the supported range fits signed nanoseconds.
constexpr bool supported_anchor(std::int64_t seconds, std::uint32_t micros) {
  return supported_epoch(seconds) && micros < 1000000 &&
         (seconds < kMaxEpochSeconds || micros == 0);
}

constexpr std::int64_t anchor_ns(std::int64_t seconds, std::uint32_t micros) {
  return seconds * 1000000000LL + static_cast<std::int64_t>(micros) * 1000;
}

constexpr std::int64_t estimate_ns(std::int64_t now_mono_us,
                                   std::int64_t anchor_mono_us,
                                   std::int64_t anchor_utc_ns) {
  return anchor_utc_ns + (now_mono_us - anchor_mono_us) * 1000;
}

// Whole-second legacy host requests and transport latency can make harmless
// refreshes differ slightly. Keep the discontinuity threshold below the
// ten-second sampling period; each future row still records its exact anchor.
constexpr std::int64_t kEpochDiscontinuityNs = 2000000000LL;
constexpr bool starts_new_epoch(std::int32_t previous_source,
                                std::int32_t next_source,
                                std::int64_t skew_ns) {
  return previous_source == None ||
         (previous_source == Rtc &&
          (next_source == Host || next_source == Network)) ||
         skew_ns > kEpochDiscontinuityNs || skew_ns < -kEpochDiscontinuityNs;
}

constexpr const char *source_name(std::int32_t source) {
  return source == Host      ? "host"
         : source == Rtc     ? "rtc"
         : source == Network ? "network"
                             : "none";
}

// Days since 1970-01-01 for a valid proleptic Gregorian calendar date.
// Howard Hinnant's days_from_civil algorithm; independent of process TZ.
constexpr std::int64_t days_from_civil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}
} // namespace utc
} // namespace aq
