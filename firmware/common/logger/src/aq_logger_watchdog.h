#pragma once

#include <cstdint>

// Pure boot-loop guard for the storage-worker watchdog. The record lives in
// RTC no-init memory on the board, survives a task-watchdog panic reset and is
// lost on power-on, so the magic and check word reject power-on garbage.
namespace aqlogger::watchdog {
constexpr std::uint32_t kMagic = 0x41515744; // "AQWD"
// After this many stall resets without a healthy period between them the
// watchdog stops resetting and only warns.
constexpr std::uint32_t kMaxConsecutive = 3;

enum class Mode : std::uint8_t { Armed, WarnOnly };

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

// Clears the count once the worker has stayed live for healthy_s since the
// later of boot and its last expired stall. Returns true when it cleared.
inline bool clear_if_healthy(Guard &guard, std::int64_t now_us,
                             std::int64_t healthy_since_us,
                             std::uint32_t healthy_s) {
  if (!valid(guard) || guard.consecutive == 0 || now_us < healthy_since_us ||
      now_us - healthy_since_us <
          static_cast<std::int64_t>(healthy_s) * 1000000LL)
    return false;
  store(guard, 0, guard.pending);
  return true;
}
} // namespace aqlogger::watchdog
