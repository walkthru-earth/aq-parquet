#pragma once

#include <cstddef>
#include <cstdint>

// Sampling cadence and RAM batch bounds shared by the logger runtime and its
// footer. Pure, so the host tests check the same decisions the device makes.
namespace aqlogger::cadence {
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
} // namespace aqlogger::cadence
