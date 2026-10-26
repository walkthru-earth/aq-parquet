#pragma once

#include "aq_logger_watchdog.h"

#include <cstdint>

// The steps aq_logger.cpp takes to start the supervisor and to apply each of
// its decisions, written against injected interfaces so the host tests run
// this exact code. No ESP-IDF headers.
//
// Twdt wraps the ESP-IDF task watchdog for the worker's user.
//   bool add_user()             esp_task_wdt_add_user, keeps the handle
//   bool reset_user()           esp_task_wdt_reset_user on that handle
//   bool reconfigure(bool panic) esp_task_wdt_reconfigure, this panic flag
//   void delete_user()          esp_task_wdt_delete_user, drops the handle
// Timer wraps the periodic esp_timer that runs tick().
//   bool create()  bool start()  void destroy()
namespace oslogger::watchdog {

// Applies one decision of supervise(). Returns false when the task watchdog
// refused it, so the caller can unwind with failed(). default_panic is the
// panic flag the IDF config started with.
template <class Twdt>
bool apply(Twdt &twdt, Action action, bool default_panic) {
  switch (action) {
  case Action::Nothing:
    return true;
  case Action::Arm:
    if (!twdt.add_user())
      return false;
    twdt.reset_user();
    return true;
  case Action::Feed:
    return twdt.reset_user();
  case Action::Trip:
    return twdt.reconfigure(true);
  case Action::Recover: {
    const bool restored = twdt.reconfigure(default_panic);
    twdt.reset_user();
    return restored;
  }
  }
  return false;
}

struct TickResult {
  Action action;
  bool cleared;   // the healthy period cleared the reset count
  bool recovered; // a declared stall was withdrawn and the config restored
};

// One supervisor tick: decide, apply, unwind a refused action.
template <class Twdt>
TickResult tick(Supervisor &state, Guard &guard, Twdt &twdt,
                std::int64_t now_us, std::int64_t heartbeat_us,
                std::uint32_t timeout_s, std::uint32_t healthy_s,
                bool default_panic) {
  const Step step =
      supervise(state, guard, now_us, heartbeat_us, timeout_s, healthy_s);
  TickResult result{step.action, step.cleared, false};
  if (!apply(twdt, step.action, default_panic))
    failed(state, guard, step.action);
  else
    result.recovered = step.action == Action::Recover;
  return result;
}

struct Boot {
  Mode mode;
  bool stall_reset; // this boot follows a stall reset this firmware armed
  bool supervised;  // the timer exists, and after launch() it runs
  bool armed;       // the worker's task watchdog user is registered
};

// First half of the start, before the timer runs. Reads the guard, creates
// the timer before arming so no user is armed without a feeder, and hands
// the supervisor state to the timer. Publish anything the timer callback or
// poll() reads before launch().
template <class Twdt, class Timer>
Boot prepare(Supervisor &state, Guard &guard, Twdt &twdt, Timer &timer,
             bool task_watchdog_reset, std::int64_t now_us) {
  Boot boot{};
  boot.stall_reset = valid(guard) && guard.pending && task_watchdog_reset;
  boot.mode = on_boot(guard, task_watchdog_reset);
  boot.supervised = timer.create();
  boot.armed = boot.supervised && boot.mode == Mode::Armed && twdt.add_user();
  start(state, now_us, boot.armed);
  return boot;
}

// Second half. Starts the timer and, when it does not run, deletes the timer
// and the user again so the board never resets for want of a feeder.
template <class Twdt, class Timer>
void launch(Boot &boot, Supervisor &state, Twdt &twdt, Timer &timer) {
  if (boot.supervised && !timer.start()) {
    boot.supervised = false;
    timer.destroy();
  }
  if (!boot.supervised && boot.armed) {
    twdt.delete_user();
    boot.armed = false;
    state.armed = false;
  }
}
} // namespace oslogger::watchdog
