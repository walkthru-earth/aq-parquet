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
}
