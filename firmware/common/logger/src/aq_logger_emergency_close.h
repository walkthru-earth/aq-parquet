#pragma once

#include <atomic>
#include <cstdint>

// Emergency close of the open Parquet file on a critical battery. Any task
// may raise the intent, only the storage worker runs it, so the card keeps
// a single accessor. No IO, no clocks, no ESP-IDF headers, so the host tests
// run this exact code.
namespace aqlogger::power {

// Result of one close attempt on the worker.
enum class CloseOutcome : std::uint8_t {
  Closed,      // buffered rows and the open file were finalized
  NothingOpen, // no rows buffered and no file open, nothing to preserve
  BusyRetry,   // storage is not writable yet, nothing was written
  Failed,      // the finalize write failed
};

inline const char *outcome_name(CloseOutcome outcome) {
  switch (outcome) {
  case CloseOutcome::Closed:
    return "closed";
  case CloseOutcome::NothingOpen:
    return "nothing-open";
  case CloseOutcome::BusyRetry:
    return "busy-retry";
  case CloseOutcome::Failed:
    return "failed";
  }
  return "unknown";
}

// One attempt against the worker's view of the card.
//   bool has_data()  rows are buffered or a file is open
//   bool writable()  probes storage now, true when a write may go ahead
//   bool finalize()  finishes the open file with the buffered rows
template <class Storage> CloseOutcome attempt_close(Storage &storage) {
  if (!storage.has_data())
    return CloseOutcome::NothingOpen;
  if (!storage.writable())
    return CloseOutcome::BusyRetry;
  return storage.finalize() ? CloseOutcome::Closed : CloseOutcome::Failed;
}

// Retry bounds. The defaults back off 1, 2, 4, 8, 16, 30 and 30 s, about 91 s
// across eight attempts, before giving up. Calibrate against the measured
// time from critical to cutoff (#28).
struct RetryPolicy {
  std::uint8_t max_attempts = 8;
  std::int64_t first_backoff_ms = 1000;
  std::int64_t max_backoff_ms = 30000;
};

// What the worker does after an attempt.
enum class Next : std::uint8_t {
  Done,   // closed or nothing to preserve, the intent is cleared
  Retry,  // the intent stays, the next attempt is due at retry_at_ms()
  GaveUp, // attempts exhausted, the intent is cleared and the loss is logged
};

class EmergencyClose {
public:
  explicit EmergencyClose(const RetryPolicy &policy = RetryPolicy{})
      : policy_(policy) {}

  // Any task. Idempotent while an intent is pending.
  void request() { requested_.store(true, std::memory_order_release); }

  // Worker only. Folds a new request into the worker state and reports
  // whether an attempt is due now. Call it at each safe IO boundary.
  bool due(std::int64_t now_ms) {
    if (requested_.exchange(false, std::memory_order_acq_rel) && !active_) {
      active_ = true;
      attempts_ = 0;
      retry_at_ms_ = now_ms;
    }
    return active_ && now_ms >= retry_at_ms_;
  }

  // Worker only. True when a transfer should stop at its next frame so the
  // worker can run a due attempt. Never consumes the request.
  bool preempt(std::int64_t now_ms) const {
    if (requested_.load(std::memory_order_acquire))
      return !active_ || now_ms >= retry_at_ms_;
    return active_ && now_ms >= retry_at_ms_;
  }

  // Worker only. Records the outcome of an attempt that due() allowed.
  Next finish(CloseOutcome outcome, std::int64_t now_ms) {
    if (!active_)
      return Next::Done;
    ++attempts_;
    if (outcome == CloseOutcome::Closed ||
        outcome == CloseOutcome::NothingOpen) {
      active_ = false;
      return Next::Done;
    }
    if (attempts_ >= policy_.max_attempts) {
      active_ = false;
      return Next::GaveUp;
    }
    std::int64_t backoff = policy_.first_backoff_ms;
    for (std::uint8_t i = 1; i < attempts_ && backoff < policy_.max_backoff_ms;
         ++i)
      backoff *= 2;
    if (backoff > policy_.max_backoff_ms)
      backoff = policy_.max_backoff_ms;
    retry_at_ms_ = now_ms + backoff;
    return Next::Retry;
  }

  bool active() const { return active_; }
  std::uint8_t attempts() const { return attempts_; }
  std::int64_t retry_at_ms() const { return retry_at_ms_; }

private:
  RetryPolicy policy_;
  std::atomic<bool> requested_{false};
  // Worker owned.
  bool active_ = false;
  std::uint8_t attempts_ = 0;
  std::int64_t retry_at_ms_ = 0;
};

} // namespace aqlogger::power
