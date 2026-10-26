#pragma once

#include <cstdint>

// Pure storage-worker watchdog logic. It has no ESP-IDF headers so the host
// tests can drive it with a fake clock. The board side in os_logger.cpp owns
// the task-watchdog user, the supervisor timer and the RTC record, and only
// applies the actions decided here.
namespace oslogger::watchdog {
constexpr std::uint32_t kMagic = 0x41515744; // "AQWD"
// After this many stall resets without a healthy period between them the
// watchdog stops resetting and only warns.
constexpr std::uint32_t kMaxConsecutive = 3;

// True when the worker heartbeat is older than timeout_s (clamped to >= 5 s).
// No heartbeat yet means the worker has not started and is never expired.
inline bool stall_expired(std::int64_t now_us, std::int64_t heartbeat_us,
                          std::uint32_t timeout_s) {
  if (heartbeat_us <= 0 || now_us <= heartbeat_us || timeout_s == 0)
    return false;
  const std::int64_t timeout_us =
      static_cast<std::int64_t>(timeout_s < 5U ? 5U : timeout_s) * 1000000LL;
  return now_us - heartbeat_us > timeout_us;
}

enum class Mode : std::uint8_t { Armed, WarnOnly };

// Boot-loop guard. The record lives in RTC no-init memory on the board,
// survives a task-watchdog panic reset and is lost on power-on, so the magic
// and check word reject power-on garbage.
struct Guard {
  std::uint32_t magic;
  std::uint32_t consecutive;
  std::uint32_t pending; // 1 while a stall reset has been armed
  std::uint32_t check;
};

inline std::uint32_t check_word(const Guard &guard) {
  return ~(guard.magic ^ (guard.consecutive * 0x9E3779B1U) ^
           (guard.pending << 31));
}

inline bool valid(const Guard &guard) {
  return guard.magic == kMagic && guard.consecutive <= kMaxConsecutive &&
         guard.pending <= 1 && guard.check == check_word(guard);
}

inline void store(Guard &guard, std::uint32_t consecutive,
                  std::uint32_t pending) {
  guard.magic = kMagic;
  guard.consecutive = consecutive;
  guard.pending = pending;
  guard.check = check_word(guard);
}

inline Mode mode(const Guard &guard) {
  return valid(guard) && guard.consecutive >= kMaxConsecutive ? Mode::WarnOnly
                                                              : Mode::Armed;
}

// Called once per boot. A stall reset counts only when this firmware armed it
// (pending) and the chip reports a task-watchdog reset. Any other reset keeps
// the count, so only a healthy period clears it.
inline Mode on_boot(Guard &guard, bool task_watchdog_reset) {
  if (!valid(guard))
    store(guard, 0, 0);
  std::uint32_t consecutive = guard.consecutive;
  if (guard.pending && task_watchdog_reset && consecutive < kMaxConsecutive)
    ++consecutive;
  store(guard, consecutive, 0);
  return mode(guard);
}

// Marks the reset this firmware is about to cause.
inline void mark_pending(Guard &guard) {
  if (!valid(guard))
    store(guard, 0, 0);
  store(guard, guard.consecutive, 1);
}

// Withdraws a pending stall once the worker recovers or the panic could not
// be armed, so a later unrelated task-watchdog reset is not counted.
inline void clear_pending(Guard &guard) {
  if (!valid(guard))
    store(guard, 0, 0);
  store(guard, guard.consecutive, 0);
}

// Clears the count once the worker has stayed live for healthy_s since the
// later of boot and its last expired stall. Returns true when it cleared.
// A healthy period also means no stall is pending.
inline bool clear_if_healthy(Guard &guard, std::int64_t now_us,
                             std::int64_t healthy_since_us,
                             std::uint32_t healthy_s) {
  if (!valid(guard) || guard.consecutive == 0 || now_us < healthy_since_us ||
      now_us - healthy_since_us <
          static_cast<std::int64_t>(healthy_s) * 1000000LL)
    return false;
  store(guard, 0, 0);
  return true;
}

// One supervisor tick, decided by supervise() and applied by the board.
// - Nothing leaves the task watchdog alone.
// - Feed resets the worker's task-watchdog user on its behalf.
// - Arm registers the worker's task-watchdog user, then feeds it.
// - Trip stops feeding and switches the task watchdog to panic.
// - Recover restores the original task-watchdog config, then feeds.
enum class Action : std::uint8_t { Nothing, Feed, Arm, Trip, Recover };

// Supervisor state, owned by the supervisor timer after start.
struct Supervisor {
  std::int64_t healthy_since_us = 0; // later of start and the last expiry
  bool armed = false;   // the worker's task-watchdog user is registered
  bool tripped = false; // a stall is declared and panic was requested
  bool restore = false; // the original config still has to be restored
};

struct Step {
  Action action;
  bool cleared; // the healthy period just cleared the reset count
};

inline void start(Supervisor &state, std::int64_t now_us, bool armed) {
  state = Supervisor{};
  state.healthy_since_us = now_us;
  state.armed = armed;
}

// Decides one tick from the clock, the last worker heartbeat and the state.
// The state and guard change as if the action succeeds. When applying it
// fails the board calls failed() to unwind.
inline Step supervise(Supervisor &state, Guard &guard, std::int64_t now_us,
                      std::int64_t heartbeat_us, std::uint32_t timeout_s,
                      std::uint32_t healthy_s) {
  const bool expired = stall_expired(now_us, heartbeat_us, timeout_s);
  Step step{Action::Nothing, false};
  if (expired)
    state.healthy_since_us = now_us;
  else
    step.cleared =
        clear_if_healthy(guard, now_us, state.healthy_since_us, healthy_s);
  if (!state.armed) {
    if (!expired && mode(guard) == Mode::Armed) {
      state.armed = true;
      step.action = Action::Arm;
    }
    return step;
  }
  if (expired) {
    // Once tripped, stay silent and let the task watchdog fire.
    if (!state.tripped) {
      state.tripped = true;
      state.restore = false;
      mark_pending(guard);
      step.action = Action::Trip;
    }
    return step;
  }
  if (state.tripped || state.restore) {
    // The worker moved again before the task watchdog fired.
    state.tripped = false;
    state.restore = false;
    clear_pending(guard);
    step.action = Action::Recover;
    return step;
  }
  step.action = Action::Feed;
  return step;
}

// Unwinds an action the board could not apply. A failed Arm retries on the
// next live tick. A failed Trip withdraws the pending stall and retries on the
// next tick. A failed Recover keeps feeding and retries the restore.
inline void failed(Supervisor &state, Guard &guard, Action action) {
  switch (action) {
  case Action::Arm:
    state.armed = false;
    break;
  case Action::Trip:
    state.tripped = false;
    clear_pending(guard);
    break;
  case Action::Recover:
    state.restore = true;
    break;
  case Action::Nothing:
  case Action::Feed:
    break;
  }
}
} // namespace oslogger::watchdog
