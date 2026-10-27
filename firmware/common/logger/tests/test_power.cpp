#include "os_logger_emergency_close.h"
#include "os_logger_power.h"

#include <cassert>
#include <cstring>

namespace {
using opensensor::battery::State;
using oslogger::power::Tracker;

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
  // A rebound to Low is not a recovery. One discharge closes once (#28).
  for (int i = 0; i < 3; ++i) {
    assert(!tracker.observe(State::Low).finalize);
    change = tracker.observe(State::Critical);
    assert(change.changed && !change.finalize);
  }
  // A settling gauge does not re-arm either.
  assert(!tracker.observe(State::Settling).finalize);
  assert(!tracker.observe(State::Critical).finalize);
  // Ok, past the low threshold plus hysteresis, re-arms.
  assert(!tracker.observe(State::Ok).finalize);
  assert(!tracker.observe(State::Low).finalize);
  assert(tracker.observe(State::Critical).finalize);
  // External power (a charge session) re-arms.
  assert(!tracker.observe(State::ExternalPowerLikely).finalize);
  assert(tracker.observe(State::Critical).finalize);
  assert(tracker.last() == State::Critical);
  assert(Tracker::rearms(State::Ok) &&
         Tracker::rearms(State::ExternalPowerLikely));
  assert(!Tracker::rearms(State::Low) && !Tracker::rearms(State::Critical) &&
         !Tracker::rearms(State::Unknown) && !Tracker::rearms(State::Settling));

  // Booting straight into critical finalizes once.
  Tracker boot;
  assert(boot.observe(State::Critical).finalize);
  assert(!boot.observe(State::Critical).finalize);
}

void lines() {
  char out[64];
  auto n =
      oslogger::power::format_line(out, sizeof(out), State::Low, 3480, false);
  assert(n == std::strlen(out));
  assert(std::strcmp(out, "POWER STATE state=low mv=3480") == 0);
  n = oslogger::power::format_line(out, sizeof(out), State::Critical, 3301,
                                   true);
  assert(std::strcmp(out, "POWER STATE state=critical mv=3301 "
                          "action=finalize") == 0 &&
         n == std::strlen(out));
  oslogger::power::format_line(out, sizeof(out), State::Unknown, -1, false);
  assert(std::strcmp(out, "POWER STATE state=unknown mv=null") == 0);
  oslogger::power::format_line(out, sizeof(out), State::ExternalPowerLikely,
                               4267, false);
  assert(std::strcmp(out, "POWER STATE state=external_power_likely "
                          "mv=4267") == 0);
  // Too small never reports a truncated line.
  char tiny[12];
  assert(oslogger::power::format_line(tiny, sizeof(tiny), State::Ok, 3900,
                                      false) == 0 &&
         tiny[0] == '\0');
  assert(oslogger::power::format_line(nullptr, 10, State::Ok, 1, false) == 0);
  constexpr std::size_t exact = sizeof("POWER STATE state=ok mv=3900") - 1;
  char fit[exact + 1];
  assert(oslogger::power::format_line(fit, sizeof(fit), State::Ok, 3900,
                                      false) == exact);
  assert(oslogger::power::format_line(fit, exact, State::Ok, 3900, false) == 0);
}

// Fake card for attempt_close. Counts calls so the tests can prove a busy
// card is never written and an empty one is never probed.
struct FakeStorage {
  bool data = true;
  bool can_write = true;
  bool commit_ok = true;
  int probes = 0;
  int writes = 0;
  bool has_data() const { return data; }
  bool writable() {
    ++probes;
    return can_write;
  }
  bool finalize() {
    ++writes;
    if (commit_ok)
      data = false;
    return commit_ok;
  }
};

using oslogger::power::attempt_close;
using oslogger::power::CloseOutcome;
using oslogger::power::EmergencyClose;
using oslogger::power::Next;
using oslogger::power::RetryPolicy;

void outcomes() {
  FakeStorage empty;
  empty.data = false;
  assert(attempt_close(empty) == CloseOutcome::NothingOpen);
  assert(empty.probes == 0 && empty.writes == 0);
  FakeStorage busy;
  busy.can_write = false;
  assert(attempt_close(busy) == CloseOutcome::BusyRetry);
  assert(busy.probes == 1 && busy.writes == 0);
  FakeStorage broken;
  broken.commit_ok = false;
  assert(attempt_close(broken) == CloseOutcome::Failed);
  FakeStorage good;
  assert(attempt_close(good) == CloseOutcome::Closed && good.writes == 1);
  assert(std::strcmp(oslogger::power::outcome_name(CloseOutcome::Closed),
                     "closed") == 0);
  assert(std::strcmp(oslogger::power::outcome_name(CloseOutcome::NothingOpen),
                     "nothing-open") == 0);
  assert(std::strcmp(oslogger::power::outcome_name(CloseOutcome::BusyRetry),
                     "busy-retry") == 0);
  assert(std::strcmp(oslogger::power::outcome_name(CloseOutcome::Failed),
                     "failed") == 0);
}

void intent_lifecycle() {
  EmergencyClose close;
  assert(!close.due(0) && !close.preempt(0) && !close.active());
  // A finish without a due attempt changes nothing.
  assert(close.finish(CloseOutcome::Failed, 0) == Next::Done);
  // Raised from the main loop, it survives until the worker looks. There
  // is no queue to fill, so a busy command queue cannot lose it.
  close.request();
  close.request();
  assert(close.preempt(5) && close.due(5) && close.active());
  assert(close.finish(CloseOutcome::Closed, 5) == Next::Done);
  assert(!close.active() && !close.due(6) && !close.preempt(6));
  // An empty close is a quiet success, not a retry.
  close.request();
  assert(close.due(10));
  assert(close.finish(CloseOutcome::NothingOpen, 10) == Next::Done);
  assert(!close.due(11));
}

void retry_until_recovery() {
  // A failed footer, then storage in Retry, then the card recovers.
  EmergencyClose close;
  FakeStorage card;
  card.commit_ok = false;
  close.request();
  std::int64_t now = 1000;
  assert(close.due(now));
  assert(close.finish(attempt_close(card), now) == Next::Retry);
  assert(close.retry_at_ms() == now + 1000 && close.active());
  assert(!close.due(now + 999) && !close.preempt(now + 999));
  card.commit_ok = true;
  card.can_write = false;
  now += 1000;
  assert(close.due(now));
  assert(close.finish(attempt_close(card), now) == Next::Retry);
  assert(close.retry_at_ms() == now + 2000);
  // A repeated request while retrying keeps the backoff.
  close.request();
  assert(!close.due(now + 1));
  card.can_write = true;
  now += 2000;
  assert(close.due(now));
  assert(close.finish(attempt_close(card), now) == Next::Done);
  assert(close.attempts() == 3 && card.writes == 2 && !close.active());
}

void gives_up() {
  RetryPolicy policy;
  policy.max_attempts = 8;
  EmergencyClose close(policy);
  close.request();
  std::int64_t now = 0;
  const std::int64_t expected[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
  for (std::int64_t backoff : expected) {
    assert(close.due(now));
    assert(close.finish(CloseOutcome::BusyRetry, now) == Next::Retry);
    assert(close.retry_at_ms() == now + backoff);
    now += backoff;
  }
  assert(close.due(now));
  assert(close.finish(CloseOutcome::Failed, now) == Next::GaveUp);
  assert(!close.active() && !close.due(now + 60000));
  // A later critical entry, after a re-arm, starts a fresh intent.
  close.request();
  assert(close.due(now + 60000) && close.attempts() == 0);
}

void preempts_transfers() {
  EmergencyClose close;
  // A LIST, READ or serial get checks preempt() per frame and stops with
  // its existing busy error, then the worker loop runs the close.
  close.request();
  assert(close.preempt(0));
  assert(close.preempt(0)); // never consumed by a checkpoint
  assert(close.due(0));
  assert(close.finish(CloseOutcome::BusyRetry, 0) == Next::Retry);
  // While waiting out a backoff, transfers run normally.
  assert(!close.preempt(500));
  assert(close.preempt(1000));
}

void cut_before_ack() {
  // Power cut after the request but before the worker closed. A boot starts
  // fresh objects, the first critical report arms a new intent.
  {
    Tracker tracker;
    EmergencyClose close;
    if (tracker.observe(State::Critical).finalize)
      close.request();
    assert(close.preempt(0));
  }
  Tracker tracker;
  EmergencyClose close;
  if (tracker.observe(State::Critical).finalize)
    close.request();
  assert(close.due(0));
  FakeStorage card;
  assert(close.finish(attempt_close(card), 0) == Next::Done);
}

// The report_battery() path. A critical entry raises the intent and wakes
// the worker once, and a repeat does neither, so the main loop never touches
// the card.
void report_wakes_worker() {
  Tracker tracker;
  EmergencyClose close;
  int wakes = 0;
  const auto wake = [&] { ++wakes; };
  assert(!oslogger::power::report(tracker, close, State::Ok, wake).finalize);
  assert(wakes == 0 && !close.preempt(0));
  assert(
      oslogger::power::report(tracker, close, State::Critical, wake).finalize);
  assert(wakes == 1 && close.preempt(0));
  assert(
      !oslogger::power::report(tracker, close, State::Critical, wake).finalize);
  assert(wakes == 1);
  assert(close.due(0) && close.finish(CloseOutcome::Closed, 0) == Next::Done);
  // Settling never re-arms, Ok does.
  oslogger::power::report(tracker, close, State::Settling, wake);
  oslogger::power::report(tracker, close, State::Critical, wake);
  assert(wakes == 1 && !close.preempt(1));
  oslogger::power::report(tracker, close, State::Ok, wake);
  oslogger::power::report(tracker, close, State::Critical, wake);
  assert(wakes == 2 && close.preempt(1));
}

// The worker pass and its serial line, as service_emergency_close() runs it.
// files counts telemetry files finalized during the attempt.
void pass_lines() {
  using oslogger::power::format_pass;
  using oslogger::power::Pass;
  using oslogger::power::service;
  char line[160];
  EmergencyClose close;
  FakeStorage card;
  // Not due, no attempt and no line.
  Pass pass = service(close, card, 0);
  assert(!pass.attempted && card.probes == 0);
  assert(format_pass(line, sizeof(line), pass, close, "ready", 0, 0) == 0);
  // A first attempt with nothing open is quiet.
  card.data = false;
  close.request();
  pass = service(close, card, 0);
  assert(pass.attempted && pass.outcome == CloseOutcome::NothingOpen &&
         pass.next == Next::Done);
  assert(format_pass(line, sizeof(line), pass, close, "ready", 0, 0) == 0 &&
         line[0] == '\0');
  // Closed on the first attempt.
  card.data = true;
  close.request();
  pass = service(close, card, 100);
  assert(pass.outcome == CloseOutcome::Closed && pass.next == Next::Done);
  format_pass(line, sizeof(line), pass, close, "ready", 1, 100);
  assert(std::strcmp(line, "PARQUET FLUSH reason=power-critical "
                           "result=closed attempt=1 files=1") == 0);
  // Storage in Retry. The probe inside writable() may already finalize the
  // telemetry file, and files reports it on the retry line.
  card.data = true;
  card.can_write = false;
  close.request();
  pass = service(close, card, 200);
  assert(pass.outcome == CloseOutcome::BusyRetry && pass.next == Next::Retry);
  format_pass(line, sizeof(line), pass, close, "retry", 1, 200);
  assert(std::strcmp(line, "PARQUET FLUSH reason=power-critical "
                           "result=busy-retry storage=retry attempt=1 "
                           "files=1 retry_ms=1000") == 0);
  // Backoff not over, no attempt.
  assert(!service(close, card, 1199).attempted);
  // Nothing left after that probe. A later nothing-open logs, so the file
  // the probe closed is not reported as lost.
  card.data = false;
  pass = service(close, card, 1200);
  assert(pass.outcome == CloseOutcome::NothingOpen && pass.next == Next::Done);
  format_pass(line, sizeof(line), pass, close, "ready", 0, 1200);
  assert(std::strcmp(line, "PARQUET FLUSH reason=power-critical "
                           "result=nothing-open attempt=2 files=0") == 0);
  // Giving up reports closed=0 and any file a probe finalized meanwhile.
  RetryPolicy one;
  one.max_attempts = 1;
  EmergencyClose last(one);
  FakeStorage broken;
  broken.commit_ok = false;
  last.request();
  pass = service(last, broken, 0);
  assert(pass.outcome == CloseOutcome::Failed && pass.next == Next::GaveUp);
  format_pass(line, sizeof(line), pass, last, "ready", 0, 0);
  assert(std::strcmp(line, "PARQUET ERROR operation=power-close "
                           "reason=failed attempts=1 files=0 closed=0") == 0);
  // A buffer too small for the line yields nothing rather than a cut line.
  char tiny[16];
  assert(format_pass(tiny, sizeof(tiny), pass, last, "ready", 0, 0) == 0 &&
         tiny[0] == '\0');
}
} // namespace

int main() {
  tracker();
  lines();
  outcomes();
  intent_lifecycle();
  retry_until_recovery();
  gives_up();
  preempts_transfers();
  cut_before_ack();
  report_wakes_worker();
  pass_lines();
  return 0;
}
