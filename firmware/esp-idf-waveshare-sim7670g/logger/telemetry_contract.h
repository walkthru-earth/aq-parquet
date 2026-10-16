#pragma once

#include "pms5003t_identity.h"
#include "telemetry_dictionary_digest.h"
#include <aq_logger.h>
#include <aq_max17048.h>
#include <cstring>
#include <pms_frame.h>
#include <utc_clock.h>

namespace telemetry {
namespace contract {
// Schema v4 keeps the 56 leaves of v3 in the same order, renames the two UTC
// instants to microseconds (`*_utc_us`, TIMESTAMP(MICROS, UTC)) and writes a
// Parquet field_id on every leaf so an Apache Iceberg v3 table can register the
// file.
constexpr std::int32_t kSchemaVersion = 4;
constexpr const char *kSchemaName = "waveshare-sim7670g-telemetry-v4";
constexpr const char *kDictionaryVersion = "waveshare-sim7670g-telemetry-v4";
constexpr const char *kFirmware = "idf-waveshare-parquet-v2.6";
constexpr const char *kCreatedBy = "aq-parquet version 0.1";
constexpr const char *kDictionaryUri =
    "https://github.com/walkthru-earth/aq-parquet/blob/main/"
    "firmware/esp-idf-waveshare-sim7670g/logger/telemetry_fields.inc";
constexpr const char *kConfigurationId = "waveshare-sim7670g-acquisition-v2";
constexpr std::int64_t kPmsWarmupUs = 30000000;
constexpr std::uint32_t kPmsStaleAfterMs = 5000;
constexpr const char *kConfiguration =
    "{\"sample_interval_ms\":10000,\"pms_warmup_us\":30000000,"
    "\"pms_stale_after_ms\":5000,\"pms_uart_rx\":1,\"pms_uart_tx\":2,"
    "\"pms_baud\":9600,\"pms_model\":\"PMS5003T\","
    "\"battery_presence\":\"explicit-owner-configuration\","
    "\"battery_percent\":\"integer-percent-truncated-capped-at-100\","
    "\"snapshot\":\"latest-available-not-average\","
    "\"sensor_identity\":\"owner-provisioned-nvs-and-per-row\","
    "\"batch_candidate\":\"off-by-default-not-cairo-validated\"}";
constexpr const char *kUnknown = "unknown";
constexpr const char *kTimeSemantics =
    "event_time_utc_us estimates snapshot start; collection_completed_mono_us "
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
  case ambient_temperature_c:
    return "t";
  case relative_humidity_percent:
    return "rh";
  case battery_mv:
    return "bat";
  case battery_percent:
    return "pct";
  default:
    return nullptr;
  }
}
constexpr Definition kFields[] = {
#define FIELD(name, type, procedure, unit, validity)                           \
  {#name,         PhysicalType::type,                                          \
   #procedure,    unit,                                                        \
   #validity,     "urn:walkthru-earth:waveshare-sim7670g:property:" #name,     \
   live_key(name)},
#include "telemetry_fields.inc"
#undef FIELD
};
static_assert(field_count == 56, "Version the schema when changing fields");
static_assert(field_count <= kMaxColumns, "Parquet schema capacity exceeded");
using Sample = aqlogger::Row;

inline LogicalType logical_type(const Definition &field) {
  return field.type == PhysicalType::Int64 &&
                 std::strcmp(field.unit, "us_since_unix_epoch") == 0
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

enum ClockSource : std::int32_t {
  kClockNone = aq::utc::None,
  kClockHost = aq::utc::Host,
  kClockNetwork = aq::utc::Network,
};
// No RTC is available in this board contract. Caller supplies a coherent
// host or SNTP anchor; each captured row keeps its original epoch permanently.
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
struct SensorProfile {
  const char *vendor = "";
  const char *model = "";
  const char *serial = "";
  bool batch_candidate = false;
};
enum BatchCandidateStatus : std::int32_t {
  kBatchDisabled = 0,
  kBatchIdentityMismatch = 1,
  kBatchSourceUnavailable = 2,
  kBatchCandidate = 3,
};
inline std::int64_t serial_code(const SensorProfile &sensor) {
  return pms5003t::serial_code(sensor.vendor, sensor.model, sensor.serial);
}
inline bool exact_batch(const SensorProfile &sensor) {
  return pms5003t::batch_eligible(sensor.vendor, sensor.model, sensor.serial);
}
// Sensor stays powered: warm-up uses boot uptime. Add a sensor-start anchor
// if sleep/reset control is introduced. Apply only to a fresh, zeroed row.
inline void apply_pms(Sample &row, const PmsSnapshot &pms, std::int64_t now,
                      const SensorProfile &sensor = {}) {
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
  const auto identity = serial_code(sensor);
  if (identity)
    row.counter(sensor_serial_code, identity);
  const auto candidate_status = !sensor.batch_candidate ? kBatchDisabled
                                : !exact_batch(sensor)  ? kBatchIdentityMismatch
                                : status != kPmsValid ? kBatchSourceUnavailable
                                                      : kBatchCandidate;
  row.integer(pm25_batch_candidate_status, candidate_status);
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
  if (candidate_status == kBatchCandidate)
    row.number(pm25_batch_candidate_ug_m3,
               0.003964F * frame.particle_counts[0]);
  row.number(ambient_temperature_c, frame.temperature_deci_c / 10.0F);
  if (frame.humidity_deci_percent <= 1000)
    row.number(relative_humidity_percent, frame.humidity_deci_percent / 10.0F);
}

struct GaugeSnapshot {
  bool battery_installed = false; // explicit owner configuration only
  bool read_ok = false;
  std::int32_t millivolts = 0;
  std::int32_t percent = 0; // whole percent, truncated and capped at 100
};
// Decoding and range checks live in the shared aq::Max17048 driver. This
// board only adds the explicit battery-installed configuration.
inline GaugeSnapshot gauge_snapshot(const aq::Max17048Reading &reading) {
  return {true, reading.valid(), reading.millivolts, reading.percent};
}
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
