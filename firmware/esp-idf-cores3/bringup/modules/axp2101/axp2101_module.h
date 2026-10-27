// clang-format off
// GENERATED from firmware/esp-idf-cores3/bringup/modules/axp2101/module.json by tools/plugin_contract.py.
// Do not edit. Change the manifest and run `pixi run plugin-headers`.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace ossensor::axp2101 {
inline constexpr std::string_view kProcedure = "m5_power";
enum class Column : std::uint8_t {
  vbus_mv,
  battery_mv,
  battery_percent,
  charging_status,
  battery_current_ma,
  external_5v_enabled,
  usb_output_enabled,
};
inline constexpr std::array<std::string_view, 7> kColumns{
    "vbus_mv",
    "battery_mv",
    "battery_percent",
    "charging_status",
    "battery_current_ma",
    "external_5v_enabled",
    "usb_output_enabled",
};
enum class Segment : std::uint8_t {
  core,
};
inline constexpr std::array<Segment, 7> kColumnSegments{
    Segment::core,
    Segment::core,
    Segment::core,
    Segment::core,
    Segment::core,
    Segment::core,
    Segment::core,
};
} // namespace ossensor::axp2101
// clang-format on
