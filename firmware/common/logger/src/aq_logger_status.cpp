#include "aq_logger_status.h"
#include <cstdio>

namespace aqlogger {
std::size_t format_status(const StatusSnapshot &view, char *out,
                          std::size_t capacity) {
  const int written = std::snprintf(
      out, capacity,
      "{\"up_s\":%lu,\"int_s\":%lu,\"buf\":%lu,\"fin\":%lu,\"drop\":%lu,"
      "\"err\":%lu,\"miss\":%lld,\"fail\":%u,\"codec\":\"%s\",\"utc\":%u,"
      "\"gen\":%ld,\"clk\":%ld,\"rtc\":%ld,\"sd\":%u,\"sd_kib\":%lu,"
      "\"sd_used_kib\":%lu,\"heap\":%lu,\"part\":%lu,\"qf\":%lu,"
      "\"qb\":%llu,\"open\":%lu,\"open_rg\":%lu}",
      static_cast<unsigned long>(view.uptime_seconds),
      static_cast<unsigned long>(view.interval_seconds),
      static_cast<unsigned long>(view.buffered),
      static_cast<unsigned long>(view.finalized),
      static_cast<unsigned long>(view.dropped),
      static_cast<unsigned long>(view.errors),
      static_cast<long long>(view.missed), view.failed ? 1U : 0U,
      view.codec == telemetry::Codec::Lz4Raw ? "LZ4_RAW" : "UNCOMPRESSED",
      view.utc ? 1U : 0U, static_cast<long>(view.generation),
      static_cast<long>(view.clock_source), static_cast<long>(view.rtc_state),
      view.storage_ok ? 1U : 0U, static_cast<unsigned long>(view.total_kib),
      static_cast<unsigned long>(view.used_kib),
      static_cast<unsigned long>(view.heap_free),
      static_cast<unsigned long>(view.partials),
      static_cast<unsigned long>(view.quarantined),
      static_cast<unsigned long long>(view.quarantine_bytes),
      static_cast<unsigned long>(view.open_rows),
      static_cast<unsigned long>(view.open_groups));
  return written > 0 && std::size_t(written) < capacity ? std::size_t(written)
                                                        : 0;
}
} // namespace aqlogger
