// Fake-clock tests for the storage-worker watchdog supervisor. The rig mirrors
// start_worker_watchdog(), supervise_worker() and stamp_heartbeat() in
// aq_logger.cpp over a fake task watchdog with one user and one period.
#include "aq_logger_watchdog.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace wd = aqlogger::watchdog;

namespace {
constexpr std::int64_t kSecond = 1000000LL;
constexpr std::uint32_t kTimeoutS = 60;
constexpr std::uint32_t kHealthyS = 3600;
constexpr std::int64_t kTwdtPeriodUs = 5 * kSecond; // ESP_TASK_WDT_TIMEOUT_S

// One task-watchdog user with the IDF timing rule. With panic the first
// unfed period resets the chip, without it the IDF only prints.
struct FakeTwdt {
  bool user = false;
  bool panic = false; // the original config is print-only
  std::int64_t fed_us = 0;
  int prints = 0;
  bool fail_add = false;
  bool fail_reconfigure = false;

  void feed(std::int64_t now) {
    if (user)
      fed_us = now;
  }
};

struct Rig {
  std::int64_t now = 0;
  std::int64_t heartbeat = 0;
  wd::Guard guard{0xdeadbeef, 7, 9, 1}; // power-on garbage
  wd::Supervisor state;
  FakeTwdt twdt;
  bool supervised = false;
  bool fail_timer_create = false;
  bool fail_timer_start = false;
  bool reset = false; // the chip reset through the task watchdog
  int trips = 0;      // since boot
  int recoveries = 0; // since boot
  int clears = 0;

  // stamp_heartbeat(): confirmed worker progress, which also feeds the user.
  void stamp() {
    heartbeat = now;
    twdt.feed(now);
  }

  bool arm() {
    if (twdt.fail_add)
      return false;
    twdt.user = true;
    twdt.fed_us = now;
    return true;
  }

  // apply_watchdog().
  bool apply(wd::Action action) {
    switch (action) {
    case wd::Action::Nothing:
      return true;
    case wd::Action::Arm:
      return arm();
    case wd::Action::Feed:
      twdt.feed(now);
      return true;
    case wd::Action::Trip:
      if (twdt.fail_reconfigure)
        return false;
      twdt.panic = true;
      return true;
    case wd::Action::Recover: {
      const bool restored = !twdt.fail_reconfigure;
      if (restored)
        twdt.panic = false;
      twdt.feed(now);
      return restored;
    }
    }
    return false;
  }

  // start_worker_watchdog() after a reset of the given kind.
  wd::Mode boot(bool task_watchdog_reset) {
    now = 0;
    heartbeat = 0;
    reset = false;
    trips = 0;
    recoveries = 0;
    // A reset drops every user and the panic switch, not the injected faults.
    FakeTwdt fresh;
    fresh.fail_add = twdt.fail_add;
    fresh.fail_reconfigure = twdt.fail_reconfigure;
    twdt = fresh;
    const auto mode = wd::on_boot(guard, task_watchdog_reset);
    supervised = !fail_timer_create;
    bool armed = supervised && mode == wd::Mode::Armed && arm();
    wd::start(state, now, armed);
    if (supervised && fail_timer_start)
      supervised = false;
    if (!supervised && armed) {
      twdt.user = false; // disarm_worker_watchdog()
      armed = false;
    }
    state.armed = armed;
    stamp(); // the worker's first loop pass
    return mode;
  }

  // supervise_worker().
  void tick() {
    const auto step =
        wd::supervise(state, guard, now, heartbeat, kTimeoutS, kHealthyS);
    clears += step.cleared ? 1 : 0;
    if (!apply(step.action))
      wd::failed(state, guard, step.action);
    else if (step.action == wd::Action::Trip)
      ++trips;
    else if (step.action == wd::Action::Recover)
      ++recoveries;
  }

  // Advances the fake clock in 100 ms steps. The supervisor ticks every
  // second while supervised, and the worker stamps every stamp_us (0 never).
  void run(std::int64_t duration_us, std::int64_t stamp_us) {
    const std::int64_t end = now + duration_us;
    std::int64_t next_stamp = stamp_us ? now + stamp_us : end + 1;
    while (now < end && !reset) {
      now += kSecond / 10;
      if (now >= next_stamp) {
        stamp();
        next_stamp += stamp_us;
      }
      if (supervised && now % kSecond == 0)
        tick();
      if (twdt.user && now - twdt.fed_us >= kTwdtPeriodUs) {
        if (twdt.panic)
          reset = true;
        else {
          ++twdt.prints;
          twdt.fed_us = now;
        }
      }
    }
  }
};

void healthy_worker_only_feeds() {
  Rig rig;
  assert(rig.boot(false) == wd::Mode::Armed);
  assert(rig.supervised && rig.twdt.user && rig.state.armed);
  rig.run(2 * 3600 * kSecond, kSecond);
  assert(!rig.reset && rig.trips == 0 && rig.twdt.prints == 0);
  assert(!rig.twdt.panic && rig.guard.pending == 0);
}

// Issue #32. The latch used to stay set after the first stall, so the second
// stall never fed and reset with panic still on.
void transient_stalls_recover_each_time() {
  Rig rig;
  rig.boot(false);
  rig.run(10 * kSecond, kSecond);
  for (int round = 1; round <= 2; ++round) {
    rig.run(62 * kSecond, 0); // silent past the 60 s timeout
    assert(rig.trips == round && !rig.reset);
    assert(rig.twdt.panic && rig.guard.pending == 1 && rig.state.tripped);
    rig.stamp(); // the worker moves before the next period ends
    rig.run(kSecond, kSecond);
    assert(rig.recoveries == round && !rig.reset);
    assert(!rig.state.tripped && !rig.twdt.panic && rig.guard.pending == 0);
    rig.run(30 * kSecond, kSecond);
    assert(!rig.reset && rig.twdt.prints == 0);
  }
  // A recovered stall is not counted. A later idle-task reset is not either.
  assert(rig.boot(true) == wd::Mode::Armed && rig.guard.consecutive == 0);
  // A third stall that lasts resets within one period after the trip.
  rig.run(10 * kSecond, kSecond);
  const std::int64_t last = rig.heartbeat;
  rig.run(120 * kSecond, 0);
  assert(rig.reset && rig.trips == 1);
  assert(rig.now <= last + (kTimeoutS + 1) * kSecond + kTwdtPeriodUs);
  assert(rig.guard.pending == 1);
  assert(rig.boot(true) == wd::Mode::Armed && rig.guard.consecutive == 1);
}

// Three stall resets fall back to warn-only. A healthy hour re-arms it.
void repeated_stalls_hit_the_boot_loop_guard() {
  Rig rig;
  rig.boot(false);
  for (std::uint32_t n = 1; n <= wd::kMaxConsecutive; ++n) {
    rig.run(10 * kSecond, kSecond);
    rig.run(120 * kSecond, 0);
    assert(rig.reset);
    const auto mode = rig.boot(true);
    assert(rig.guard.consecutive == n);
    assert(mode ==
           (n < wd::kMaxConsecutive ? wd::Mode::Armed : wd::Mode::WarnOnly));
  }
  // Warn-only has no user, so a stall can never reset.
  assert(!rig.twdt.user && !rig.state.armed);
  rig.run(600 * kSecond, 0);
  assert(!rig.reset && rig.trips == 0 && rig.guard.pending == 0);
  // A plain reboot keeps the count.
  assert(rig.boot(false) == wd::Mode::WarnOnly);
  // Continuous liveness for the healthy period clears the count and re-arms.
  rig.run((kHealthyS - 1) * kSecond, kSecond);
  assert(rig.clears == 0 && !rig.state.armed);
  rig.run(2 * kSecond, kSecond);
  assert(rig.clears == 1 && rig.guard.consecutive == 0);
  assert(rig.state.armed && rig.twdt.user);
  // Re-armed: the next lasting stall resets and counts from one.
  rig.run(120 * kSecond, 0);
  assert(rig.reset && rig.boot(true) == wd::Mode::Armed);
  assert(rig.guard.consecutive == 1);
}

// A slow write whose blocks each land inside the timeout never trips, even
// when the whole group takes many times the timeout.
void slow_progressing_write_never_trips() {
  Rig rig;
  rig.boot(false);
  rig.run(600 * kSecond, 59 * kSecond);
  assert(!rig.reset && rig.trips == 0 && rig.twdt.prints == 0);
  rig.run(600 * kSecond, 20 * kSecond);
  assert(!rig.reset && rig.trips == 0 && rig.twdt.prints == 0);
}

// A sink call that never returns stamps nothing after it, so it trips and
// resets with the stall pending.
void sink_that_never_returns_trips() {
  Rig rig;
  rig.boot(false);
  rig.run(30 * kSecond, kSecond);
  const std::int64_t entered = rig.heartbeat;
  rig.run(600 * kSecond, 0);
  assert(rig.reset && rig.trips == 1 && rig.guard.pending == 1);
  assert(rig.now > entered + kTimeoutS * kSecond);
  assert(rig.now <= entered + (kTimeoutS + 1) * kSecond + kTwdtPeriodUs);
}

// A supervisor timer that cannot be created or started leaves no armed user,
// so the board never resets for want of a feeder.
void timer_failure_leaves_no_armed_user() {
  for (int start_fails = 0; start_fails <= 1; ++start_fails) {
    Rig rig;
    rig.fail_timer_create = start_fails == 0;
    rig.fail_timer_start = start_fails == 1;
    assert(rig.boot(false) == wd::Mode::Armed);
    assert(!rig.supervised && !rig.twdt.user && !rig.state.armed);
    rig.run(600 * kSecond, 0);
    assert(!rig.reset && rig.trips == 0 && rig.guard.pending == 0);
  }
}

// A refused panic switch withdraws the pending stall and retries. A refused
// restore keeps feeding and retries until the original config is back.
void reconfigure_failures_unwind() {
  Rig rig;
  rig.boot(false);
  rig.run(10 * kSecond, kSecond);
  rig.twdt.fail_reconfigure = true;
  rig.run(65 * kSecond, 0);
  assert(rig.trips == 0 && !rig.state.tripped && rig.guard.pending == 0);
  assert(!rig.reset && rig.twdt.prints > 0); // print-only, as configured
  rig.twdt.fail_reconfigure = false;
  rig.run(2 * kSecond, 0);
  assert(rig.trips == 1 && rig.twdt.panic && rig.guard.pending == 1);
  rig.twdt.fail_reconfigure = true;
  rig.stamp();
  rig.run(3 * kSecond, kSecond);
  assert(rig.recoveries == 0 && rig.state.restore && rig.guard.pending == 0);
  assert(!rig.reset); // still fed while the restore is retried
  rig.twdt.fail_reconfigure = false;
  rig.run(2 * kSecond, kSecond);
  assert(rig.recoveries == 1 && !rig.state.restore && !rig.twdt.panic);
  rig.run(600 * kSecond, kSecond);
  assert(!rig.reset);
}

// A refused add_user leaves warn-only and retries on later live ticks.
void arm_failure_retries() {
  Rig rig;
  rig.twdt.fail_add = true;
  rig.boot(false);
  assert(rig.supervised && !rig.state.armed && !rig.twdt.user);
  rig.run(5 * kSecond, kSecond);
  assert(!rig.state.armed);
  rig.twdt.fail_add = false;
  rig.run(2 * kSecond, kSecond);
  assert(rig.state.armed && rig.twdt.user);
  rig.run(600 * kSecond, kSecond);
  assert(!rig.reset && rig.trips == 0);
}

void guard_edges() {
  wd::Guard guard{};
  // clear_pending() repairs garbage and keeps a valid count.
  wd::clear_pending(guard);
  assert(wd::valid(guard) && guard.consecutive == 0 && guard.pending == 0);
  wd::mark_pending(guard);
  assert(wd::on_boot(guard, true) == wd::Mode::Armed);
  wd::mark_pending(guard);
  wd::clear_pending(guard);
  assert(guard.consecutive == 1 && guard.pending == 0);
  // A healthy period clears the count and any pending stall with it.
  wd::mark_pending(guard);
  assert(wd::clear_if_healthy(guard, 3600 * kSecond, 0, kHealthyS));
  assert(guard.consecutive == 0 && guard.pending == 0);
  assert(wd::on_boot(guard, true) == wd::Mode::Armed);
  assert(guard.consecutive == 0);
  // No heartbeat yet is never a stall.
  wd::Supervisor state;
  wd::start(state, 0, true);
  const auto step =
      wd::supervise(state, guard, 600 * kSecond, 0, kTimeoutS, kHealthyS);
  assert(step.action == wd::Action::Feed && !state.tripped);
}
} // namespace

int main() {
  healthy_worker_only_feeds();
  transient_stalls_recover_each_time();
  repeated_stalls_hit_the_boot_loop_guard();
  slow_progressing_write_never_trips();
  sink_that_never_returns_trips();
  timer_failure_leaves_no_armed_user();
  reconfigure_failures_unwind();
  arm_failure_retries();
  guard_edges();
  std::puts("watchdog supervisor tests passed");
}
