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

ossync::EnqueueResult NotifyCredits::take(std::uint32_t generation,
                                          std::int64_t now_us) {
  if (!open_for(generation))
    return ossync::EnqueueResult::Closed;
  if (!available_)
    return ossync::EnqueueResult::Full;
  // The stall clock starts when the first frame of a burst goes out, so a
  // long idle spell before it never counts against the host.
  if (available_ == capacity_)
    progress_us_ = now_us;
  --available_;
  return ossync::EnqueueResult::Ok;
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

void ResponseQueue::configure(std::uint8_t slots,
                              std::uint32_t progress_timeout_ms) {
  if (slots > kMaxNotifyCredits)
    slots = kMaxNotifyCredits;
  credits_.configure(slots, progress_timeout_ms);
  clear();
}

void ResponseQueue::clear() {
  head_ = 0;
  queued_ = 0;
  ++epoch_;
}

void ResponseQueue::open(std::uint32_t generation, std::int64_t now_us) {
  credits_.open(generation, now_us);
  clear();
}

void ResponseQueue::close() {
  credits_.close();
  clear();
}

ResponseQueue::Reservation ResponseQueue::reserve(std::uint32_t generation,
                                                  std::int64_t now_us) {
  Reservation reservation;
  reservation.result = credits_.take(generation, now_us);
  // The credit keeps this slot outside the queued range until it is
  // published, so the host never reads it while it is filled.
  reservation.slot = static_cast<std::uint8_t>((head_ + queued_) % capacity());
  reservation.epoch = epoch_;
  return reservation;
}

bool ResponseQueue::publish(const Reservation &reservation,
                            std::uint32_t generation, std::uint16_t length,
                            std::uint16_t handle) {
  // Pops move head and queued together, so within one epoch the next free
  // slot is still the reserved one.
  if (reservation.result != ossync::EnqueueResult::Ok ||
      reservation.epoch != epoch_ || !credits_.open_for(generation) ||
      queued_ >= capacity() ||
      reservation.slot != (head_ + queued_) % capacity())
    return false;
  slots_[reservation.slot] = {length, handle, generation};
  ++queued_;
  return true;
}

bool ResponseQueue::front(TxSlot &slot, std::uint8_t &index) const {
  if (!queued_)
    return false;
  index = head_;
  slot = slots_[head_];
  return true;
}

ResponseQueue::Outcome ResponseQueue::on_notify_tx(std::uint32_t session,
                                                   NotifyStatus status,
                                                   std::int64_t now_us) {
  Outcome outcome;
  outcome.current = credits_.on_notify_tx(session, status, now_us);
  if (!outcome.current || status == NotifyStatus::Failed) {
    clear();
  } else if (status == NotifyStatus::Sent && queued_) {
    head_ = static_cast<std::uint8_t>((head_ + 1) % capacity());
    --queued_;
  }
  outcome.stalled = outcome.current && status == NotifyStatus::Refused &&
                    credits_.stalled(now_us);
  return outcome;
}

StallClose::Action StallClose::on_refused(std::uint32_t session,
                                          bool stalled) const {
  if (!stalled)
    return Action::Retry;
  return session == closing_ ? Action::Wait : Action::Close;
}

bool StallClose::log(std::uint32_t session) {
  if (session == logged_)
    return false;
  logged_ = session;
  return true;
}

StallClose::Action StallClose::on_close(std::uint32_t session,
                                        CloseStart start) {
  if (start == CloseStart::Failed)
    return Action::Retry;
  closing_ = session;
  return Action::Wait;
}

bool FlagWatch::on_publish(std::uint8_t flags) {
  const bool flip = seen_ && flags != flags_;
  seen_ = true;
  flags_ = flags;
  return flip;
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
