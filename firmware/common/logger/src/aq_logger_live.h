#pragma once

#include <aq_logger.h>
#include <cstddef>
#include <cstdint>

namespace aqlogger {
// LIVE keys are short JSON names chosen by each board's field table. They are
// emitted verbatim, so keep them to 1..15 characters of [a-z0-9_].
constexpr std::size_t kMaxLiveKey = 15;

// True when every non-null live_key is well formed and no two fields share
// one. A table with no live keys is valid and publishes `{}`.
[[nodiscard]] bool valid_live_keys(const Field *fields, std::size_t count);

// Formats one row as the LIVE JSON object. Emits every field that declares a
// live_key and holds a valid value, in field-table order. Null values and
// non-finite floats are omitted, never written as zero. INT32/INT64 print as
// integers and FLOAT with one decimal. Returns the length without the NUL, or
// zero when `capacity` is too small. Never reports truncated JSON as valid.
[[nodiscard]] std::size_t format_live(const Field *fields, std::size_t count,
                                      const Row &row, char *out,
                                      std::size_t capacity);

// Bytes the LIVE object for `row` needs without the NUL, with no capacity
// limit. An overflow uses it to report the exact size it would have needed.
[[nodiscard]] std::size_t live_size(const Field *fields, std::size_t count,
                                    const Row &row);

// What to send for one sample. `length` bytes of `out` go to BLE and LAN,
// zero means send nothing. `needed` is nonzero only after an overflow and
// holds the bytes the full object would have needed. `log` asks the caller to
// print the format_live_overflow line.
struct LivePublish {
  std::size_t length = 0;
  std::size_t needed = 0;
  bool log = false;
};

// Writes the overflow log line, `PARQUET LIVE overflow bytes=<needed>
// max=<max>` and a newline, into `out`. Returns its length, or zero when
// `capacity` is too small.
[[nodiscard]] std::size_t format_live_overflow(char *out, std::size_t capacity,
                                               std::size_t needed,
                                               std::size_t max);

// Formats LIVE and handles an object larger than `capacity - 1`. Instead of
// skipping the publish, which would leave the previous sample cached and
// served as current, it writes `{}` (every key absent) and asks for a log
// line at most once per kRepeatUs. Pure and single owner, the logger calls it
// from the main task only.
class LiveOverflow {
public:
  static constexpr std::int64_t kRepeatUs = 3600LL * 1000000LL;
  [[nodiscard]] LivePublish publish(const Field *fields, std::size_t count,
                                    const Row &row, char *out,
                                    std::size_t capacity, std::int64_t now_us);

private:
  bool logged_ = false;
  std::int64_t last_log_us_ = 0;
};
} // namespace aqlogger
