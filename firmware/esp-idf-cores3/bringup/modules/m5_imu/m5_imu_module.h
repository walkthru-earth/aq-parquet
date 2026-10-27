// clang-format off
// GENERATED from firmware/esp-idf-cores3/bringup/modules/m5_imu/module.json by tools/plugin_contract.py.
// Do not edit. Change the manifest and run `pixi run plugin-headers`.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace ossensor::m5_imu {
inline constexpr std::string_view kProcedure = "m5_imu";
enum class Column : std::uint8_t {
  imu_fresh_mask,
  accel_x_g,
  accel_y_g,
  accel_z_g,
  gyro_x_dps,
  gyro_y_dps,
  gyro_z_dps,
  mag_x_raw,
  mag_y_raw,
  mag_z_raw,
  imu_temperature_c,
};
inline constexpr std::array<std::string_view, 11> kColumns{
    "imu_fresh_mask",
    "accel_x_g",
    "accel_y_g",
    "accel_z_g",
    "gyro_x_dps",
    "gyro_y_dps",
    "gyro_z_dps",
    "mag_x_raw",
    "mag_y_raw",
    "mag_z_raw",
    "imu_temperature_c",
};
} // namespace ossensor::m5_imu
// clang-format on
