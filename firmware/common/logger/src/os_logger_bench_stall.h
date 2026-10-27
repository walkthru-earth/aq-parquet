#pragma once

// Bench only. The `parquet bench-stall <s>` serial command blocks the storage
// worker without stamping its heartbeat, so a bench can force the worker
// watchdog (issue #49, #61 item 3). os_logger.cpp compiles the command only
// with CONFIG_OS_LOGGER_BENCH_STALL=y. Pure and allocation free, so the host
// test runs the same parser and deadline the firmware uses.

#include <cstdint>
#include <expected>
#include <string_view>

namespace oslogger::bench {

inline constexpr std::string_view kStallPrefix = "parquet bench-stall ";
// 0 blocks forever. Anything else is a returning stall of that many seconds.
// One hour covers every watchdog timeout the Kconfig range allows (600 s at
// most) plus a warn-mode stall that outlasts several worker-stall warnings.
inline constexpr std::uint32_t kMaxStallSeconds = 3600;

enum class [[nodiscard]] StallError : std::uint8_t {
  Malformed,  // empty, a sign, a space or any other non digit
  OutOfRange, // more than kMaxStallSeconds
};

[[nodiscard]] constexpr const char *stall_error_name(StallError error) {
  using enum StallError;
  switch (error) {
  case Malformed:
    return "invalid-seconds";
  case OutOfRange:
    return "seconds-out-of-range";
  }
  return "unknown";
}

[[nodiscard]] constexpr bool is_stall_command(std::string_view command) {
  return command.starts_with(kStallPrefix);
}

// The seconds after the prefix, decimal digits only. Stops counting once the
// value passes the bound, so a long digit run cannot overflow.
[[nodiscard]] constexpr std::expected<std::uint32_t, StallError>
parse_stall_seconds(std::string_view command) {
  using enum StallError;
  if (!is_stall_command(command))
    return std::unexpected(Malformed);
  const auto digits = command.substr(kStallPrefix.size());
  if (digits.empty())
    return std::unexpected(Malformed);
  std::uint32_t seconds = 0;
  bool over = false;
  for (const char digit : digits) {
    if (digit < '0' || digit > '9')
      return std::unexpected(Malformed);
    if (!over)
      seconds = seconds * 10 + static_cast<std::uint32_t>(digit - '0');
    over = over || seconds > kMaxStallSeconds;
  }
  if (over)
    return std::unexpected(OutOfRange);
  return seconds;
}

// True while a stall that began at start_us still blocks at now_us. A zero
// stall never ends.
[[nodiscard]] constexpr bool stall_continues(std::uint32_t seconds,
                                             std::int64_t start_us,
                                             std::int64_t now_us) {
  return seconds == 0 ||
         now_us - start_us < static_cast<std::int64_t>(seconds) * 1000000;
}

} // namespace oslogger::bench
