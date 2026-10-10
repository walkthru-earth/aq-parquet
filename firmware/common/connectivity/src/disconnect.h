#pragma once

// Typed disconnect reasons, the persistent DisconnectEvent record and the
// protocol v3 BYE / DIAG / ABORTED frames (docs/shared/ble-sync-protocol.md,
// "Protocol 3 contract", #protocol-3-contract). Encoders are pure.
// record_disconnect() stores the event in the DebugLog event ring, which
// persists across a warm reboot when the board attached its PSRAM block.
// Events never carry an address, token, PIN or name.

#include <cstddef>
#include <cstdint>

namespace ble {
enum class DisconnectReason : std::uint8_t {
  Unknown = 0,
  IdleTimeout = 1,
  HandshakeTimeout = 2,
  AuthFailed = 3,
  SessionsFull = 4,
  SlowPeer = 5,
  TxOverflow = 6,
  Malformed = 7,
  WifiLost = 8,
  LanDisabled = 9,
  TokenRotated = 10,
  Reboot = 11,
  PeerClosed = 12,
  RecvError = 13,
  SendError = 14,
  ServerStop = 15,
  KeepaliveFailed = 16,
};
constexpr std::size_t kDisconnectReasons = 17;
const char *disconnect_reason_name(DisconnectReason reason);

// `link` values: BLE, a LAN slot (1 to 3), or a refused LAN client that never
// held a slot.
constexpr std::uint8_t kEventLinkBle = 0;
constexpr std::uint8_t kEventLinkRefused = 0xff;

// 16 bytes, little-endian on the wire and in the persistent ring.
struct __attribute__((packed)) DisconnectEvent {
  std::uint32_t uptime_s;
  std::uint32_t generation;
  std::uint8_t reason;
  std::uint8_t link;
  std::int16_t err;
  std::uint16_t queued_frames;
  std::uint16_t boot_seq;
};
static_assert(sizeof(DisconnectEvent) == 16, "DisconnectEvent wire size");
constexpr std::size_t kEventBytes = sizeof(DisconnectEvent);

constexpr std::size_t kByeBytes = 14;        // type + 13-byte body
constexpr std::size_t kDiagHeaderBytes = 38; // type, boot_seq, counts, n
constexpr std::size_t kAbortedBytes = 2;

void encode_event(const DisconnectEvent &event, std::uint8_t *out);
DisconnectEvent decode_event(const std::uint8_t *in);

// Each returns the frame length, or 0 when `capacity` is too small.
std::size_t encode_bye(std::uint8_t *out, std::size_t capacity,
                       DisconnectReason reason, std::uint32_t generation,
                       std::int16_t err, std::uint16_t queued_frames,
                       std::uint32_t uptime_s);
// `events` are oldest first. Sends the newest that fit `capacity`, oldest
// first, so n = min(count, (capacity - 38) / 16).
std::size_t encode_diag(std::uint8_t *out, std::size_t capacity,
                        std::uint16_t boot_seq,
                        const std::uint16_t (&counts)[kDisconnectReasons],
                        const DisconnectEvent *events, std::size_t count);
std::size_t encode_aborted(std::uint8_t *out, std::size_t capacity,
                           std::uint8_t op);

// NimBLE reports a controller disconnect reason as kHciStatusBase plus the
// HCI code. Maps HCI 0x13 (remote user terminated) to PeerClosed, 0x08
// (supervision timeout) to KeepaliveFailed and 0x16 (local host terminated)
// to `local`, the reason this side gave when it asked to terminate. Anything
// else, and 0x16 without a local reason, is Unknown with `err` set to the HCI
// code, or to the raw host status when it is not an HCI code.
constexpr int kHciStatusBase = 0x200;
DisconnectReason map_ble_disconnect(int status, DisconnectReason local,
                                    std::int16_t &err);

// Stores one event (stamped with the current boot_seq) and bumps the
// persistent per-reason counter plus the since-boot STATUS counters.
DisconnectEvent record_disconnect(DisconnectReason reason, std::uint8_t link,
                                  std::uint32_t generation, std::int16_t err,
                                  std::uint16_t queued_frames,
                                  std::int64_t uptime_us);

struct DisconnectStats {
  std::uint32_t count = 0; // since this boot (STATUS `dc`)
  DisconnectReason last = DisconnectReason::Unknown; // valid when count > 0
};
DisconnectStats disconnect_stats();

// DIAG from the persistent ring and counters.
std::size_t encode_diag_snapshot(std::uint8_t *out, std::size_t capacity);
} // namespace ble
