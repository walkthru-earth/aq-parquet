// clang-format off
// GENERATED from firmware/common/sensors/modules/pmsa003/module.json by tools/plugin_contract.py.
// Do not edit. Change the manifest and run `pixi run plugin-headers`.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace ossensor::pmsa003 {
inline constexpr std::string_view kProcedure = "pmsa003";
enum class Column : std::uint8_t {
  pms_status,
  pms_age_ms,
  pm1_cf1_ug_m3,
  pm25_cf1_ug_m3,
  pm10_cf1_ug_m3,
  pm1_atmospheric_ug_m3,
  pm25_atmospheric_ug_m3,
  pm10_atmospheric_ug_m3,
  particles_gt03_per_01l,
  particles_gt05_per_01l,
  particles_gt10_per_01l,
  particles_gt25_per_01l,
  particles_gt50_per_01l,
  particles_gt100_per_01l,
  pms_firmware,
  pms_error,
  pms_frames,
  pms_checksum_errors,
  pms_length_errors,
  pms_received_mono_us,
};
inline constexpr std::array<std::string_view, 20> kColumns{
    "pms_status",
    "pms_age_ms",
    "pm1_cf1_ug_m3",
    "pm25_cf1_ug_m3",
    "pm10_cf1_ug_m3",
    "pm1_atmospheric_ug_m3",
    "pm25_atmospheric_ug_m3",
    "pm10_atmospheric_ug_m3",
    "particles_gt03_per_01l",
    "particles_gt05_per_01l",
    "particles_gt10_per_01l",
    "particles_gt25_per_01l",
    "particles_gt50_per_01l",
    "particles_gt100_per_01l",
    "pms_firmware",
    "pms_error",
    "pms_frames",
    "pms_checksum_errors",
    "pms_length_errors",
    "pms_received_mono_us",
};
inline constexpr Column kStatusColumn = Column::pms_status;
inline constexpr std::int32_t kStatusMissing = 0;
inline constexpr std::int32_t kStatusWarming = 1;
inline constexpr std::int32_t kStatusStale = 2;
inline constexpr std::int32_t kStatusSensorError = 3;
inline constexpr std::int32_t kStatusValid = 4;
} // namespace ossensor::pmsa003
// clang-format on
