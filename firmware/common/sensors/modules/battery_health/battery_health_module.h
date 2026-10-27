// clang-format off
// GENERATED from firmware/common/sensors/modules/battery_health/module.json by tools/plugin_contract.py.
// Do not edit. Change the manifest and run `pixi run plugin-headers`.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace ossensor::battery_health {
inline constexpr std::string_view kProcedure = "battery_health";
enum class Column : std::uint8_t {
  battery_health_state,
  battery_minutes_to_empty,
};
inline constexpr std::array<std::string_view, 2> kColumns{
    "battery_health_state",
    "battery_minutes_to_empty",
};
enum class Segment : std::uint8_t {
  core,
};
inline constexpr std::array<Segment, 2> kColumnSegments{
    Segment::core,
    Segment::core,
};
inline constexpr Column kStatusColumn = Column::battery_health_state;
inline constexpr std::int32_t kStatusUnknown = 0;
inline constexpr std::int32_t kStatusExternalPowerLikely = 1;
inline constexpr std::int32_t kStatusOk = 2;
inline constexpr std::int32_t kStatusLow = 3;
inline constexpr std::int32_t kStatusCritical = 4;
inline constexpr std::int32_t kStatusSettling = 5;
} // namespace ossensor::battery_health
// clang-format on
