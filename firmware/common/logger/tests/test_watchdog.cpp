// Fake-clock tests for the storage-worker watchdog supervisor. The rig calls
// the same watchdog::prepare(), launch(), tick() and apply() that
// os_logger.cpp calls, over a fake task watchdog with the IDF feed rule.
#include "os_logger_watchdog.h"
#include "os_logger_watchdog_apply.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

namespace wd = oslogger::watchdog;

namespace {
constexpr std::int64_t kSecond = 1000000LL;
constexpr std::uint32_t kTimeoutS = 60;
constexpr std::uint32_t kHealthyS = 3600;
constexpr std::int64_t kTwdtPeriodUs = 5 * kSecond; // ESP_TASK_WDT_TIMEOUT_S
// begin() runs a little after boot, so a seeded heartbeat is never zero.
constexpr std::int64_t kBeginUs = 300000;

// The ESP-IDF task watchdog as task_wdt.c implements it. Each entry has a
// has_reset flag. A reset marks its entry, and the hardware timer is fed only
// when every other entry has reset too, which then clears every flag. The
// two idle tasks are entries that reset whenever they run. A reconfigure
// restarts the period and leaves the flags alone. A period that ends unfed
// prints, or panics once panic is on.
struct FakeTwdt {
  struct Entry {
    bool present = false;
    bool has_reset = false;
  };
  const std::int64_t *clock = nullptr;
  Entry idle[2]{{true, false}, {true, false}};
  Entry user;
  bool panic = false; // the original config is print-only
  std::int64_t period_start = 0;
  int prints = 0;
  bool fail_add = false;
  bool fail_reconfigure = false;
  std::string calls; // a add, r reset, c reconfigure, d delete

  bool others_reset(const Entry &except) const {
    for (const Entry *entry : {&idle[0], &idle[1], &user})
      if (entry != &except && entry->present && !entry->has_reset)
        return false;
    return true;
  }
  void timer_feed() {
    period_start = *clock;
    idle[0].has_reset = idle[1].has_reset = user.has_reset = false;
  }
  bool reset(Entry &entry) {
    if (!entry.present)
      return false; // ESP_ERR_NOT_FOUND
    const bool all = others_reset(entry);
    entry.has_reset = true;
    if (all)
      timer_feed();
    return true;
  }
  void idle_tasks_run() {
    reset(idle[0]);
    reset(idle[1]);
  }

  // The Twdt interface of os_logger_watchdog_apply.h.
  bool add_user() {
    calls += 'a';
    if (fail_add || user.present)
      return false;
    const bool all = others_reset(user);
    user = {true, false};
    if (all)
      timer_feed();
    return true;
  }
  bool reset_user() {
    calls += 'r';
    return reset(user);
  }
  bool reconfigure(bool trigger_panic) {
    calls += 'c';
    if (fail_reconfigure)
      return false;
    panic = trigger_panic;
    period_start = *clock;
    return true;
  }
  void delete_user() {
    calls += 'd';
    user = {};
    if (others_reset(user))
      timer_feed();
  }

  // True when the period just ended unfed with panic on.
  bool period_check() {
    if (*clock - period_start < kTwdtPeriodUs)
      return false;
    if (panic)
      return true;
    ++prints;
    period_start = *clock;
    return false;
  }
};

struct FakeTimer {
  bool fail_create = false;
  bool fail_start = false;
  bool running = false;
  bool create() { return !fail_create; }
  bool start() {
    running = !fail_start;
    return running;
  }
  void destroy() { running = false; }
};

struct Rig {
  std::int64_t now = 0;
  std::int64_t heartbeat = 0;
  wd::Guard guard{0xdeadbeef, 7, 9, 1}; // power-on garbage
  wd::Supervisor state;
  FakeTwdt twdt;
  FakeTimer timer;
  wd::Boot last_boot{};
  bool idle_starved = false; // the idle tasks stop running
  bool reset = false;        // the chip reset through the task watchdog
  int trips = 0;             // since boot
  int recoveries = 0;        // since boot
  int clears = 0;

  // stamp_heartbeat(): confirmed worker progress, which also feeds the user.
  void stamp() {
    heartbeat = now;
    if (twdt.user.present)
      twdt.reset_user();
  }

  // begin() and start_worker_watchdog() after a reset of the given kind.
  // begin() seeds the heartbeat before the worker task starts, and the
  // worker stamps first thing unless it never runs.
  wd::Mode boot(bool task_watchdog_reset, bool worker_runs = true) {
    now = kBeginUs;
    reset = false;
    trips = 0;
    recoveries = 0;
    // A reset drops every user and the panic switch, not the injected faults.
    FakeTwdt fresh;
    fresh.fail_add = twdt.fail_add;
    fresh.fail_reconfigure = twdt.fail_reconfigure;
    twdt = fresh;
    twdt.clock = &now;
    twdt.period_start = now;
    timer.running = false;
    heartbeat = now;
    last_boot =
        wd::prepare(state, guard, twdt, timer, task_watchdog_reset, now);
    wd::launch(last_boot, state, twdt, timer);
    if (worker_runs)
      stamp();
    return last_boot.mode;
  }

  // supervise_worker().
  void tick() {
    const auto result = wd::tick(state, guard, twdt, now, heartbeat, kTimeoutS,
                                 kHealthyS, false);
    clears += result.cleared ? 1 : 0;
    trips += result.action == wd::Action::Trip && state.tripped ? 1 : 0;
    recoveries += result.recovered ? 1 : 0;
  }

  // Advances the fake clock in 100 ms steps. The idle tasks run every step,
  // the supervisor ticks every second while its timer runs, and the worker
  // stamps every stamp_us (0 never).
  void run(std::int64_t duration_us, std::int64_t stamp_us) {
    const std::int64_t end = now + duration_us;
    std::int64_t next_stamp = stamp_us ? now + stamp_us : end + 1;
    while (now < end && !reset) {
      now += kSecond / 10;
      if (!idle_starved)
        twdt.idle_tasks_run();
      if (now >= next_stamp) {
        stamp();
        next_stamp += stamp_us;
      }
      if (timer.running && now % kSecond == 0)
        tick();
      reset = twdt.period_check();
    }
  }
};

// apply() calls the task watchdog in the order os_logger.cpp relies on. Arm
// registers before it resets, and Recover restores before it feeds.
void apply_calls_in_order() {
  std::int64_t clock = 0;
  FakeTwdt twdt;
  twdt.clock = &clock;
  assert(wd::apply(twdt, wd::Action::Nothing, false) && twdt.calls.empty());
  assert(wd::apply(twdt, wd::Action::Arm, false) && twdt.calls == "ar");
  assert(twdt.user.present);
  twdt.calls.clear();
  assert(wd::apply(twdt, wd::Action::Feed, false) && twdt.calls == "r");
  twdt.calls.clear();
  assert(wd::apply(twdt, wd::Action::Trip, false) && twdt.calls == "c");
  assert(twdt.panic);
  twdt.calls.clear();
  assert(wd::apply(twdt, wd::Action::Recover, false) && twdt.calls == "cr");
  assert(!twdt.panic);
  twdt.calls.clear();
  twdt.fail_reconfigure = true;
  assert(!wd::apply(twdt, wd::Action::Recover, false) && twdt.calls == "cr");
  twdt.calls.clear();
  assert(!wd::apply(twdt, wd::Action::Trip, false) && twdt.calls == "c");
  // A refused add never resets a user that is not there.
  FakeTwdt refused;
  refused.clock = &clock;
  refused.fail_add = true;
  assert(!wd::apply(refused, wd::Action::Arm, false) && refused.calls == "a");
  // A deleted user is not found, as the worker sees after disarm.
  twdt.delete_user();
  assert(!wd::apply(twdt, wd::Action::Feed, false));
}

void healthy_worker_only_feeds() {
  Rig rig;
  assert(rig.boot(false) == wd::Mode::Armed);
  assert(rig.timer.running && rig.twdt.user.present && rig.state.armed);
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
    assert(rig.last_boot.stall_reset && rig.guard.consecutive == n);
    assert(mode ==
           (n < wd::kMaxConsecutive ? wd::Mode::Armed : wd::Mode::WarnOnly));
  }
  // Warn-only has no user, so a stall can never reset.
  assert(!rig.twdt.user.present && !rig.state.armed);
  rig.run(600 * kSecond, 0);
  assert(!rig.reset && rig.trips == 0 && rig.guard.pending == 0);
  // A plain reboot keeps the count.
  assert(rig.boot(false) == wd::Mode::WarnOnly);
  // Continuous liveness for the healthy period clears the count and re-arms.
  rig.run((kHealthyS - 1) * kSecond, kSecond);
  assert(rig.clears == 0 && !rig.state.armed);
  rig.run(2 * kSecond, kSecond);
  assert(rig.clears == 1 && rig.guard.consecutive == 0);
  assert(rig.state.armed && rig.twdt.user.present);
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

// A worker that blocks before its first stamp. begin() seeded the heartbeat,
// so it ages out like any other stall. Without the seed it never would.
void worker_that_never_stamps_trips() {
  Rig rig;
  assert(rig.boot(false, false) == wd::Mode::Armed);
  const std::int64_t seeded = rig.heartbeat;
  assert(seeded == kBeginUs && rig.state.armed);
  rig.run(600 * kSecond, 0);
  assert(rig.reset && rig.trips == 1 && rig.guard.pending == 1);
  assert(rig.now > seeded + kTimeoutS * kSecond);
  assert(rig.now <= seeded + (kTimeoutS + 1) * kSecond + kTwdtPeriodUs);
  assert(rig.boot(true) == wd::Mode::Armed && rig.last_boot.stall_reset);
  assert(rig.guard.consecutive == 1);
}

// The timer feeds only once every entry has reset. A user fed every second
// does not hide idle tasks that stop running, which print as the IDF config
// says and never reset while panic is off.
void starved_idle_tasks_still_print() {
  Rig rig;
  rig.boot(false);
  rig.run(10 * kSecond, kSecond);
  assert(rig.twdt.prints == 0);
  rig.idle_starved = true;
  rig.run(30 * kSecond, kSecond);
  assert(rig.twdt.prints >= 5 && !rig.reset && rig.trips == 0);
  rig.idle_starved = false;
  const int prints = rig.twdt.prints;
  rig.run(60 * kSecond, kSecond);
  assert(rig.twdt.prints <= prints + 1 && !rig.reset);
}

// A supervisor timer that cannot be created or started leaves no armed user,
// so the board never resets for want of a feeder.
void timer_failure_leaves_no_armed_user() {
  for (int start_fails = 0; start_fails <= 1; ++start_fails) {
    Rig rig;
    rig.timer.fail_create = start_fails == 0;
    rig.timer.fail_start = start_fails == 1;
    assert(rig.boot(false) == wd::Mode::Armed);
    assert(!rig.last_boot.supervised && !rig.last_boot.armed);
    assert(!rig.timer.running && !rig.twdt.user.present && !rig.state.armed);
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
  assert(rig.timer.running && !rig.state.armed && !rig.twdt.user.present);
  rig.run(5 * kSecond, kSecond);
  assert(!rig.state.armed);
  rig.twdt.fail_add = false;
  rig.run(2 * kSecond, kSecond);
  assert(rig.state.armed && rig.twdt.user.present);
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
  apply_calls_in_order();
  healthy_worker_only_feeds();
  transient_stalls_recover_each_time();
  repeated_stalls_hit_the_boot_loop_guard();
  slow_progressing_write_never_trips();
  sink_that_never_returns_trips();
  worker_that_never_stamps_trips();
  starved_idle_tasks_still_print();
  timer_failure_leaves_no_armed_user();
  reconfigure_failures_unwind();
  arm_failure_retries();
  guard_edges();
  std::puts("watchdog supervisor tests passed");
}
