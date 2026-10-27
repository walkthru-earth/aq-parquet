// clang-format off
// GENERATED from firmware/common/sensors/modules/max17048/module.json by tools/plugin_contract.py.
// Do not edit. Change the manifest and run `pixi run plugin-headers`.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace ossensor::max17048 {
inline constexpr std::string_view kProcedure = "max17048";
enum class Column : std::uint8_t {
  gauge_status,
  battery_mv,
  battery_percent,
  battery_soc_raw_percent,
  battery_charge_rate_percent_per_h,
  gauge_status_flags,
  gauge_hibernating,
};
inline constexpr std::array<std::string_view, 7> kColumns{
    "gauge_status",
    "battery_mv",
    "battery_percent",
    "battery_soc_raw_percent",
    "battery_charge_rate_percent_per_h",
    "gauge_status_flags",
    "gauge_hibernating",
};
inline constexpr Column kStatusColumn = Column::gauge_status;
inline constexpr std::int32_t kStatusBatteryNotInstalledOrDisabled = 0;
inline constexpr std::int32_t kStatusReadErrorOrImplausible = 1;
inline constexpr std::int32_t kStatusValid = 2;
} // namespace ossensor::max17048
// clang-format on
