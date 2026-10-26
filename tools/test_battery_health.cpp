#include "../firmware/common/power/src/os_battery_health.h"

#include <cassert>
#include <cstdint>
#include <cstring>

namespace {
using opensensor::battery::Assessment;
using opensensor::battery::Monitor;
using opensensor::battery::Sample;
using opensensor::battery::State;
using opensensor::battery::Thresholds;

constexpr std::int64_t kTick = 10'000;
constexpr std::int64_t kMinute = 60'000;

Sample volts(std::int32_t mv, std::int64_t now) {
  Sample sample;
  sample.now_ms = now;
  sample.vcell_ok = true;
  sample.millivolts = mv;
  sample.soc_ok = true;
  sample.raw_soc = 50 * 256;
  sample.status_ok = true;
  return sample;
}

struct Feed {
  Monitor monitor;
  std::int64_t now = 0;
  explicit Feed(const Thresholds &t = Thresholds{}) : monitor(t) {}
  Assessment at(std::int32_t mv) {
    now += kTick;
    return monitor.update(volts(mv, now));
  }
  Assessment invalid() {
    now += kTick;
    Sample sample;
    sample.now_ms = now;
    return monitor.update(sample);
  }
};

void names_and_codes() {
  static_assert(static_cast<int>(State::Unknown) == 0);
  static_assert(static_cast<int>(State::ExternalPowerLikely) == 1);
  static_assert(static_cast<int>(State::Ok) == 2);
  static_assert(static_cast<int>(State::Low) == 3);
  static_assert(static_cast<int>(State::Critical) == 4);
  static_assert(static_cast<int>(State::Settling) == 5);
  assert(std::strcmp(opensensor::battery::state_name(State::Unknown),
                     "unknown") == 0);
  assert(
      std::strcmp(opensensor::battery::state_name(State::ExternalPowerLikely),
                  "external_power_likely") == 0);
  assert(std::strcmp(opensensor::battery::state_name(State::Ok), "ok") == 0);
  assert(std::strcmp(opensensor::battery::state_name(State::Low), "low") == 0);
  assert(std::strcmp(opensensor::battery::state_name(State::Critical),
                     "critical") == 0);
  assert(std::strcmp(opensensor::battery::state_name(State::Settling),
                     "settling") == 0);
  assert(std::strcmp(opensensor::battery::state_name(static_cast<State>(99)),
                     "unknown") == 0);
}

void threshold_validation() {
  assert(Thresholds{}.validate());
  auto t = Thresholds{};
  t.critical_enter_mv = 2499;
  assert(!t.validate());
  t = Thresholds{};
  t.critical_enter_mv = 3430; // crit + hysteresis above low
  assert(!t.validate());
  t = Thresholds{};
  t.low_enter_mv = 4150; // low + hysteresis above the external exit
  assert(!t.validate());
  t = Thresholds{};
  t.external_power_mv = 4501;
  assert(!t.validate());
  t = Thresholds{};
  t.debounce_samples = 0;
  assert(!t.validate());
  t.debounce_samples = 11;
  assert(!t.validate());
  t = Thresholds{};
  t.hysteresis_mv = 9;
  assert(!t.validate());
  t = Thresholds{};
  t.external_power_hysteresis_mv = 101;
  assert(!t.validate());
  t = Thresholds{};
  t.settling_minutes = 241;
  assert(!t.validate());
  t.settling_minutes = 0;
  assert(t.validate());

  // An invalid set never classifies.
  t = Thresholds{};
  t.debounce_samples = 0;
  Feed feed{t};
  assert(!feed.monitor.configured());
  for (int i = 0; i < 5; ++i)
    assert(feed.at(3900).state == State::Unknown);
}

void initial_classification_needs_agreement() {
  Feed feed;
  assert(feed.at(3900).state == State::Unknown);
  assert(feed.at(3900).state == State::Unknown);
  assert(feed.at(3400).state == State::Unknown); // band changed, restart
  assert(feed.at(3400).state == State::Unknown);
  assert(feed.at(3400).state == State::Low);

  Feed usb;
  for (int i = 0; i < 2; ++i)
    assert(usb.at(4267).state == State::Unknown);
  assert(usb.at(4267).state == State::ExternalPowerLikely);

  Feed empty;
  for (int i = 0; i < 2; ++i)
    empty.at(3300);
  assert(empty.at(3300).state == State::Critical);

  // A cell resting on the critical threshold at boot alternates Low and
  // Critical reads. They form one run, so it still leaves Unknown, and
  // Critical wins when at least half the run read critical.
  Feed edge;
  assert(edge.at(3340).state == State::Unknown);
  assert(edge.at(3360).state == State::Unknown);
  assert(edge.at(3340).state == State::Critical);

  Feed edge_low_first;
  assert(edge_low_first.at(3360).state == State::Unknown);
  assert(edge_low_first.at(3340).state == State::Unknown);
  assert(edge_low_first.at(3360).state == State::Low); // 1 of 3 critical

  Thresholds four;
  four.debounce_samples = 4;
  Feed edge_tie{four};
  assert(edge_tie.at(3360).state == State::Unknown);
  assert(edge_tie.at(3340).state == State::Unknown);
  assert(edge_tie.at(3360).state == State::Unknown);
  assert(edge_tie.at(3340).state == State::Critical); // 2 of 4, tie
}

void debounce_and_hysteresis() {
  Feed feed;
  for (int i = 0; i < 3; ++i)
    feed.at(3800);
  assert(feed.at(3800).state == State::Ok);
  // Two low samples and a recovery do not change the state.
  feed.at(3490);
  feed.at(3490);
  assert(feed.at(3800).state == State::Ok);
  feed.at(3500); // at the threshold counts as low
  feed.at(3490);
  assert(feed.at(3480).state == State::Low);
  // Inside the hysteresis band the state holds.
  for (int i = 0; i < 5; ++i)
    assert(feed.at(3574).state == State::Low);
  feed.at(3575);
  feed.at(3600);
  assert(feed.at(3600).state == State::Ok);

  // A run that touches critical once and low twice commits the mildest.
  feed.at(3300);
  feed.at(3450);
  assert(feed.at(3300).state == State::Low);
  feed.at(3350);
  feed.at(3340);
  assert(feed.at(3200).state == State::Critical);
  for (int i = 0; i < 5; ++i)
    assert(feed.at(3424).state == State::Critical);
  feed.at(3425);
  feed.at(3425);
  assert(feed.at(3425).state == State::Low);

  // External power enters at 4240 and leaves at or below 4220.
  feed.at(4240);
  feed.at(4250);
  assert(feed.at(4260).state == State::ExternalPowerLikely);
  for (int i = 0; i < 5; ++i)
    assert(feed.at(4221).state == State::ExternalPowerLikely);
  feed.at(4220);
  feed.at(4200);
  assert(feed.at(4100).state == State::Ok);

  // Direction changes restart the run.
  feed.at(3400);
  feed.at(3400);
  feed.at(4267);
  feed.at(3400);
  assert(feed.at(3400).state == State::Ok);
  assert(feed.at(3400).state == State::Low);
}

void debounce_of_one() {
  Thresholds t;
  t.debounce_samples = 1;
  Feed feed{t};
  assert(feed.at(3800).state == State::Ok);
  assert(feed.at(3300).state == State::Critical);
  assert(feed.at(4300).state == State::ExternalPowerLikely);
  assert(feed.invalid().state == State::Unknown);
}

void invalid_samples() {
  Feed feed;
  for (int i = 0; i < 3; ++i)
    feed.at(3800);
  // Two invalids keep the state, the third reports unknown.
  assert(feed.invalid().state == State::Ok);
  assert(feed.invalid().state == State::Ok);
  assert(feed.invalid().state == State::Unknown);
  assert(feed.invalid().state == State::Unknown);
  // Recovery needs a fresh agreeing run.
  feed.at(3800);
  feed.at(3800);
  assert(feed.at(3800).state == State::Ok);

  // An invalid sample clears a pending run.
  feed.at(3300);
  feed.at(3300);
  feed.invalid();
  feed.at(3300);
  assert(feed.at(3300).state == State::Ok);
  assert(feed.at(3300).state == State::Critical);
}

void settling_window() {
  Feed feed;
  feed.monitor.note_gauge_reset(feed.now);
  for (int i = 0; i < 2; ++i)
    feed.at(3800);
  auto a = feed.at(3800);
  assert(a.state == State::Settling && a.settling && !a.soc_trusted);
  // Settling never hides a low or critical cell.
  for (int i = 0; i < 3; ++i)
    a = feed.at(3300);
  assert(a.state == State::Critical && a.settling && !a.soc_trusted);
  for (int i = 0; i < 3; ++i)
    a = feed.at(4300);
  assert(a.state == State::ExternalPowerLikely && a.settling);
  for (int i = 0; i < 3; ++i)
    a = feed.at(3800);
  assert(a.state == State::Settling);
  // The window ends after 60 minutes from the reset.
  feed.now = 60 * kMinute - 2 * kTick;
  assert(feed.at(3800).state == State::Settling);
  a = feed.at(3800);
  assert(a.state == State::Ok && !a.settling && a.soc_trusted);

  // Time running backwards restarts the window.
  Feed clock;
  clock.now = 100 * kMinute;
  clock.monitor.note_gauge_reset(clock.now);
  for (int i = 0; i < 3; ++i)
    clock.at(3800);
  clock.now = 0;
  assert(clock.at(3800).state == State::Settling);
  clock.now = 59 * kMinute;
  assert(clock.at(3800).state == State::Settling);
  clock.now = 61 * kMinute;
  assert(clock.at(3800).state == State::Ok);

  // RI seen in a sample starts the window once per rising edge.
  Feed ri;
  for (int i = 0; i < 3; ++i)
    ri.at(3800);
  auto sample = volts(3800, ri.now += kTick);
  sample.reset_indicator = true;
  assert(ri.monitor.update(sample).state == State::Settling);
  ri.now += 59 * kMinute;
  sample = volts(3800, ri.now);
  sample.reset_indicator = true; // still set, the window is not restarted
  assert(ri.monitor.update(sample).state == State::Settling);
  ri.now += 2 * kMinute;
  sample = volts(3800, ri.now);
  sample.reset_indicator = true;
  assert(ri.monitor.update(sample).state == State::Ok);
  // A failed STATUS read neither starts nor ends anything.
  sample = volts(3800, ri.now += kTick);
  sample.status_ok = false;
  sample.reset_indicator = true;
  assert(ri.monitor.update(sample).state == State::Ok);

  // A zero window disables settling.
  Thresholds none;
  none.settling_minutes = 0;
  Feed off{none};
  off.monitor.note_gauge_reset(0);
  for (int i = 0; i < 2; ++i)
    off.at(3800);
  assert(off.at(3800).state == State::Ok);
}

void soc_trust_and_minutes() {
  Feed feed;
  for (int i = 0; i < 3; ++i)
    feed.at(3800);
  auto sample = volts(3800, feed.now += kTick);
  sample.raw_soc = 50 * 256;
  sample.crate_ok = true;
  sample.crate_milli_percent_per_hour = -10 * 208; // -2.08 %/h
  auto a = feed.monitor.update(sample);
  assert(a.soc_trusted);
  // 50 % at 2.08 %/h is 1442.3 minutes.
  assert(a.minutes_to_empty == 1442);

  sample.crate_milli_percent_per_hour = 208; // charging
  assert(feed.monitor.update(sample).minutes_to_empty == -1);
  sample.crate_milli_percent_per_hour = 0;
  assert(feed.monitor.update(sample).minutes_to_empty == -1);
  sample.crate_milli_percent_per_hour = -208; // 0.208 %/h, about 10 days
  assert(feed.monitor.update(sample).minutes_to_empty == -1);
  sample.raw_soc = 34 * 256; // 34 % at 0.208 %/h is 9807 minutes
  assert(feed.monitor.update(sample).minutes_to_empty == 9807);
  sample.crate_milli_percent_per_hour = -0x8000 * 208;
  assert(feed.monitor.update(sample).minutes_to_empty == 0);
  sample.crate_ok = false;
  assert(feed.monitor.update(sample).minutes_to_empty == -1);
  sample.crate_ok = true;
  sample.raw_soc = 100 * 256 + 1; // above full is not trusted
  a = feed.monitor.update(sample);
  assert(!a.soc_trusted && a.minutes_to_empty == -1);
  sample.raw_soc = 100 * 256;
  assert(feed.monitor.update(sample).soc_trusted);
  sample.soc_ok = false;
  assert(!feed.monitor.update(sample).soc_trusted);

  // External power and unknown state never trust SOC.
  Feed usb;
  for (int i = 0; i < 3; ++i)
    a = usb.at(4267);
  assert(a.state == State::ExternalPowerLikely && !a.soc_trusted);
  Feed fresh;
  assert(!fresh.at(3800).soc_trusted);
}
} // namespace

int main() {
  names_and_codes();
  threshold_validation();
  initial_classification_needs_agreement();
  debounce_and_hysteresis();
  debounce_of_one();
  invalid_samples();
  settling_window();
  soc_trust_and_minutes();
  return 0;
}
