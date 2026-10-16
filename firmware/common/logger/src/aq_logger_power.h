#pragma once

#include <aq_battery_health.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>

// Pure power-state bookkeeping for the logger runtime. No IO, no clocks.
namespace aqlogger::power {

struct Change {
  bool changed = false;  // first report or a different state
  bool finalize = false; // close the open file once for this critical entry
};

// Tracks reported battery states. A critical entry asks for one finalize.
// Unknown readings do not re-arm it, so a flapping gauge read cannot turn
// one low cell into many tiny files. Any other state re-arms it.
class Tracker {
public:
  Change observe(aq::battery::State state) {
    using aq::battery::State;
    Change change;
    change.changed = !seen_ || state != last_;
    if (state == State::Critical) {
      change.finalize = armed_;
      armed_ = false;
    } else if (state != State::Unknown) {
      armed_ = true;
    }
    seen_ = true;
    last_ = state;
    return change;
  }
  aq::battery::State last() const { return last_; }

private:
  aq::battery::State last_ = aq::battery::State::Unknown;
  bool seen_ = false;
  bool armed_ = true;
};

// `POWER STATE state=<name> mv=<n>` with ` action=finalize` when a
// finalize was requested. A negative mV prints as `mv=null`. Returns the
// length without the NUL, or zero when `capacity` is too small.
inline std::size_t format_line(char *out, std::size_t capacity,
                               aq::battery::State state, std::int32_t mv,
                               bool finalize) {
  if (!out || capacity == 0)
    return 0;
  const char *action = finalize ? " action=finalize" : "";
  const char *name = aq::battery::state_name(state);
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

} // namespace aqlogger::power
