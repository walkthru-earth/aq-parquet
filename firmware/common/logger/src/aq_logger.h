#pragma once

#include <ble_sync.h>
#include <cstddef>
#include <cstdint>
#include <numeric_sample.h>
#include <parquet_writer.h>

namespace aqlogger {
using Row = aq::NumericSample<telemetry::kMaxColumns>;
struct Field {
  const char *name = nullptr;
  telemetry::PhysicalType type = telemetry::PhysicalType::Int32;
  const char *procedure = nullptr;
  const char *unit = nullptr;
  const char *validity = nullptr;
  const char *property_uri = nullptr;
};
// All pointed-to descriptors and strings must outlive the runtime. The schema
// must include INT64 sequence, monotonic_us, event_time_utc_ns and INT32
// clock_epoch. UTC values remain null until a supported clock anchor exists.
struct Config {
  const Field *fields = nullptr;
  std::size_t column_count = 0;
  std::int32_t schema_version = 0;
  const char *schema_name = nullptr;
  const char *firmware = nullptr;
  // Passed to Writer.finish; nullptr keeps the historical writer default.
  const char *created_by = nullptr;
  const char *dictionary_version = nullptr;
  const char *dictionary_uri = nullptr;
  const char *dictionary_sha256 = nullptr;
  const char *configuration_id = nullptr;
  const char *configuration = nullptr;
  const char *time_semantics = nullptr;
  const char *board = nullptr;
  const char *output_directory = "/sd/output";
  // Optional legacy archive root; no board path is assumed.
  const char *legacy_directory = nullptr;
  const telemetry::KeyValue *metadata = nullptr;
  std::size_t metadata_count = 0;
};
struct Hooks {
  void *context = nullptr;
  // Main loop only. Set hardware fields; runtime supplies identity, clock,
  // memory/storage counters and collection completion after this callback.
  void (*collect)(Row &, std::int64_t now, std::int64_t scheduled,
                  void *context) = nullptr;
  // Storage worker only, after the board has mounted its filesystem.
  std::uint64_t (*storage_total_bytes)(void *) = nullptr;
  std::uint64_t (*storage_used_bytes)(void *) = nullptr;
  // Supply both or neither. Otherwise the engine owns its own bus mutex.
  void (*lock_bus)(void *) = nullptr;
  void (*unlock_bus)(void *) = nullptr;
  // Main loop only. read must reject unset/invalid RTC or voltage-low data;
  // write must verify readback. Values are UTC seconds, never local time.
  bool (*rtc_read)(std::int64_t &utc_seconds, void *) = nullptr;
  bool (*rtc_write)(std::int64_t utc_seconds, void *) = nullptr;
};
// One singleton per firmware image; call once after hardware/storage setup.
bool begin(const Config &, const Hooks &, bool sd_mounted);
void poll();
bool start_links(bool display_detected);
bool enqueue_request(const ble::ControlRequest &);
void lock_bus();
void unlock_bus();
const char *station_text_id();
const char *device_text_id();
const char *firmware_text_id();
std::size_t field_index(const char *name); // column_count when absent
void integer(Row &, const char *name, std::int32_t value);
void counter(Row &, const char *name, std::int64_t value);
void number(Row &, const char *name, float value);
struct ClockView {
  std::int64_t utc_ns = 0;
  std::int32_t epoch = 0;
  std::int32_t source = 0;
  std::int32_t rtc_state = 0;
  const char *source_name = "none";
};
ClockView clock_view();
} // namespace aqlogger
