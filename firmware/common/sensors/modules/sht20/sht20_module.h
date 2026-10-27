// clang-format off
// GENERATED from firmware/common/sensors/modules/sht20/module.json by tools/plugin_contract.py.
// Do not edit. Change the manifest and run `pixi run plugin-headers`.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace ossensor::sht20 {
inline constexpr std::string_view kProcedure = "sht20_unresolved";
enum class Column : std::uint8_t {
  ambient_temperature_c,
  relative_humidity_percent,
};
inline constexpr std::array<std::string_view, 2> kColumns{
    "ambient_temperature_c",
    "relative_humidity_percent",
};
enum class Segment : std::uint8_t {
  core,
};
inline constexpr std::array<Segment, 2> kColumnSegments{
    Segment::core,
    Segment::core,
};
} // namespace ossensor::sht20
// clang-format on
