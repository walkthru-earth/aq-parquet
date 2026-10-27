// clang-format off
// GENERATED from firmware/esp-idf-cores3/bringup/modules/bm8563/module.json by tools/plugin_contract.py.
// Do not edit. Change the manifest and run `pixi run plugin-headers`.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace ossensor::bm8563 {
inline constexpr std::string_view kProcedure = "m5_rtc";
enum class Column : std::uint8_t {
  rtc_date_yyyymmdd,
  rtc_time_hhmmss,
  rtc_read_ok,
};
inline constexpr std::array<std::string_view, 3> kColumns{
    "rtc_date_yyyymmdd",
    "rtc_time_hhmmss",
    "rtc_read_ok",
};
enum class Segment : std::uint8_t {
  core,
};
inline constexpr std::array<Segment, 3> kColumnSegments{
    Segment::core,
    Segment::core,
    Segment::core,
};
inline constexpr Column kStatusColumn = Column::rtc_read_ok;
inline constexpr std::int32_t kStatusFailed = 0;
inline constexpr std::int32_t kStatusOk = 1;
} // namespace ossensor::bm8563
// clang-format on
