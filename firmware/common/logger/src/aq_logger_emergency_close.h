#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

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

// What one worker pass did.
struct Pass {
  bool attempted = false;
  CloseOutcome outcome = CloseOutcome::NothingOpen;
  Next next = Next::Done;
};

// Worker only. Runs the attempt when one is due, against the worker's view
// of the card.
template <class Storage>
Pass service(EmergencyClose &close, Storage &storage, std::int64_t now_ms) {
  Pass pass;
  if (!close.due(now_ms))
    return pass;
  pass.attempted = true;
  pass.outcome = attempt_close(storage);
  pass.next = close.finish(pass.outcome, now_ms);
  return pass;
}

// The log line for a pass, after finish(). `files` counts the telemetry
// files finalized during the attempt, including one a storage probe in
// writable() finalized before the close itself wrote.
//   closed     PARQUET FLUSH reason=power-critical result=closed attempt=<n>
//              files=<f>
//   retry      PARQUET FLUSH reason=power-critical result=<outcome>
//              storage=<state> attempt=<n> files=<f> retry_ms=<ms>
//   gave up    PARQUET ERROR operation=power-close reason=<outcome>
//              attempts=<n> files=<f> closed=0
// nothing-open is quiet on a first attempt. After a retry it logs
// result=nothing-open, because an earlier probe may have closed the file.
// Returns the length without the NUL, zero when nothing is logged or the
// line does not fit.
inline std::size_t format_pass(char *out, std::size_t capacity,
                               const Pass &pass, const EmergencyClose &close,
                               const char *storage_state, unsigned files,
                               std::int64_t now_ms) {
  if (!out || capacity == 0)
    return 0;
  out[0] = '\0';
  if (!pass.attempted)
    return 0;
  const unsigned attempts = close.attempts();
  const char *outcome = outcome_name(pass.outcome);
  int written = 0;
  if (pass.next == Next::GaveUp) {
    written = std::snprintf(out, capacity,
                            "PARQUET ERROR operation=power-close reason=%s "
                            "attempts=%u files=%u closed=0",
                            outcome, attempts, files);
  } else if (pass.next == Next::Retry) {
    written = std::snprintf(
        out, capacity,
        "PARQUET FLUSH reason=power-critical result=%s storage=%s "
        "attempt=%u files=%u retry_ms=%lld",
        outcome, storage_state ? storage_state : "unknown", attempts, files,
        static_cast<long long>(close.retry_at_ms() - now_ms));
  } else if (pass.outcome == CloseOutcome::Closed ||
             (pass.outcome == CloseOutcome::NothingOpen && attempts > 1)) {
    written = std::snprintf(out, capacity,
                            "PARQUET FLUSH reason=power-critical result=%s "
                            "attempt=%u files=%u",
                            outcome, attempts, files);
  }
  if (written <= 0 || static_cast<std::size_t>(written) >= capacity) {
    out[0] = '\0';
    return 0;
  }
  return static_cast<std::size_t>(written);
}

} // namespace aqlogger::power
