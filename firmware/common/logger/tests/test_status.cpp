#include <aq_logger_status.h>
#include <array>
#include <ble_sync.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>

int main() {
  // Exercise the actual policy called by both snapshot publishers.
  assert(!ble::snapshot_notification_fits(0, 517));
  assert(!ble::snapshot_notification_fits(1, 0));
  assert(!ble::snapshot_notification_fits(1, 2));
  assert(ble::snapshot_notification_fits(20, 23));
  assert(!ble::snapshot_notification_fits(21, 23));
  assert(ble::snapshot_notification_fits(244, 247));
  assert(!ble::snapshot_notification_fits(245, 247));
  assert(ble::snapshot_notification_fits(480, 483));
  assert(!ble::snapshot_notification_fits(480, 482));
  assert(ble::snapshot_notification_fits(480, 517));
  assert(!ble::snapshot_notification_fits(481, 517));
  assert(!ble::snapshot_notification_fits(
      std::numeric_limits<std::size_t>::max(), 517));
  aqlogger::StatusSnapshot view;
  const auto maximum = std::numeric_limits<std::uint32_t>::max();
  view.uptime_seconds = view.interval_seconds = view.buffered = view.finalized =
      view.dropped = view.errors = view.total_kib = view.used_kib =
          view.heap_free = view.partials = view.quarantined = view.open_rows =
              view.open_groups = maximum;
  // Include the extra minus sign even though runtime miss/generation counts
  // are nonnegative: this also covers every signed representable length.
  view.missed = std::numeric_limits<std::int64_t>::min();
  view.generation = view.clock_source = view.rtc_state =
      std::numeric_limits<std::int32_t>::min();
  view.quarantine_bytes = std::numeric_limits<std::uint64_t>::max();
  view.failed = view.utc = view.storage_ok = true;
  std::array<char, ble::kMaxJson + 16> buffer{};
  for (const auto codec :
       {telemetry::Codec::Uncompressed, telemetry::Codec::Lz4Raw}) {
    view.codec = codec;
    const auto length =
        aqlogger::format_status(view, buffer.data(), buffer.size());
    assert(length && length <= ble::kMaxJson);
    assert(!ble::snapshot_notification_fits(length, 23));
    assert(!ble::snapshot_notification_fits(length, 247));
    assert(ble::snapshot_notification_fits(length, 517));
    assert(length == std::strlen(buffer.data()));
    assert(buffer[length - 1] == '}');
    std::cout << buffer.data() << '\n';
    const auto original = buffer;
    assert(aqlogger::format_status(view, buffer.data(), length) == 0);
    assert(buffer[length - 1] == '\0');
    assert(aqlogger::format_status(view, buffer.data(), length + 1) == length);
    assert(buffer == original);
    assert(aqlogger::format_status(view, buffer.data(), 1) == 0);
    assert(buffer[0] == '\0');
    assert(aqlogger::format_status(view, nullptr, 0) == 0);
  }
}
