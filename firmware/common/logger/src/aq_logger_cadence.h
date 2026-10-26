#pragma once

#include <cstddef>
#include <cstdint>

// Sampling cadence and RAM batch bounds shared by the logger runtime and its
// footer. Pure, so the host tests check the same decisions the device makes.
namespace oslogger::cadence {
// Default sample period. Both released boards sample every 10 s and write
// footer sample_interval_ms "10000".
inline constexpr std::uint32_t kDefaultSampleIntervalMs = 10000;
// RAM batch rows, one row group each. Sizes the PSRAM WriterState array and
// is written as footer row_group_rows_max.
inline constexpr std::size_t kMaxBatchRows = 90;
// Every rotation the runtime accepts (600, 900, 1800 and 3600 s) is a whole
// multiple of 300 s. A period that divides it keeps every window whole.
inline constexpr std::uint32_t kRotationQuantumMs = 300000;
inline constexpr std::uint32_t kMinSampleIntervalMs = 1000;

[[nodiscard]] constexpr bool valid_sample_interval(std::uint32_t ms) {
  return ms >= kMinSampleIntervalMs && ms <= kRotationQuantumMs &&
         kRotationQuantumMs % ms == 0;
}

// Rows per row group for one rotation window, at most kMaxBatchRows so the
// loss window never exceeds the RAM batch. At 10 s, 600 s gives 60 x 1,
// 900 s 90 x 1, 1800 s 90 x 2 and 3600 s 90 x 4.
[[nodiscard]] constexpr std::size_t rows_per_group(std::uint32_t rotation_s,
                                                   std::uint32_t sample_ms) {
  if (sample_ms == 0)
    return 1;
  const auto rows = std::uint64_t{rotation_s} * 1000 / sample_ms;
  return rows < 1               ? 1
         : rows < kMaxBatchRows ? static_cast<std::size_t>(rows)
                                : kMaxBatchRows;
}

// The sampling deadline period. begin() derives it once from
// Config::sample_interval_ms, and poll() and the footer read the same value.
[[nodiscard]] constexpr std::int64_t period_us(std::uint32_t sample_ms) {
  return std::int64_t{sample_ms} * 1000;
}

// One poll of the monotonic sampling deadline. Not due before next. Once due,
// whole periods that passed unsampled are skipped and counted, the sample is
// stamped with the latest deadline at or before now, and the following
// deadline is one period later. A period of 0 or less is never due.
struct Step {
  bool due;
  std::int64_t skipped;
  std::int64_t deadline_us;
  std::int64_t next_us;
};

[[nodiscard]] constexpr Step advance(std::int64_t now_us, std::int64_t next_us,
                                     std::int64_t period) {
  if (period <= 0 || now_us < next_us)
    return {
        .due = false, .skipped = 0, .deadline_us = next_us, .next_us = next_us};
  const auto skipped = (now_us - next_us) / period;
  const auto deadline = next_us + skipped * period;
  return {.due = true,
          .skipped = skipped,
          .deadline_us = deadline,
          .next_us = deadline + period};
}
} // namespace oslogger::cadence
