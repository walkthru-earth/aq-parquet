#pragma once

#include "parquet_writer.h"
#include "telemetry_dictionary_digest.h"
#include <cstdint>
#include <cstring>
#include <numeric_sample.h>
#include <utc_clock.h>

namespace telemetry {
namespace contract {
// File schema v4 appends four nullable H3 location fields to the unchanged
// original 77 leaves. UTC annotations/statistics and bounded row groups remain.
// Dictionary v3 tracks the field bytes; schema v4 tracks the reader contract.
constexpr std::int32_t kSchemaVersion = 4;
constexpr const char *kSchemaName = "cores3-telemetry-v4";
constexpr const char *kDictionaryVersion = "cores3-telemetry-v3";
constexpr const char *kFirmware = "idf-cores3-parquet-v6.9";
constexpr const char *kDictionaryUri =
    "https://github.com/walkthru-earth/aq-parquet/blob/main/"
    "firmware/esp-idf-cores3/bringup/telemetry_fields.inc";
// Version the acquisition procedure separately from the physical file codec.
constexpr const char *kConfigurationId = "cores3-acquisition-v2";
constexpr const char *kConfiguration =
    "{\"sample_interval_ms\":10000,\"pms_warmup_us\":30000000,"
    "\"pms_stale_after_ms\":5000,\"pms_uart_rx\":18,\"pms_uart_tx\":17,"
    "\"pms_baud\":9600,\"m5unified\":\"0.2.25\",\"esp_idf\":\"6.1\",\"ltr553\":"
    "\"raw-v1\","
    "\"snapshot\":\"latest-available-not-average\"}";
constexpr const char *kUnknown = "unknown";
constexpr const char *kTimeSemantics =
    "event_time_utc_ns estimates snapshot start; collection_completed_mono_us "
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
};
constexpr Definition kFields[] = {
#define FIELD(name, type, procedure, unit, validity)                           \
  {#name, PhysicalType::type, #procedure,                                      \
   unit,  #validity,          "urn:walkthru-earth:cores3:property:" #name},
#include "telemetry_fields.inc"
#undef FIELD
};
static_assert(field_count == 81, "Version the schema when changing fields");
static_assert(field_count <= kMaxColumns, "Parquet schema capacity exceeded");

using Sample = aq::NumericSample<field_count>;

// Fields whose unit is nanoseconds since the Unix epoch are the UTC instants;
// the annotation changes what readers present, never the stored INT64.
constexpr const char *kUtcNanosUnit = "ns_since_unix_epoch";
inline LogicalType logical_type(const Definition &field) {
  return field.type == PhysicalType::Int64 &&
                 std::strcmp(field.unit, kUtcNanosUnit) == 0
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
    row.counter(event_time_utc_ns,
                aq::utc::estimate_ns(now, mono_anchor, utc_anchor));
    row.counter(clock_anchor_mono_us, mono_anchor);
    row.counter(clock_anchor_utc_ns, utc_anchor);
  }
}
} // namespace contract
} // namespace telemetry
