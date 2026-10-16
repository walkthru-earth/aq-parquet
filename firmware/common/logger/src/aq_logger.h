#pragma once

#include <ble_sync.h>
#include <cstddef>
#include <cstdint>
#include <numeric_sample.h>
#include <parquet_writer.h>
#include <sync_service.h>

namespace aqlogger {
using Row = aq::NumericSample<telemetry::kMaxColumns>;
struct Field {
  const char *name = nullptr;
  telemetry::PhysicalType type = telemetry::PhysicalType::Int32;
  const char *procedure = nullptr;
  const char *unit = nullptr;
  const char *validity = nullptr;
  const char *property_uri = nullptr;
  // Short JSON key for the LIVE snapshot, or nullptr to keep the field out of
  // LIVE. Keys are [a-z0-9_]{1,15} and unique within the table.
  const char *live_key = nullptr;
};
// All pointed-to descriptors and strings must outlive the runtime. The schema
// must include INT64 sequence, monotonic_us, event_time_utc_us and INT32
// clock_epoch. UTC values remain null until a supported clock anchor exists.
// INT64 fields with unit "us_since_unix_epoch" are written as
// TIMESTAMP(MICROS, UTC). Each leaf's Parquet field_id is its 1-based position
// in `fields`, so a board appends new fields and never reorders them.
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
  // codec-test copies live here, outside the listed and synced archive.
  const char *benchmark_directory = "/sd/bench";
  const telemetry::KeyValue *metadata = nullptr;
  std::size_t metadata_count = 0;
  // Sync service tuning; nullptr keeps the shared defaults.
  const aqsync::ServiceConfig *sync = nullptr;
  // Storage worker task. Its stack stays in internal RAM: the worker commits
  // NVS (SET_CONFIG, station id), and a flash write disables the cache that a
  // PSRAM stack needs. Size from the hourly DIAG tasks line plus 25 percent.
  unsigned worker_priority = 3;
  std::size_t worker_stack_bytes = 24576;
};
// Shared default for each board's sampling and UI task: above the idle
// housekeeping, below the storage worker (3), the LAN task (4) and the BLE
// error task (4).
constexpr unsigned kBoardTaskPriority = 2;
struct Hooks {
  void *context = nullptr;
  // Main loop only. Set hardware fields; runtime supplies identity, clock,
  // memory/storage counters and collection completion after this callback.
  void (*collect)(Row &, std::int64_t now, std::int64_t scheduled,
                  void *context) = nullptr;
  // Storage worker only, after the board has mounted its filesystem, called
  // with the bus lock held. free returns false when the volume cannot answer.
  std::uint64_t (*storage_total_bytes)(void *) = nullptr;
  std::uint64_t (*storage_used_bytes)(void *) = nullptr;
  bool (*storage_free_bytes)(std::uint64_t &free_bytes, void *) = nullptr;
  std::uint32_t (*storage_cluster_bytes)(void *) = nullptr; // 0 when unknown
  // Optional. Unmount and mount the card again so a reinserted card or space
  // freed on a PC becomes visible. Called by the storage worker with the bus
  // lock held and no file open. Never formats. Returns true when mounted.
  bool (*storage_remount)(void *) = nullptr;
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
