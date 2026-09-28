#pragma once

#include <cstddef>
#include <cstdint>
#include <parquet_writer.h>

namespace aqlogger {
// Pure snapshot/formatter shared by the runtime and host boundary tests.
struct StatusSnapshot {
  std::uint32_t uptime_seconds = 0;
  std::uint32_t interval_seconds = 0;
  std::uint32_t buffered = 0;
  std::uint32_t finalized = 0;
  std::uint32_t dropped = 0;
  std::uint32_t errors = 0;
  std::int64_t missed = 0;
  bool failed = false;
  telemetry::Codec codec = telemetry::Codec::Uncompressed;
  bool utc = false;
  std::int32_t generation = 0;
  std::int32_t clock_source = 0;
  std::int32_t rtc_state = 0;
  bool storage_ok = false;
  std::uint32_t total_kib = 0;
  std::uint32_t used_kib = 0;
  std::uint32_t heap_free = 0;
  std::uint32_t partials = 0;
  std::uint32_t quarantined = 0;
  std::uint64_t quarantine_bytes = 0;
  std::uint32_t open_rows = 0;
  std::uint32_t open_groups = 0;
};
// Returns zero on insufficient capacity, never reports truncated JSON as valid.
std::size_t format_status(const StatusSnapshot &, char *, std::size_t capacity);
} // namespace aqlogger
