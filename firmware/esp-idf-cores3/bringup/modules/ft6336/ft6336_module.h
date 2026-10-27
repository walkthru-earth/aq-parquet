// clang-format off
// GENERATED from firmware/esp-idf-cores3/bringup/modules/ft6336/module.json by tools/plugin_contract.py.
// Do not edit. Change the manifest and run `pixi run plugin-headers`.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace ossensor::ft6336 {
inline constexpr std::string_view kProcedure = "m5_touch";
enum class Column : std::uint8_t {
  touch_points,
  touch_x_px,
  touch_y_px,
};
inline constexpr std::array<std::string_view, 3> kColumns{
    "touch_points",
    "touch_x_px",
    "touch_y_px",
};
enum class Segment : std::uint8_t {
  core,
};
inline constexpr std::array<Segment, 3> kColumnSegments{
    Segment::core,
    Segment::core,
    Segment::core,
};
} // namespace ossensor::ft6336
// clang-format on
