// clang-format off
// GENERATED from firmware/esp-idf-waveshare-sim7670g/logger/modules/waveshare/module.json by tools/plugin_contract.py.
// Do not edit. Change the manifest and run `pixi run plugin-headers`.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace ossensor::waveshare {
inline constexpr std::string_view kProcedure = "owner_provisioned_identity";
enum class Column : std::uint8_t {
  sensor_serial_code,
  pm25_batch_candidate_ug_m3,
  pm25_batch_candidate_status,
};
inline constexpr std::array<std::string_view, 3> kColumns{
    "sensor_serial_code",
    "pm25_batch_candidate_ug_m3",
    "pm25_batch_candidate_status",
};
enum class Segment : std::uint8_t {
  pms_identity,
};
inline constexpr std::array<Segment, 3> kColumnSegments{
    Segment::pms_identity,
    Segment::pms_identity,
    Segment::pms_identity,
};
} // namespace ossensor::waveshare
// clang-format on
