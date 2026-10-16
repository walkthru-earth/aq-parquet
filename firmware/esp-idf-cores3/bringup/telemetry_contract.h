#pragma once

#include "parquet_writer.h"
#include "telemetry_dictionary_digest.h"
#include <cstdint>
#include <cstring>
#include <numeric_sample.h>
#include <utc_clock.h>

namespace telemetry {
namespace contract {
// File schema v5 keeps the 81 leaves of v4 in the same order, renames the two
// UTC instants to microseconds (`*_utc_us`, TIMESTAMP(MICROS, UTC)) and writes
// a Parquet field_id on every leaf so an Apache Iceberg v3 table can register
// the file. Dictionary v4 tracks the field bytes; schema v5 tracks the reader
// contract.
constexpr std::int32_t kSchemaVersion = 5;
constexpr const char *kSchemaName = "cores3-telemetry-v5";
constexpr const char *kDictionaryVersion = "cores3-telemetry-v4";
constexpr const char *kFirmware = "idf-cores3-parquet-v6.12";
constexpr const char *kDictionaryUri =
    "https://github.com/walkthru-earth/aq-parquet/blob/main/"
    "firmware/esp-idf-cores3/bringup/telemetry_fields.inc";
// Version the acquisition procedure separately from the physical file codec.
constexpr const char *kConfigurationId = "cores3-acquisition-v3";
constexpr const char *kConfiguration =
    "{\"sample_interval_ms\":10000,\"pms_warmup_us\":30000000,"
    "\"pms_stale_after_ms\":5000,\"pms_uart_rx\":18,\"pms_uart_tx\":17,"
    "\"pms_baud\":9600,\"m5unified\":\"0.2.26\",\"esp_idf\":\"6.1\",\"ltr553\":"
    "\"raw-v1\","
    "\"snapshot\":\"latest-available-not-average\"}";
constexpr const char *kUnknown = "unknown";
constexpr const char *kTimeSemantics =
    "event_time_utc_us estimates snapshot start; collection_completed_mono_us "
    "bounds sequential reads; pms_received_mono_us is last checksum-valid UART "
    "frame receipt, not sensor phenomenon time; result/ingestion time and "
    "clock uncertainty unknown; no retroactive UTC assignment";

enum Field : std::size_t {
#define FIELD(name, type, procedure, unit, validity) name,
#include "telemetry_fields.inc"
#undef FIELD
  field_count
};
struct Definition {
  const char *name = nullptr;
  PhysicalType type = PhysicalType::Int32;
  const char *procedure = nullptr;
  const char *unit = nullptr;
  const char *validity = nullptr;
  const char *property_uri = nullptr;
  const char *live_key = nullptr;
};
// LIVE JSON key for each field, or nullptr for fields that stay out of the
// LIVE snapshot. The shared logger emits them in field-table order.
constexpr const char *live_key(Field field) {
  switch (field) {
  case sequence:
    return "seq";
  case monotonic_us:
    return "mono";
  case event_time_utc_us:
    return "utc";
  case pms_status:
    return "pms";
  case pm1_cf1_ug_m3:
    return "c1";
  case pm25_cf1_ug_m3:
    return "c25";
  case pm10_cf1_ug_m3:
    return "c10";
  case pm1_atmospheric_ug_m3:
    return "pm1";
  case pm25_atmospheric_ug_m3:
    return "pm25";
  case pm10_atmospheric_ug_m3:
    return "pm10";
  case particles_gt03_per_01l:
    return "n03";
  case particles_gt05_per_01l:
    return "n05";
  case particles_gt10_per_01l:
    return "n1";
  case particles_gt25_per_01l:
    return "n25";
  case particles_gt50_per_01l:
    return "n5";
  case particles_gt100_per_01l:
    return "n10";
  case imu_temperature_c:
    return "imu_t";
  case light_ch0_raw:
    return "als";
  case vbus_mv:
    return "vbus";
  case battery_mv:
    return "bat";
  case battery_percent:
    return "pct";
  case charging_status:
    return "chg";
  case ambient_temperature_c:
    return "t";
  case relative_humidity_percent:
    return "rh";
  default:
    return nullptr;
  }
}
constexpr Definition kFields[] = {
#define FIELD(name, type, procedure, unit, validity)                           \
  {#name,         PhysicalType::type,                                          \
   #procedure,    unit,                                                        \
   #validity,     "urn:walkthru-earth:cores3:property:" #name,                 \
   live_key(name)},
#include "telemetry_fields.inc"
#undef FIELD
};
static_assert(field_count == 81, "Version the schema when changing fields");
static_assert(field_count <= kMaxColumns, "Parquet schema capacity exceeded");

using Sample = aq::NumericSample<field_count>;

// Fields whose unit is microseconds since the Unix epoch are the UTC instants;
// the annotation changes what readers present, never the stored INT64.
constexpr const char *kUtcMicrosUnit = "us_since_unix_epoch";
inline LogicalType logical_type(const Definition &field) {
  return field.type == PhysicalType::Int64 &&
                 std::strcmp(field.unit, kUtcMicrosUnit) == 0
             ? LogicalType::TimestampMicrosUtc
             : LogicalType::None;
}

template <typename Row>
inline void prepare_columns(Column *columns, Row *rows) {
  for (std::size_t i = 0; i < field_count; ++i)
    columns[i] = Column{
        kFields[i].name,          kFields[i].type,
        &rows[0].data[i],         sizeof(Row),
        &rows[0].valid[i],        sizeof(Row),
        logical_type(kFields[i]), static_cast<std::int32_t>(i + 1)}; // field_id
}

// `clock_status` codes. The anchor pairs an external UTC estimate with a
// device monotonic instant; the code says where that pairing came from.
//   0  no anchor: UTC fields null, rows go to the `unsynced` tree
//   1  host estimate supplied on this boot (serial `parquet time`, BLE/LAN
//      SET_TIME); the same value is written to the BM8563 RTC
//   2  restored at boot from the BM8563 RTC, which only ever holds a value a
//      host or SNTP supplied earlier (whole seconds, plus RTC
//      drift since that sync); a later host sync starts a new epoch
//   3  successful Wi-Fi SNTP response on this boot, microsecond resolution
enum ClockSource : std::int32_t {
  kClockNone = aq::utc::None,
  kClockHost = aq::utc::Host,
  kClockRtc = aq::utc::Rtc,
  kClockNetwork = aq::utc::Network,
};

// Inputs are a single coherent anchor snapshot taken under the clock mutex.
// Caller provides a fresh zero-initialized row. Unknown UTC stays null.
inline void apply_clock(Sample &row, std::int64_t now, std::int64_t mono_anchor,
                        std::int64_t utc_anchor, std::int32_t generation,
                        std::int32_t source = kClockHost) {
  row.integer(clock_status, generation ? source : kClockNone);
  row.integer(clock_epoch, generation);
  if (generation) {
    row.counter(event_time_utc_us,
                aq::utc::estimate_us(now, mono_anchor, utc_anchor));
    row.counter(clock_anchor_mono_us, mono_anchor);
    row.counter(clock_anchor_utc_us, aq::utc::ns_to_us(utc_anchor));
  }
}
} // namespace contract
} // namespace telemetry
