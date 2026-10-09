#pragma once

#include <cstddef>
#include <cstdint>

namespace aqlogger {
constexpr bool same_rotation_window(std::int32_t epoch, bool dated,
                                    std::int64_t window,
                                    std::int32_t reference_epoch,
                                    bool reference_dated,
                                    std::int64_t reference_window) {
  return epoch == reference_epoch && dated == reference_dated &&
         (!dated || window == reference_window);
}

// Row count is a RAM/row-group bound, not a UTC file boundary. An offset
// refresh near a boundary can leave one extra sample in the same window.
constexpr bool finalize_after_group(bool explicit_finalize,
                                    std::size_t row_groups,
                                    std::size_t maximum_row_groups) {
  return explicit_finalize || row_groups >= maximum_row_groups;
}
} // namespace aqlogger
