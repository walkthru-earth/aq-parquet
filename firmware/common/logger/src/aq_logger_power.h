#pragma once

#include "aq_logger_emergency_close.h"

#include <aq_battery_health.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>

// Pure power-state bookkeeping for the logger runtime. No IO, no clocks.
namespace oslogger::power {

struct Change {
  bool changed = false;  // first report or a different state
  bool finalize = false; // close the open file once for this critical entry
};

// Tracks reported battery states. A critical entry asks for one finalize,
// so one discharge closes the open file once. Only a real recovery re-arms
// it, Ok (the cell rose past the low threshold plus hysteresis) or external
// power. A rebound to Low, an Unknown read or a Settling gauge does not, so
// a cell hovering near the critical threshold cannot cut many tiny files.
// Settling also covers a gauge reset seen while running, so a recovery in
// that window re-arms only once settling ends and Ok is reported.
class Tracker {
public:
  static bool rearms(opensensor::battery::State state) {
    using opensensor::battery::State;
    return state == State::Ok || state == State::ExternalPowerLikely;
  }

  Change observe(opensensor::battery::State state) {
    using opensensor::battery::State;
    Change change;
    change.changed = !seen_ || state != last_;
    if (state == State::Critical) {
      change.finalize = armed_;
      armed_ = false;
    } else if (rearms(state)) {
      armed_ = true;
    }
    seen_ = true;
    last_ = state;
    return change;
  }
  opensensor::battery::State last() const { return last_; }

private:
  opensensor::battery::State last_ = opensensor::battery::State::Unknown;
  bool seen_ = false;
  bool armed_ = true;
};

// The main loop side of report_battery(). A critical entry raises the
// storage worker's close intent and then wakes the worker. It never touches
// the card and there is no queue to fill.
template <class Wake>
Change report(Tracker &tracker, EmergencyClose &close,
              opensensor::battery::State state, Wake &&wake) {
  const Change change = tracker.observe(state);
  if (change.finalize) {
    close.request();
    wake();
  }
  return change;
}

// `POWER STATE state=<name> mv=<n>` with ` action=finalize` when a
// finalize was requested. A negative mV prints as `mv=null`. Returns the
// length without the NUL, or zero when `capacity` is too small.
inline std::size_t format_line(char *out, std::size_t capacity,
                               opensensor::battery::State state,
                               std::int32_t mv, bool finalize) {
  if (!out || capacity == 0)
    return 0;
  const char *action = finalize ? " action=finalize" : "";
  const char *name = opensensor::battery::state_name(state);
  const int written =
      mv < 0 ? std::snprintf(out, capacity, "POWER STATE state=%s mv=null%s",
                             name, action)
             : std::snprintf(out, capacity, "POWER STATE state=%s mv=%ld%s",
                             name, static_cast<long>(mv), action);
  if (written < 0 || static_cast<std::size_t>(written) >= capacity) {
    out[0] = '\0';
    return 0;
  }
  return static_cast<std::size_t>(written);
}

} // namespace oslogger::power
