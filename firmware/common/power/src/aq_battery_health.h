#pragma once

#include <cstdint>

// Pure battery health classification for one fuel-gauged Li-ion cell.
// No IO, no heap, no clocks. The board feeds one Sample per acquisition
// tick and owns every hardware decision. The thresholds are an
// uncalibrated starting point, not a measured discharge curve.
namespace aq::battery {

// Stable integer codes. They are stored in telemetry and must never be
// renumbered. Add new states at the end.
enum class State : std::uint8_t {
  Unknown = 0,             // no committed classification yet
  ExternalPowerLikely = 1, // cell voltage only a supply rail can hold
  Ok = 2,
  Low = 3,
  Critical = 4,
  Settling = 5, // the gauge reset recently, SOC is not trusted yet
};

const char *state_name(State state);

// Calibration entry point. Voltages are the gauge VCELL reading in mV.
// A sample enters a lower level at or below its enter threshold and
// returns once it reaches the enter threshold plus the hysteresis.
struct Thresholds {
  std::int32_t external_power_mv = 4240;
  std::int32_t external_power_hysteresis_mv = 20;
  std::int32_t low_enter_mv = 3500;
  std::int32_t critical_enter_mv = 3350;
  std::int32_t hysteresis_mv = 75;
  std::uint8_t debounce_samples = 3;
  std::uint16_t settling_minutes = 60;

  // Checks ordering and bounds. An invalid set must not be used.
  bool validate() const;
};

// One acquisition tick. `now_ms` is monotonic time.
struct Sample {
  std::int64_t now_ms = 0;
  bool vcell_ok = false; // VCELL read and passed the driver range check
  std::int32_t millivolts = 0;
  bool soc_ok = false;
  std::uint16_t raw_soc = 0; // 1/256 % per LSB, uncapped
  bool crate_ok = false;
  std::int32_t crate_milli_percent_per_hour = 0; // signed
  bool status_ok = false;
  bool reset_indicator = false; // STATUS.RI as read this tick
};

struct Assessment {
  State state = State::Unknown;
  bool settling = false;
  bool soc_trusted = false;
  // Whole minutes until the gauge reaches 0% at the current rate, or -1
  // when the estimate is unavailable or longer than seven days.
  std::int32_t minutes_to_empty = -1;
};

class Monitor {
public:
  explicit Monitor(const Thresholds &thresholds = Thresholds{});

  // False when the thresholds failed validate(). Such a monitor stays
  // Unknown forever.
  bool configured() const { return configured_; }

  // Starts the settling window, for example after STATUS.RI was seen
  // and cleared at boot.
  void note_gauge_reset(std::int64_t now_ms);

  Assessment update(const Sample &sample);

  const Thresholds &thresholds() const { return thresholds_; }

private:
  enum Level : std::int8_t {
    kNone = -1,
    kExternal = 0,
    kOk = 1,
    kLow = 2,
    kCritical = 3,
  };

  Level initial_level(std::int32_t mv) const;
  Level target_level(Level from, std::int32_t mv) const;
  std::int32_t down_mv(int boundary) const;
  std::int32_t up_mv(int boundary) const;
  bool settling_at(std::int64_t now_ms);

  Thresholds thresholds_;
  bool configured_ = false;
  Level level_ = kNone;
  Level pending_ = kNone; // mildest target of the current run
  int pending_direction_ = 0;
  std::uint8_t pending_count_ = 0;
  std::uint8_t invalid_count_ = 0;
  bool reset_seen_ = false;
  bool settling_active_ = false;
  std::int64_t settling_start_ms_ = 0;
};

} // namespace aq::battery
