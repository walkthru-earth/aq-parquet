#include "ble_policy.h"

namespace ble {
void NotifyCredits::configure(std::uint8_t slots,
                              std::uint32_t progress_timeout_ms) {
  capacity_ = slots ? slots : 1;
  progress_timeout_us_ = std::int64_t(progress_timeout_ms) * 1000;
  close();
}

void NotifyCredits::open(std::uint32_t generation, std::int64_t now_us) {
  generation_ = generation;
  available_ = capacity_;
  progress_us_ = now_us;
  open_ = true;
}

void NotifyCredits::close() {
  open_ = false;
  available_ = capacity_;
}

aqsync::EnqueueResult NotifyCredits::take(std::uint32_t generation,
                                          std::int64_t now_us) {
  if (!open_for(generation))
    return aqsync::EnqueueResult::Closed;
  if (!available_)
    return aqsync::EnqueueResult::Full;
  // The stall clock starts when the first frame of a burst goes out, so a
  // long idle spell before it never counts against the host.
  if (available_ == capacity_)
    progress_us_ = now_us;
  --available_;
  return aqsync::EnqueueResult::Ok;
}

void NotifyCredits::cancel(std::uint32_t generation) {
  if (open_for(generation) && available_ < capacity_)
    ++available_;
}

bool NotifyCredits::on_notify_tx(std::uint32_t generation, NotifyStatus status,
                                 std::int64_t now_us) {
  if (!open_for(generation))
    return false;
  switch (status) {
  case NotifyStatus::Sent:
    if (available_ < capacity_)
      ++available_;
    progress_us_ = now_us;
    break;
  case NotifyStatus::Refused:
    break;
  case NotifyStatus::Failed:
    close();
    break;
  }
  return true;
}

bool NotifyCredits::stalled(std::int64_t now_us) const {
  return open_ && available_ < capacity_ &&
         now_us - progress_us_ >= progress_timeout_us_;
}

void AdvertWindow::extend(std::int64_t now_us, std::uint32_t window_ms) {
  const std::int64_t until = now_us + std::int64_t(window_ms) * 1000;
  if (until > fast_until_us_)
    fast_until_us_ = until;
}

std::uint32_t AdvertWindow::remaining_ms(std::int64_t now_us) const {
  if (!fast(now_us))
    return 0;
  return static_cast<std::uint32_t>((fast_until_us_ - now_us + 999) / 1000);
}

void IntervalPolicy::connect(std::int64_t now_us) {
  last_us_ = now_us;
  relaxed_ = false;
}

IntervalPolicy::Action IntervalPolicy::on_request(std::int64_t now_us) {
  last_us_ = now_us;
  if (!relaxed_)
    return Action::None;
  relaxed_ = false;
  return Action::RequestActive;
}

IntervalPolicy::Action IntervalPolicy::on_timer(std::int64_t now_us,
                                                std::uint32_t relax_after_ms,
                                                std::uint32_t &rearm_ms) {
  rearm_ms = 0;
  if (relaxed_)
    return Action::None;
  const std::int64_t due = last_us_ + std::int64_t(relax_after_ms) * 1000;
  if (now_us < due) {
    rearm_ms = static_cast<std::uint32_t>((due - now_us + 999) / 1000);
    return Action::None;
  }
  relaxed_ = true;
  return Action::RequestRelaxed;
}
} // namespace ble
