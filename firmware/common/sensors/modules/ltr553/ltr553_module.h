// clang-format off
// GENERATED from firmware/common/sensors/modules/ltr553/module.json by tools/plugin_contract.py.
// Do not edit. Change the manifest and run `pixi run plugin-headers`.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace ossensor::ltr553 {
inline constexpr std::string_view kProcedure = "ltr553";
enum class Column : std::uint8_t {
  light_ch0_raw,
  light_ch1_raw,
  proximity_raw,
  light_status,
  proximity_status,
};
inline constexpr std::array<std::string_view, 5> kColumns{
    "light_ch0_raw",
    "light_ch1_raw",
    "proximity_raw",
    "light_status",
    "proximity_status",
};
enum class Segment : std::uint8_t {
  core,
};
inline constexpr std::array<Segment, 5> kColumnSegments{
    Segment::core,
    Segment::core,
    Segment::core,
    Segment::core,
    Segment::core,
};
inline constexpr Column kStatusColumn = Column::light_status;
inline constexpr std::int32_t kStatusUnavailable = 0;
inline constexpr std::int32_t kStatusNotFresh = 1;
inline constexpr std::int32_t kStatusInvalid = 2;
inline constexpr std::int32_t kStatusValid = 3;
inline constexpr std::int32_t kStatusIoError = 4;
} // namespace ossensor::ltr553
// clang-format on
