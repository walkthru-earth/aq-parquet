// Compile the real disconnect codec and DebugLog event ring. Prints golden
// frames as hex for tools/test_disconnect.py to pin and decode.
#include <debug_log.h>
#include <disconnect.h>
#include <sync_codec.h>

#include <cassert>
#include <cstdio>
#include <string_view>
#include <vector>

// The default board layout (runtime Kconfig), 64 KiB and 64 events.
constexpr DebugLog::Layout kTestLayout{65536, 64};

namespace {
void print_hex(const std::uint8_t *bytes, std::size_t size) {
  for (std::size_t i = 0; i < size; ++i)
    std::printf("%02x", bytes[i]);
  std::printf("\n");
}

ble::DisconnectEvent event(std::uint32_t generation) {
  ble::DisconnectEvent value{};
  value.uptime_s = 100 + generation;
  value.generation = generation;
  value.reason = static_cast<std::uint8_t>(ble::DisconnectReason::PeerClosed);
  value.link = 1;
  value.boot_seq = 2;
  return value;
}
} // namespace

int main() {
  static_assert(sizeof(ble::DisconnectEvent) == 16, "event size");
  static_assert(ble::kDiagHeaderBytes + 61 * 16 <= 1024 &&
                    ble::kDiagHeaderBytes + 62 * 16 > 1024,
                "LAN DIAG holds 61 events");
  static_assert(ble::kDiagHeaderBytes + 29 * 16 <= 512 &&
                    ble::kDiagHeaderBytes + 30 * 16 > 512,
                "BLE DIAG holds 29 events");
  assert(std::string_view(ble::disconnect_reason_name(
             ble::DisconnectReason::KeepaliveFailed)) == "keepalive-failed");
  assert(std::string_view(ble::disconnect_reason_name(
             static_cast<ble::DisconnectReason>(99))) == "unknown");

  // Golden BYE: idle timeout, generation 0x01020304, err -11, 3 queued, 1 h.
  std::uint8_t bye[ble::kByeBytes];
  assert(!ble::encode_bye(bye, sizeof(bye) - 1,
                          ble::DisconnectReason::IdleTimeout, 0, 0, 0, 0));
  assert(ble::encode_bye(bye, sizeof(bye), ble::DisconnectReason::IdleTimeout,
                         0x01020304, -11, 3, 3600) == 14);
  print_hex(bye, sizeof(bye));

  // Golden event round trip.
  ble::DisconnectEvent sample{};
  sample.uptime_s = 120;
  sample.generation = 7;
  sample.reason = static_cast<std::uint8_t>(ble::DisconnectReason::AuthFailed);
  sample.link = 2;
  sample.err = -1;
  sample.queued_frames = 0;
  sample.boot_seq = 2;
  std::uint8_t raw[16];
  ble::encode_event(sample, raw);
  print_hex(raw, sizeof(raw));
  const auto decoded = ble::decode_event(raw);
  assert(decoded.uptime_s == 120 && decoded.generation == 7 &&
         decoded.reason == 3 && decoded.link == 2 && decoded.err == -1 &&
         decoded.boot_seq == 2);

  // Golden DIAG with one event and two non-zero counters.
  std::uint16_t counts[ble::kDisconnectReasons]{};
  counts[1] = 5;
  counts[3] = 1;
  std::uint8_t diag[1024];
  assert(!ble::encode_diag(diag, 37, 2, counts, &sample, 1));
  const auto one = ble::encode_diag(diag, sizeof(diag), 2, counts, &sample, 1);
  assert(one == 54);
  print_hex(diag, one);

  // Truncation keeps the newest events that fit, oldest first.
  std::vector<ble::DisconnectEvent> many;
  for (std::uint32_t i = 0; i < 64; ++i)
    many.push_back(event(i));
  for (const std::size_t capacity : {std::size_t(1024), std::size_t(512)}) {
    const std::size_t n = capacity == 1024 ? 61 : 29;
    const auto length =
        ble::encode_diag(diag, capacity, 9, counts, many.data(), many.size());
    assert(length == 38 + 16 * n && diag[37] == n);
    assert(ble::decode_event(diag + 38).generation == 64 - n);
    assert(ble::decode_event(diag + 38 + 16 * (n - 1)).generation == 63);
  }
  assert(ble::encode_diag(diag, 38, 9, counts, many.data(), many.size()) ==
             38 &&
         diag[37] == 0);

  // Golden ABORTED: idle (op 0) and an interrupted READ.
  std::uint8_t aborted[ble::kAbortedBytes];
  assert(!ble::encode_aborted(aborted, 1, 0));
  assert(ble::encode_aborted(aborted, sizeof(aborted), 0) == 2);
  print_hex(aborted, sizeof(aborted));
  assert(ble::encode_aborted(aborted, sizeof(aborted), 0x03) == 2);
  print_hex(aborted, sizeof(aborted));

  // Recording stamps boot_seq, persists counters and updates STATUS stats.
  std::vector<std::uint32_t> block(DebugLog::persistent_bytes(kTestLayout) / 4 +
                                   1);
  assert(ble::disconnect_stats().count == 0);
  aqlog.attach(block.data(), block.size() * 4, kTestLayout, true);
  const auto recorded =
      ble::record_disconnect(ble::DisconnectReason::SlowPeer, 3, 42, 11, 0,
                             std::int64_t(5) * 1000000 + 999999);
  assert(recorded.uptime_s == 5 && recorded.boot_seq == 1 &&
         recorded.link == 3 && recorded.err == 11);
  ble::record_disconnect(ble::DisconnectReason::PeerClosed, ble::kEventLinkBle,
                         43, 0x13, 0, 6000000);
  const auto stats = ble::disconnect_stats();
  assert(stats.count == 2 && stats.last == ble::DisconnectReason::PeerClosed);
  assert(aqlog.counter(5) == 1 && aqlog.counter(12) == 1);
  const auto snapshot = ble::encode_diag_snapshot(diag, 512);
  assert(snapshot == 38 + 32 && diag[37] == 2);
  assert(aq::sync::get_u16(diag + 1) == 1);
  assert(aq::sync::get_u16(diag + 3 + 2 * 5) == 1);
  assert(ble::decode_event(diag + 38).generation == 42);
  assert(ble::decode_event(diag + 54).err == 0x13);
  assert(ble::encode_diag_snapshot(diag, 37) == 0);

  // BLE controller reasons. NimBLE reports HCI codes above kHciStatusBase.
  using ble::DisconnectReason;
  std::int16_t err = -1;
  assert(ble::map_ble_disconnect(ble::kHciStatusBase + 0x13,
                                 DisconnectReason::Unknown,
                                 err) == DisconnectReason::PeerClosed &&
         err == 0);
  err = -1;
  assert(ble::map_ble_disconnect(ble::kHciStatusBase + 0x08,
                                 DisconnectReason::ServerStop,
                                 err) == DisconnectReason::KeepaliveFailed &&
         err == 0);
  err = -1;
  assert(ble::map_ble_disconnect(ble::kHciStatusBase + 0x16,
                                 DisconnectReason::ServerStop,
                                 err) == DisconnectReason::ServerStop &&
         err == 0);
  // A local terminate without a recorded cause keeps the HCI code.
  assert(ble::map_ble_disconnect(ble::kHciStatusBase + 0x16,
                                 DisconnectReason::Unknown,
                                 err) == DisconnectReason::Unknown &&
         err == 0x16);
  // Unmapped HCI codes keep the raw HCI code, host errors the raw status.
  assert(ble::map_ble_disconnect(ble::kHciStatusBase + 0x3e,
                                 DisconnectReason::ServerStop,
                                 err) == DisconnectReason::Unknown &&
         err == 0x3e);
  assert(ble::map_ble_disconnect(7, DisconnectReason::ServerStop, err) ==
             DisconnectReason::Unknown &&
         err == 7);
  // A 0x13 outside the HCI range is a host status, not a peer close.
  assert(ble::map_ble_disconnect(0x13, DisconnectReason::Unknown, err) ==
             DisconnectReason::Unknown &&
         err == 0x13);

  // A REBOOT that timed out records its own event. A late GAP callback for
  // the same link then records nothing, and the next link records again.
  ble::CloseClaim claim;
  assert(claim.claim(4));
  assert(!claim.claim(4));
  assert(claim.claim(5) && !claim.claim(5));
}
