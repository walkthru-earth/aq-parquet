#include <aq_logger_rotation.h>
#include <utc_clock.h>

#include <cassert>
#include <cstdint>
#include <initializer_list>

int main() {
  using namespace aqlogger;
  assert(same_rotation_window(2, true, 1, 2, true, 1));
  assert(!same_rotation_window(3, true, 1, 2, true, 1));
  assert(!same_rotation_window(2, true, 2, 2, true, 1));
  assert(!same_rotation_window(1, true, 0, 0, false, 0));
  assert(same_rotation_window(0, false, 7, 0, false, 0));
  assert(finalize_after_group(true, 1, 8));
  assert(finalize_after_group(false, 8, 8));
  for (std::size_t groups = 0; groups < 8; ++groups)
    assert(!finalize_after_group(false, groups, 8));

  // Repeated small refreshes of an already external clock retain one window
  // even when a two-second backward correction lets a 91st/361st row fit.
  for (const std::int64_t interval : {900, 3600}) {
    std::int32_t epoch = 1;
    std::size_t groups = 0, buffered = 0, finalized = 0;
    constexpr auto group_rows = 90U;
    for (std::int64_t mono_seconds = 0; mono_seconds <= interval;
         mono_seconds += 10) {
      const auto correction = mono_seconds >= interval / 2 ? -2 : 0;
      if (mono_seconds == interval / 2 || mono_seconds % 60 == 0)
        epoch += aq::utc::starts_new_epoch(aq::utc::Host, aq::utc::Host,
                                           correction * 1000000000LL);
      const auto window = (mono_seconds + correction) / interval;
      assert(same_rotation_window(epoch, true, window, 1, true, 0));
      if (++buffered == group_rows) {
        ++groups;
        buffered = 0;
        finalized += finalize_after_group(false, groups, 8);
      }
    }
    assert(finalized == 0 && buffered == 1);
    assert(!same_rotation_window(epoch, true, 1, 1, true, 0));
  }
}
