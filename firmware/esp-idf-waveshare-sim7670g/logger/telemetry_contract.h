#pragma once

#include "telemetry_dictionary_digest.h"
#include <aq_logger.h>
#include <cstring>
#include <pms_frame.h>
#include <utc_clock.h>

namespace telemetry {
namespace contract {
constexpr std::int32_t kSchemaVersion = 3;
constexpr const char *kSchemaName = "waveshare-sim7670g-telemetry-v3";
constexpr const char *kDictionaryVersion = "waveshare-sim7670g-telemetry-v3";
constexpr const char *kFirmware = "idf-waveshare-parquet-v2.3";
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
static_assert(field_count == 56, "Version the schema when changing fields");
static_assert(field_count <= kMaxColumns, "Parquet schema capacity exceeded");
using Sample = aqlogger::Row;

inline LogicalType logical_type(const Definition &field) {
  return field.type == PhysicalType::Int64 &&
                 std::strcmp(field.unit, "ns_since_unix_epoch") == 0
             ? LogicalType::TimestampNanosUtc
             : LogicalType::None;
}
template <typename Row>
inline void prepare_columns(Column *columns, Row *rows) {
  for (std::size_t i = 0; i < field_count; ++i)
    columns[i] = Column{kFields[i].name,         kFields[i].type,
                        &rows[0].data[i],        sizeof(Row),
                        &rows[0].valid[i],       sizeof(Row),
                        logical_type(kFields[i])};
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
  if (!sensor.vendor || !sensor.model || !sensor.serial ||
      std::strcmp(sensor.vendor, "Plantower") != 0 ||
      std::strcmp(sensor.model, "PMS5003T") != 0 ||
      std::strlen(sensor.serial) < 18 || std::strlen(sensor.serial) > 26 ||
      std::strncmp(sensor.serial, "PMS5003T-", 9) != 0)
    return 0;
  std::int64_t code = 0;
  for (const char *p = sensor.serial + 9; *p; ++p) {
    if (*p < '0' || *p > '9')
      return 0;
    code = code * 10 + (*p - '0');
  }
  return code;
}
inline bool exact_batch(const SensorProfile &sensor) {
  return serial_code(sensor) != 0 &&
         std::strncmp(sensor.serial, "PMS5003T-20260408", 17) == 0;
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
// MAX17048 VCELL (0x02) is 78.125 uV/LSB; SOC (0x04) is 1/256 %/LSB.
// The gauge can estimate above 100% near full charge; cap the UI value while
// rejecting a clearly implausible raw result. This board read 107.8% on USB;
// the 120% rejection threshold is a project heuristic, not a datasheet limit.
inline GaugeSnapshot decode_max17048(std::uint16_t vcell, std::uint16_t soc) {
  const auto millivolts = static_cast<std::int32_t>(vcell) * 5 / 64;
  const bool plausible =
      millivolts >= 2500 && millivolts <= 4500 && soc <= 120 * 256;
  const auto percent =
      soc >= 100 * 256 ? 100 : static_cast<std::int32_t>(soc) / 256;
  return {true, plausible, millivolts, percent};
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
