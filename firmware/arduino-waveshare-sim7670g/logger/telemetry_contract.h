#pragma once

#include "telemetry_dictionary_digest.h"
#include <aq_logger.h>
#include <cstring>
#include <pms_frame.h>
#include <utc_clock.h>

namespace telemetry {
namespace contract {
constexpr std::int32_t kSchemaVersion = 1;
constexpr const char *kSchemaName = "waveshare-sim7670g-telemetry-v1";
constexpr const char *kDictionaryVersion = "waveshare-sim7670g-telemetry-v1";
constexpr const char *kFirmware = "arduino-waveshare-parquet-v1";
constexpr const char *kCreatedBy = "aq-parquet version 0.1";
constexpr const char *kDictionaryUri =
    "https://github.com/walkthru-earth/aq-parquet/blob/main/"
    "firmware/arduino-waveshare-sim7670g/logger/telemetry_fields.inc";
constexpr const char *kConfigurationId = "waveshare-sim7670g-acquisition-v1";
constexpr std::int64_t kPmsWarmupUs = 30000000;
constexpr std::uint32_t kPmsStaleAfterMs = 5000;
constexpr const char *kConfiguration =
    "{\"sample_interval_ms\":10000,\"pms_warmup_us\":30000000,"
    "\"pms_stale_after_ms\":5000,\"pms_uart_rx\":1,\"pms_uart_tx\":2,"
    "\"pms_baud\":9600,\"pms_model\":\"PMS5003T\","
    "\"battery_presence\":\"explicit-owner-configuration\","
    "\"battery_percent\":\"integer-percent-truncated\","
    "\"snapshot\":\"latest-available-not-average\"}";
constexpr const char *kUnknown = "unknown";
constexpr const char *kTimeSemantics =
    "event_time_utc_ns estimates snapshot start; collection_completed_mono_us "
    "bounds sequential reads; pms_received_mono_us is last checksum-valid UART "
    "frame receipt, not sensor phenomenon time; result/ingestion time and "
    "clock uncertainty unknown; no RTC anchor and no retroactive UTC "
    "assignment";

enum Field : std::size_t {
#define FIELD(name, type, procedure, unit, validity) name,
#include "telemetry_fields.inc"
#undef FIELD
  field_count
};
using Definition = aqlogger::Field;
constexpr Definition kFields[] = {
#define FIELD(name, type, procedure, unit, validity)                           \
  {#name,      PhysicalType::type,                                             \
   #procedure, unit,                                                           \
   #validity,  "urn:walkthru-earth:waveshare-sim7670g:property:" #name},
#include "telemetry_fields.inc"
#undef FIELD
};
static_assert(field_count == 49, "Version the schema when changing fields");
static_assert(field_count <= kMaxColumns, "Parquet schema capacity exceeded");
using Sample = aqlogger::Row;

inline LogicalType logical_type(const Definition &field) {
  return field.type == PhysicalType::Int64 &&
                 std::strcmp(field.unit, "ns_since_unix_epoch") == 0
             ? LogicalType::TimestampNanosUtc
             : LogicalType::None;
}
inline void prepare_columns(Column *columns, Sample *rows) {
  for (std::size_t i = 0; i < field_count; ++i)
    columns[i] =
        Column{kFields[i].name,         kFields[i].type,   &rows[0].data[i],
               sizeof(Sample),          &rows[0].valid[i], sizeof(Sample),
               logical_type(kFields[i])};
}

enum ClockSource : std::int32_t {
  kClockNone = aq::utc::None,
  kClockHost = aq::utc::Host,
};
// No RTC is available in this board contract. Caller supplies a coherent
// host anchor; each captured row keeps its original epoch permanently.
inline void apply_clock(Sample &row, std::int64_t now, std::int64_t mono_anchor,
                        std::int64_t utc_anchor, std::int32_t generation) {
  row.integer(clock_status, generation ? kClockHost : kClockNone);
  row.integer(clock_epoch, generation);
  if (generation) {
    row.counter(event_time_utc_ns,
                aq::utc::estimate_ns(now, mono_anchor, utc_anchor));
    row.counter(clock_anchor_mono_us, mono_anchor);
    row.counter(clock_anchor_utc_ns, utc_anchor);
  }
}

struct PmsSnapshot {
  plantower::Frame frame{};
  std::uint32_t age_ms = 0;
  std::int64_t received_mono_us = 0;
  std::uint32_t frames = 0;
  std::uint32_t checksum_errors = 0;
  std::uint32_t length_errors = 0;
  bool present = false;
};
enum PmsStatus : std::int32_t {
  kPmsMissing = 0,
  kPmsWarming = 1,
  kPmsStale = 2,
  kPmsSensorError = 3,
  kPmsValid = 4,
  kPmsModelMismatch = 5,
};
// Sensor stays powered: warm-up uses boot uptime. Add a sensor-start anchor
// if sleep/reset control is introduced. Apply only to a fresh, zeroed row.
inline void apply_pms(Sample &row, const PmsSnapshot &pms, std::int64_t now) {
  const auto &frame = pms.frame;
  const auto status =
      !pms.present                    ? kPmsMissing
      : now < kPmsWarmupUs            ? kPmsWarming
      : pms.age_ms > kPmsStaleAfterMs ? kPmsStale
      : frame.sensor_error            ? kPmsSensorError
      : !frame.has_temperature_humidity || frame.particle_count_fields != 4
          ? kPmsModelMismatch
          : kPmsValid;
  row.integer(pms_status, status);
  row.counter(pms_frames, pms.frames);
  row.counter(pms_checksum_errors, pms.checksum_errors);
  row.counter(pms_length_errors, pms.length_errors);
  if (pms.present) {
    row.counter(pms_age_ms, pms.age_ms);
    row.counter(pms_received_mono_us, pms.received_mono_us);
    row.integer(pms_firmware, frame.firmware_version);
    row.integer(pms_error, frame.sensor_error);
  }
  if (status != kPmsValid)
    return;
  row.integer(pm1_cf1_ug_m3, frame.cf1_pm1);
  row.integer(pm25_cf1_ug_m3, frame.cf1_pm25);
  row.integer(pm10_cf1_ug_m3, frame.cf1_pm10);
  row.integer(pm1_atmospheric_ug_m3, frame.atmospheric_pm1);
  row.integer(pm25_atmospheric_ug_m3, frame.atmospheric_pm25);
  row.integer(pm10_atmospheric_ug_m3, frame.atmospheric_pm10);
  for (std::size_t i = 0; i < 4; ++i)
    row.integer(particles_gt03_per_01l + i, frame.particle_counts[i]);
  row.number(ambient_temperature_c, frame.temperature_deci_c / 10.0F);
  if (frame.humidity_deci_percent <= 1000)
    row.number(relative_humidity_percent, frame.humidity_deci_percent / 10.0F);
}

struct GaugeSnapshot {
  bool battery_installed = false; // explicit owner configuration only
  bool read_ok = false;
  std::int32_t millivolts = 0;
  std::int32_t percent = 0; // whole percent, truncated by adapter
};
inline void apply_gauge(Sample &row, const GaugeSnapshot &gauge) {
  row.integer(gauge_status, !gauge.battery_installed ? 0
                            : !gauge.read_ok         ? 1
                                                     : 2);
  if (!gauge.battery_installed || !gauge.read_ok)
    return;
  if (gauge.millivolts > 0)
    row.integer(battery_mv, gauge.millivolts);
  if (gauge.percent >= 0 && gauge.percent <= 100)
    row.integer(battery_percent, gauge.percent);
}
} // namespace contract
} // namespace telemetry
