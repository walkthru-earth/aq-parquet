// Deterministic interleavings for the actual worker queue/wakeup helper.
#include "aq_logger_work_queue.h"
#include <cassert>

int main() {
  FakeQueue samples, commands;
  FakeSemaphore wakeup;
  const int item = 1;

  // A command arriving between idle queue inspection and the wait wakes it.
  before_idle_wait = [&] {
    assert(aqlogger::work::enqueue(&commands, &item, &wakeup));
  };
  aqlogger::work::wait_if_idle(&samples, &wakeup);
  assert(commands.depth == 1 && wakeup.blocked == 0 && !wakeup.pending);
  commands.depth = 0;

  // A producer can enqueue before the task exists; its signal is retained.
  assert(aqlogger::work::enqueue(&commands, &item, &wakeup));
  aqlogger::work::wait_if_idle(&samples, &wakeup);
  assert(commands.depth == 1 && wakeup.blocked == 0);
  commands.depth = 0;

  // Binary signals coalesce, while every successfully copied item survives.
  for (unsigned i = 0; i < commands.capacity; ++i)
    assert(aqlogger::work::enqueue(&commands, &item, &wakeup));
  assert(!aqlogger::work::enqueue(&commands, &item, &wakeup));
  assert(commands.depth == commands.capacity && wakeup.pending);
  aqlogger::work::wait_if_idle(&samples, &wakeup);
  assert(commands.depth == commands.capacity && wakeup.blocked == 0);
  commands.depth = 0;

  // Samples queued behind a processed command keep their priority next pass.
  assert(aqlogger::work::enqueue(&samples, &item, &wakeup));
  const auto waits = wakeup.waits;
  aqlogger::work::wait_if_idle(&samples, &wakeup);
  assert(wakeup.waits == waits && samples.depth == 1);
  samples.depth = 0;
  aqlogger::work::wait_if_idle(&samples,
                               &wakeup); // consume the retained signal
  assert(wakeup.blocked == 0);
  aqlogger::work::wait_if_idle(&samples, &wakeup); // truly idle, bounded wait
  assert(wakeup.blocked == 1);

  // A rejected enqueue must not create a wakeup or a phantom item.
  commands.capacity = 0;
  assert(!aqlogger::work::enqueue(&commands, &item, &wakeup));
  assert(!wakeup.pending && commands.depth == 0);

  // Worker stall liveness: passive warnings after 5 s with 30 s cooldown, and
  // opt-in active watchdog restart when worker_stall_restart_s is configured.
  using aqlogger::work::check_stall;
  using aqlogger::work::StallAction;
  std::int64_t last_warn = 0;
  assert(check_stall(10000000LL, 0, last_warn, 0) == StallAction::None);
  assert(check_stall(5000000LL, 1000000LL, last_warn, 0) == StallAction::None);
  assert(check_stall(6000001LL, 1000000LL, last_warn, 0) == StallAction::Warn);
  assert(last_warn == 6000001LL);
  assert(check_stall(20000000LL, 1000000LL, last_warn, 0) == StallAction::None);
  assert(check_stall(36000002LL, 1000000LL, last_warn, 0) == StallAction::Warn);
  assert(last_warn == 36000002LL);
  // With restart_s = 120, stalls <= 120 s warn/throttle; > 120 s trigger
  // Restart.
  assert(check_stall(121000000LL, 1000000LL, last_warn, 120) ==
         StallAction::Warn);
  assert(check_stall(121000000LL, 1000000LL, last_warn, 120) ==
         StallAction::None);
  assert(check_stall(121000001LL, 1000000LL, last_warn, 120) ==
         StallAction::Restart);
  // Values below 5 s clamp to 5 s so short SD flushes never boot-loop.
  assert(check_stall(5500000LL, 1000000LL, last_warn, 1) == StallAction::None);
  assert(check_stall(6000001LL, 1000000LL, last_warn, 1) ==
         StallAction::Restart);
}
