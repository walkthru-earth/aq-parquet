#include "os_battery_health.h"

namespace opensensor::battery {
namespace {
constexpr std::int64_t kMaxMinutesToEmpty = 7 * 24 * 60;
constexpr std::uint16_t kFullRawSoc = 100 * 256;
} // namespace

const char *state_name(State state) {
  switch (state) {
  case State::Unknown:
    return "unknown";
  case State::ExternalPowerLikely:
    return "external_power_likely";
  case State::Ok:
    return "ok";
  case State::Low:
    return "low";
  case State::Critical:
    return "critical";
  case State::Settling:
    return "settling";
  }
  return "unknown";
}

bool Thresholds::validate() const {
  return critical_enter_mv >= 2500 && hysteresis_mv >= 10 &&
         hysteresis_mv <= 500 && external_power_hysteresis_mv >= 0 &&
         external_power_hysteresis_mv <= 100 &&
         critical_enter_mv + hysteresis_mv <= low_enter_mv &&
         low_enter_mv + hysteresis_mv <=
             external_power_mv - external_power_hysteresis_mv &&
         external_power_mv <= 4500 && debounce_samples >= 1 &&
         debounce_samples <= 10 && settling_minutes <= 240;
}

Monitor::Monitor(const Thresholds &thresholds)
    : thresholds_(thresholds), configured_(thresholds.validate()) {}

void Monitor::note_gauge_reset(std::int64_t now_ms) {
  if (thresholds_.settling_minutes == 0)
    return;
  settling_active_ = true;
  settling_start_ms_ = now_ms;
}

bool Monitor::settling_at(std::int64_t now_ms) {
  if (!settling_active_)
    return false;
  // Time that runs backwards cannot prove the window elapsed, so the
  // window restarts instead of ending early.
  if (now_ms < settling_start_ms_)
    settling_start_ms_ = now_ms;
  const std::int64_t window =
      static_cast<std::int64_t>(thresholds_.settling_minutes) * 60 * 1000;
  if (now_ms - settling_start_ms_ >= window) {
    settling_active_ = false;
    return false;
  }
  return true;
}

std::int32_t Monitor::down_mv(int boundary) const {
  switch (boundary) {
  case 0:
    return thresholds_.external_power_mv -
           thresholds_.external_power_hysteresis_mv;
  case 1:
    return thresholds_.low_enter_mv;
  default:
    return thresholds_.critical_enter_mv;
  }
}

std::int32_t Monitor::up_mv(int boundary) const {
  switch (boundary) {
  case 0:
    return thresholds_.external_power_mv;
  case 1:
    return thresholds_.low_enter_mv + thresholds_.hysteresis_mv;
  default:
    return thresholds_.critical_enter_mv + thresholds_.hysteresis_mv;
  }
}

Monitor::Level Monitor::initial_level(std::int32_t mv) const {
  if (mv >= thresholds_.external_power_mv)
    return kExternal;
  if (mv <= thresholds_.critical_enter_mv)
    return kCritical;
  if (mv <= thresholds_.low_enter_mv)
    return kLow;
  return kOk;
}

Monitor::Level Monitor::target_level(Level from, std::int32_t mv) const {
  int level = from;
  while (level < kCritical && mv <= down_mv(level))
    ++level;
  if (level == from) {
    while (level > kExternal && mv >= up_mv(level - 1))
      --level;
  }
  return static_cast<Level>(level);
}

Assessment Monitor::update(const Sample &sample) {
  Assessment result;
  if (!configured_)
    return result;

  if (sample.status_ok) {
    if (sample.reset_indicator && !reset_seen_)
      note_gauge_reset(sample.now_ms);
    reset_seen_ = sample.reset_indicator;
  }
  result.settling = settling_at(sample.now_ms);

  const std::uint8_t needed = thresholds_.debounce_samples;
  if (!sample.vcell_ok) {
    pending_ = kNone;
    pending_direction_ = 0;
    pending_count_ = 0;
    boot_critical_ = 0;
    if (invalid_count_ < needed)
      ++invalid_count_;
    if (invalid_count_ >= needed)
      level_ = kNone;
  } else {
    invalid_count_ = 0;
    if (level_ == kNone) {
      // Low and Critical reads at boot form one run, so a cell resting on
      // the critical threshold still leaves Unknown. Critical wins when at
      // least half the run read critical, a tie included. Any other band
      // change restarts the run.
      const Level band = initial_level(sample.millivolts);
      const auto low_side = [](Level level) {
        return level == kLow || level == kCritical;
      };
      if (band == pending_ || (low_side(band) && low_side(pending_))) {
        ++pending_count_;
      } else {
        pending_ = band;
        pending_count_ = 1;
        boot_critical_ = 0;
      }
      if (band == kCritical)
        ++boot_critical_;
      if (low_side(pending_))
        pending_ = 2 * boot_critical_ >= pending_count_ ? kCritical : kLow;
    } else {
      const Level target = target_level(level_, sample.millivolts);
      if (target == level_) {
        pending_ = kNone;
        pending_direction_ = 0;
        pending_count_ = 0;
      } else {
        const int direction = target > level_ ? 1 : -1;
        if (direction == pending_direction_ && pending_ != kNone) {
          ++pending_count_;
          // Commit only the mildest move every sample in the run supports.
          if ((direction > 0 && target < pending_) ||
              (direction < 0 && target > pending_))
            pending_ = target;
        } else {
          pending_direction_ = direction;
          pending_ = target;
          pending_count_ = 1;
        }
      }
    }
    if (pending_ != kNone && pending_count_ >= needed) {
      level_ = pending_;
      pending_ = kNone;
      pending_direction_ = 0;
      pending_count_ = 0;
      boot_critical_ = 0;
    }
  }

  switch (level_) {
  case kNone:
    result.state = State::Unknown;
    break;
  case kExternal:
    result.state = State::ExternalPowerLikely;
    break;
  case kOk:
    result.state = result.settling ? State::Settling : State::Ok;
    break;
  case kLow:
    result.state = State::Low;
    break;
  case kCritical:
    result.state = State::Critical;
    break;
  }

  result.soc_trusted = sample.soc_ok && sample.raw_soc <= kFullRawSoc &&
                       !result.settling &&
                       (level_ == kOk || level_ == kLow || level_ == kCritical);
  if (result.soc_trusted && sample.crate_ok &&
      sample.crate_milli_percent_per_hour < 0) {
    const std::int64_t rate =
        -static_cast<std::int64_t>(sample.crate_milli_percent_per_hour);
    const std::int64_t minutes =
        static_cast<std::int64_t>(sample.raw_soc) * 60 * 1000 / (256 * rate);
    if (minutes <= kMaxMinutesToEmpty)
      result.minutes_to_empty = static_cast<std::int32_t>(minutes);
  }
  return result;
}

} // namespace opensensor::battery
