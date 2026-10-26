#pragma once

#include <cstddef>
#include <cstdint>

namespace oslogger {
// Dated rows use UTC seconds / rotation. Unsynced rows have no UTC, so they
// rotate on monotonic seconds / rotation within one boot and clock epoch.
constexpr std::int64_t rotation_window(std::int64_t seconds,
                                       std::int64_t rotation_seconds) {
  return rotation_seconds > 0 ? seconds / rotation_seconds : 0;
}

constexpr bool same_rotation_window(std::int32_t epoch, bool dated,
                                    std::int64_t window,
                                    std::int32_t reference_epoch,
                                    bool reference_dated,
                                    std::int64_t reference_window) {
  return epoch == reference_epoch && dated == reference_dated &&
         window == reference_window;
}

// Row count is a RAM/row-group bound, not a UTC file boundary. An offset
// refresh near a boundary can leave one extra sample in the same window.
constexpr bool finalize_after_group(bool explicit_finalize,
                                    std::size_t row_groups,
                                    std::size_t maximum_row_groups) {
  return explicit_finalize || row_groups >= maximum_row_groups;
}
} // namespace oslogger
