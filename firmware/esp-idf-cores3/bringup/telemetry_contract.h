#pragma once

#include "parquet_writer.h"
#include "telemetry_descriptor_digest.h"
#include "telemetry_dictionary_digest.h"
#include "telemetry_template.h"
#include <algorithm>
#include <aq_logger_live.h>
#include <column_semantics.h>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <numeric_sample.h>
#include <span>
#include <string_view>
#include <utc_clock.h>

namespace telemetry {
namespace contract {
// File schema v5 keeps the 81 leaves of v4 in the same order, renames the two
// UTC instants to microseconds (`*_utc_us`, TIMESTAMP(MICROS, UTC)) and writes
// a Parquet field_id on every leaf so an Apache Iceberg v3 table can register
// the file. The dictionary version tracks the field bytes and schema v5 tracks
// the reader contract. Dictionary v5 (firmware v6.13) adds each LIVE key to the
// field table so the digest covers it. Columns are unchanged from v4.
// Dictionary v6 (firmware v6.14) adds per field LIVE decimals, columns stay v5.
constexpr std::int32_t kSchemaVersion = 5;
constexpr const char *kSchemaName = "cores3-telemetry-v5";
constexpr const char *kDictionaryVersion = "cores3-telemetry-v6";
constexpr const char *kFirmware = "idf-cores3-parquet-v6.14";
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
#define FIELD(name, type, procedure, unit, validity, live, decimals) name,
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
  std::uint8_t decimals = 0;
};
// The dictionary carries each LIVE key, so the digest covers it. "" keeps a
// field out of the LIVE snapshot. The shared logger emits keys in
// field-table order.
constexpr const char *live_or_null(const char *key) {
  return key[0] == '\0' ? nullptr : key;
}
// LIVE digits after the point, in the dictionary so the digest covers them.
// Integer fields carry 0, a Float carries 0..kMaxLiveDecimals from its
// sensor resolution.
template <std::size_t N> constexpr bool decimals_fit(const Definition (&f)[N]) {
  return std::all_of(std::begin(f), std::end(f), [](const Definition &field) {
    return field.decimals <=
           (field.type == PhysicalType::Float ? aqlogger::kMaxLiveDecimals : 0);
  });
}
// Column semantics for the STAC template (#84). telemetry_semantics.inc stays
// out of the dict digest. It is the single source of each FIELD property_uri,
// a board neutral id looked up by name at compile time, so only the property
// strings reach the image.
static constexpr aq::semantics::Column kSemantics[] = {
#define SEM(name, ucum, role, cf_standard_name, aggregate, plausible_min,      \
            plausible_max, settle_s, valid_when, property, sensor_vendor,      \
            sensor_model)                                                      \
  {#name,                                                                      \
   ucum,                                                                       \
   aq::semantics::Role::role,                                                  \
   cf_standard_name,                                                           \
   aq::semantics::Aggregate::aggregate,                                        \
   #plausible_min,                                                             \
   #plausible_max,                                                             \
   settle_s,                                                                   \
   valid_when,                                                                 \
   property,                                                                   \
   sensor_vendor,                                                              \
   sensor_model},
#include "telemetry_semantics.inc"
#undef SEM
};
constexpr Definition kFields[] = {
#define FIELD(name, type, procedure, unit, validity, live, decimals)           \
  {#name,                                                                      \
   PhysicalType::type,                                                         \
   #procedure,                                                                 \
   unit,                                                                       \
   #validity,                                                                  \
   aq::semantics::property_of(kSemantics, #name),                              \
   live_or_null(live),                                                         \
   decimals},
#include "telemetry_fields.inc"
#undef FIELD
};
static_assert(field_count == 81, "Version the schema when changing fields");
static_assert(field_count <= kMaxColumns, "Parquet schema capacity exceeded");
static_assert(
    decimals_fit(kFields),
    "decimals is 0 for integer fields and at most kMaxLiveDecimals for Float");

static_assert(aq::semantics::aligned(kFields, kSemantics),
              "telemetry_semantics.inc must list every FIELD in order");
static_assert(aq::semantics::well_formed(kFields, kSemantics),
              "telemetry_semantics.inc has a malformed row");
static_assert(aq::semantics::every_property_set(kFields),
              "every FIELD needs a property from telemetry_semantics.inc");

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

// Status codes collect() writes in telemetry_logger.cpp. telemetry_classes.inc
// names each value for the board template (#85), and the footer legends below
// must list the same pairs.
enum PmsStatus : std::int32_t {
  kPmsMissing = 0,
  kPmsWarming = 1,
  kPmsStale = 2,
  kPmsSensorError = 3,
  kPmsValid = 4,
};
enum LightStatus : std::int32_t {
  kLightUnavailable = 0,
  kLightNotFresh = 1,
  kLightInvalid = 2,
  kLightValid = 3,
  kLightIoError = 4,
};
enum ProximityStatus : std::int32_t {
  kProximityUnavailable = 0,
  kProximityNotFresh = 1,
  kProximitySaturated = 2,
  kProximityValid = 3,
  kProximityIoError = 4,
};
// M5Unified Power_Class::is_charging_t. This header stays free of M5Unified
// for the host fixture, so telemetry_logger.cpp ties the two by static_assert.
enum ChargingStatus : std::int32_t {
  kChargingDischarging = 0,
  kChargingCharging = 1,
  kChargingUnknown = 2,
};
constexpr const char *kPmsStatusLegend =
    "0=missing,1=warming,2=stale,3=sensor-error,4=valid";
constexpr const char *kLightStatusLegend =
    "0=unavailable,1=not-fresh,2=invalid,3=valid,4=io-error";
constexpr const char *kProximityStatusLegend =
    "0=unavailable,1=not-fresh,2=saturated,3=valid,4=io-error";

// classification:classes for the board template (#85), out of the dict
// digest like kSemantics.
static constexpr aq::semantics::Class kClasses[] = {
#define CLASS(column, value, name, nodata)                                     \
  {#column, static_cast<std::int64_t>(value), name, nodata},
#include "telemetry_classes.inc"
#undef CLASS
};
// Code columns with no classes. pms_firmware and pms_error are raw PMSA003
// frame bytes with no published code list, imu_fresh_mask is a bitmask.
static constexpr std::string_view kClasslessColumns[] = {
    "pms_firmware", "pms_error", "imu_fresh_mask"};
static_assert(aq::semantics::classes_complete(kSemantics, kClasses,
                                              kClasslessColumns),
              "every code or boolean column needs telemetry_classes.inc rows");
static_assert(aq::semantics::matches_legend(kClasses, "pms_status",
                                            kPmsStatusLegend) &&
                  aq::semantics::matches_legend(kClasses, "light_status",
                                                kLightStatusLegend) &&
                  aq::semantics::matches_legend(kClasses, "proximity_status",
                                                kProximityStatusLegend),
              "telemetry_classes.inc and the footer status legends differ");
constexpr bool clock_classes_match_source_names() {
  return std::ranges::none_of(kClasses, [](const aq::semantics::Class &entry) {
    return entry.column == "clock_status" &&
           entry.name !=
               aq::utc::source_name(static_cast<std::int32_t>(entry.value));
  });
}
static_assert(clock_classes_match_source_names(),
              "clock_status classes must use aq::utc::source_name");

// No CoreS3 column is a modelResult, so no column needs opensensor:derived.
static constexpr std::span<const aq::semantics::Derived> kDerived{};
static_assert(aq::semantics::derived_complete(kFields, kSemantics, kDerived),
              "every modelResult column needs one kDerived entry");

// Board template title and keywords (#85). The title names the board and its
// sensors by make and model. Keywords are generic search terms.
constexpr const char *kTemplateTitle =
    "M5Stack CoreS3 with Plantower PMSA003, Bosch Sensortec BMI270 and BMM150, "
    "and Lite-On LTR-553ALS-WA";
static constexpr std::string_view kTemplateKeywords[] = {
    "air quality", "particulate matter", "PM2.5", "low-cost sensor"};
// The template's describedby link. Raw text at the dict-bearing path on main,
// so the href stays stable while file:checksum pins the exact bytes.
constexpr const char *kTemplateDictionaryHref =
    "https://raw.githubusercontent.com/walkthru-earth/aq-parquet/main/"
    "firmware/esp-idf-cores3/bringup/telemetry_fields.inc";

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
