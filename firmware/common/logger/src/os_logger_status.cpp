#include "os_logger_status.h"
#include <cstdint>
#include <cstdio>

namespace oslogger {
namespace {
// Bounds a printed number. Every reachable value lies inside, so only the
// worst-case length changes: the UTC anchor is limited to 2020 to 2100 (13
// digit milliseconds), and the clock age and the missed deadline count are
// nonnegative counts that fit a u32 for 136 years of uptime.
constexpr std::int64_t clamp(std::int64_t value, std::int64_t high) {
  return value < 0 ? 0 : value > high ? high : value;
}
constexpr std::int64_t kMaxUtcMs = 9999999999999LL;
constexpr std::int64_t kMaxU32 = UINT32_MAX;
} // namespace

std::size_t format_status(const StatusSnapshot &view, char *out,
                          std::size_t capacity) {
  char utc_ms[24] = "null", clock_age[24] = "null", last_disconnect[8] = "null";
  if (view.disconnects)
    std::snprintf(last_disconnect, sizeof(last_disconnect), "%u",
                  unsigned(view.last_disconnect));
  if (view.utc) {
    std::snprintf(utc_ms, sizeof(utc_ms), "%lld",
                  static_cast<long long>(clamp(view.utc_ms, kMaxUtcMs)));
    std::snprintf(
        clock_age, sizeof(clock_age), "%lld",
        static_cast<long long>(clamp(view.clock_age_seconds, kMaxU32)));
  }
  const int written = std::snprintf(
      out, capacity,
      "{\"up_s\":%lu,\"int_s\":%lu,\"buf\":%lu,\"fin\":%lu,\"drop\":%lu,"
      "\"err\":%lu,\"miss\":%lld,\"fail\":%u,\"codec\":\"%s\",\"utc\":%u,"
      "\"gen\":%ld,\"clk\":%u,\"rtc\":%u,\"sd\":%u,\"sd_state\":%u,"
      "\"sd_kib\":%lu,"
      "\"sd_used_kib\":%lu,\"heap\":%lu,\"part\":%lu,\"qf\":%lu,"
      "\"qb\":%llu,\"open\":%lu,\"open_rg\":%lu,\"dc\":%u,\"dr\":%s,"
      "\"ls\":%lu,\"utc_ms\":%s,\"clk_age_s\":%s}",
      static_cast<unsigned long>(view.uptime_seconds),
      static_cast<unsigned long>(view.interval_seconds),
      static_cast<unsigned long>(view.buffered),
      static_cast<unsigned long>(view.finalized),
      static_cast<unsigned long>(view.dropped),
      static_cast<unsigned long>(view.errors),
      static_cast<long long>(clamp(view.missed, kMaxU32)),
      view.failed ? 1U : 0U,
      view.codec == telemetry::Codec::Lz4Raw ? "LZ4_RAW" : "UNCOMPRESSED",
      view.utc ? 1U : 0U, static_cast<long>(view.generation),
      unsigned(view.clock_source), unsigned(view.rtc_state),
      view.storage_ok ? 1U : 0U, unsigned(view.sd_state),
      static_cast<unsigned long>(view.total_kib),
      static_cast<unsigned long>(view.used_kib),
      static_cast<unsigned long>(view.heap_free),
      static_cast<unsigned long>(view.partials),
      static_cast<unsigned long>(view.quarantined),
      static_cast<unsigned long long>(view.quarantine_bytes),
      static_cast<unsigned long>(view.open_rows),
      static_cast<unsigned long>(view.open_groups), unsigned(view.disconnects),
      last_disconnect, static_cast<unsigned long>(view.mount_epoch), utc_ms,
      clock_age);
  return written > 0 && std::size_t(written) < capacity ? std::size_t(written)
                                                        : 0;
}
} // namespace oslogger
