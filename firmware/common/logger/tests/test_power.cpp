#include "aq_logger_power.h"

#include <cassert>
#include <cstring>

namespace {
using aq::battery::State;
using aqlogger::power::Tracker;

void tracker() {
  Tracker tracker;
  auto change = tracker.observe(State::Unknown);
  assert(change.changed && !change.finalize);
  change = tracker.observe(State::Unknown);
  assert(!change.changed && !change.finalize);
  change = tracker.observe(State::Ok);
  assert(change.changed && !change.finalize);
  change = tracker.observe(State::Low);
  assert(change.changed && !change.finalize);
  change = tracker.observe(State::Critical);
  assert(change.changed && change.finalize);
  // Staying critical never repeats the finalize.
  for (int i = 0; i < 5; ++i) {
    change = tracker.observe(State::Critical);
    assert(!change.changed && !change.finalize);
  }
  // An unknown reading in between does not re-arm.
  assert(tracker.observe(State::Unknown).changed);
  change = tracker.observe(State::Critical);
  assert(change.changed && !change.finalize);
  // Recovery re-arms, the next entry finalizes again.
  assert(!tracker.observe(State::Low).finalize);
  assert(tracker.observe(State::Critical).finalize);
  assert(!tracker.observe(State::ExternalPowerLikely).finalize);
  assert(tracker.observe(State::Critical).finalize);
  assert(!tracker.observe(State::Settling).finalize);
  assert(tracker.observe(State::Critical).finalize);
  assert(tracker.last() == State::Critical);

  // Booting straight into critical finalizes once.
  Tracker boot;
  assert(boot.observe(State::Critical).finalize);
  assert(!boot.observe(State::Critical).finalize);
}

void lines() {
  char out[64];
  auto n =
      aqlogger::power::format_line(out, sizeof(out), State::Low, 3480, false);
  assert(n == std::strlen(out));
  assert(std::strcmp(out, "POWER STATE state=low mv=3480") == 0);
  n = aqlogger::power::format_line(out, sizeof(out), State::Critical, 3301,
                                   true);
  assert(std::strcmp(out, "POWER STATE state=critical mv=3301 "
                          "action=finalize") == 0 &&
         n == std::strlen(out));
  aqlogger::power::format_line(out, sizeof(out), State::Unknown, -1, false);
  assert(std::strcmp(out, "POWER STATE state=unknown mv=null") == 0);
  aqlogger::power::format_line(out, sizeof(out), State::ExternalPowerLikely,
                               4267, false);
  assert(std::strcmp(out, "POWER STATE state=external_power_likely "
                          "mv=4267") == 0);
  // Too small never reports a truncated line.
  char tiny[12];
  assert(aqlogger::power::format_line(tiny, sizeof(tiny), State::Ok, 3900,
                                      false) == 0 &&
         tiny[0] == '\0');
  assert(aqlogger::power::format_line(nullptr, 10, State::Ok, 1, false) == 0);
  constexpr std::size_t exact = sizeof("POWER STATE state=ok mv=3900") - 1;
  char fit[exact + 1];
  assert(aqlogger::power::format_line(fit, sizeof(fit), State::Ok, 3900,
                                      false) == exact);
  assert(aqlogger::power::format_line(fit, exact, State::Ok, 3900, false) == 0);
}
} // namespace

int main() {
  tracker();
  lines();
  return 0;
}
