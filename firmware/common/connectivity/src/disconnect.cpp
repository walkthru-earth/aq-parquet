#include "disconnect.h"

#include "debug_log.h"
#include "sync_codec.h"

#include <atomic>
#include <cstring>

namespace ble {
namespace {
using aq::sync::get_u16;
using aq::sync::get_u32;
using aq::sync::put_u16;
using aq::sync::put_u32;

constexpr std::uint8_t kFrameByeType = 0x4b;
constexpr std::uint8_t kFrameDiagType = 0x4c;
constexpr std::uint8_t kFrameAbortedType = 0x4d;
static_assert(kEventBytes == DebugLog::kEventBytes, "event ring slot size");
static_assert(kDisconnectReasons <= DebugLog::kCounters, "counter slots");

std::atomic<std::uint32_t> disconnects{0};
std::atomic<std::uint8_t> last_reason{0};
} // namespace

const char *disconnect_reason_name(DisconnectReason reason) {
  static constexpr const char *kNames[kDisconnectReasons] = {
      "unknown",         "idle-timeout", "handshake-timeout", "auth-failed",
      "sessions-full",   "slow-peer",    "tx-overflow",       "malformed",
      "wifi-lost",       "lan-disabled", "token-rotated",     "reboot",
      "peer-closed",     "recv-error",   "send-error",        "server-stop",
      "keepalive-failed"};
  const auto index = static_cast<std::size_t>(reason);
  return index < kDisconnectReasons ? kNames[index] : "unknown";
}

void encode_event(const DisconnectEvent &event, std::uint8_t *out) {
  put_u32(out, event.uptime_s);
  put_u32(out + 4, event.generation);
  out[8] = event.reason;
  out[9] = event.link;
  put_u16(out + 10, static_cast<std::uint16_t>(event.err));
  put_u16(out + 12, event.queued_frames);
  put_u16(out + 14, event.boot_seq);
}

DisconnectEvent decode_event(const std::uint8_t *in) {
  DisconnectEvent event{};
  event.uptime_s = get_u32(in);
  event.generation = get_u32(in + 4);
  event.reason = in[8];
  event.link = in[9];
  event.err = static_cast<std::int16_t>(get_u16(in + 10));
  event.queued_frames = get_u16(in + 12);
  event.boot_seq = get_u16(in + 14);
  return event;
}

std::size_t encode_bye(std::uint8_t *out, std::size_t capacity,
                       DisconnectReason reason, std::uint32_t generation,
                       std::int16_t err, std::uint16_t queued_frames,
                       std::uint32_t uptime_s) {
  if (!out || capacity < kByeBytes)
    return 0;
  out[0] = kFrameByeType;
  out[1] = static_cast<std::uint8_t>(reason);
  put_u32(out + 2, generation);
  put_u16(out + 6, static_cast<std::uint16_t>(err));
  put_u16(out + 8, queued_frames);
  put_u32(out + 10, uptime_s);
  return kByeBytes;
}

std::size_t encode_diag(std::uint8_t *out, std::size_t capacity,
                        std::uint16_t boot_seq,
                        const std::uint16_t (&counts)[kDisconnectReasons],
                        const DisconnectEvent *events, std::size_t count) {
  if (!out || capacity < kDiagHeaderBytes || (count && !events))
    return 0;
  std::size_t fit = (capacity - kDiagHeaderBytes) / kEventBytes;
  if (fit > 0xff)
    fit = 0xff; // `n` is a u8
  const std::size_t n = count < fit ? count : fit;
  out[0] = kFrameDiagType;
  put_u16(out + 1, boot_seq);
  for (std::size_t i = 0; i < kDisconnectReasons; ++i)
    put_u16(out + 3 + 2 * i, counts[i]);
  out[kDiagHeaderBytes - 1] = static_cast<std::uint8_t>(n);
  const DisconnectEvent *newest = events + (count - n);
  for (std::size_t i = 0; i < n; ++i)
    encode_event(newest[i], out + kDiagHeaderBytes + i * kEventBytes);
  return kDiagHeaderBytes + n * kEventBytes;
}

std::size_t encode_aborted(std::uint8_t *out, std::size_t capacity,
                           std::uint8_t op) {
  if (!out || capacity < kAbortedBytes)
    return 0;
  out[0] = kFrameAbortedType;
  out[1] = op;
  return kAbortedBytes;
}

DisconnectReason map_ble_disconnect(int status, DisconnectReason local,
                                    std::int16_t &err) {
  constexpr int kRemoteUserTerminated = 0x13;
  constexpr int kSupervisionTimeout = 0x08;
  constexpr int kLocalHostTerminated = 0x16;
  const bool hci = status >= kHciStatusBase && status < kHciStatusBase + 0x100;
  const int code = hci ? status - kHciStatusBase : status;
  DisconnectReason reason = DisconnectReason::Unknown;
  if (hci && code == kRemoteUserTerminated)
    reason = DisconnectReason::PeerClosed;
  else if (hci && code == kSupervisionTimeout)
    reason = DisconnectReason::KeepaliveFailed;
  else if (hci && code == kLocalHostTerminated)
    reason = local;
  err = 0;
  if (reason == DisconnectReason::Unknown)
    err = static_cast<std::int16_t>(code < -32768  ? -32768
                                    : code > 32767 ? 32767
                                                   : code);
  return reason;
}

DisconnectEvent record_disconnect(DisconnectReason reason, std::uint8_t link,
                                  std::uint32_t generation, std::int16_t err,
                                  std::uint16_t queued_frames,
                                  std::int64_t uptime_us) {
  DisconnectEvent event{};
  event.uptime_s =
      uptime_us > 0 ? static_cast<std::uint32_t>(uptime_us / 1000000) : 0;
  event.generation = generation;
  event.reason = static_cast<std::uint8_t>(reason);
  event.link = link;
  event.err = err;
  event.queued_frames = queued_frames;
  event.boot_seq = aqlog.boot_seq();
  std::uint8_t bytes[DebugLog::kEventBytes];
  encode_event(event, bytes);
  aqlog.record_event(bytes, event.reason);
  last_reason.store(event.reason, std::memory_order_relaxed);
  disconnects.fetch_add(1, std::memory_order_release);
  return event;
}

DisconnectStats disconnect_stats() {
  DisconnectStats stats;
  stats.count = disconnects.load(std::memory_order_acquire);
  stats.last = static_cast<DisconnectReason>(
      last_reason.load(std::memory_order_relaxed));
  return stats;
}

std::size_t encode_diag_snapshot(std::uint8_t *out, std::size_t capacity) {
  if (!out || capacity < kDiagHeaderBytes)
    return 0;
  std::size_t fit = (capacity - kDiagHeaderBytes) / kEventBytes;
  if (fit > 0xff)
    fit = 0xff; // `n` is a u8
  // The ring stores the wire layout, so the newest `fit` events copy straight
  // into the frame.
  const std::size_t n =
      aqlog.events(reinterpret_cast<std::uint8_t (*)[DebugLog::kEventBytes]>(
                       out + kDiagHeaderBytes),
                   fit);
  out[0] = kFrameDiagType;
  put_u16(out + 1, aqlog.boot_seq());
  for (std::size_t i = 0; i < kDisconnectReasons; ++i)
    put_u16(out + 3 + 2 * i, aqlog.counter(i));
  out[kDiagHeaderBytes - 1] = static_cast<std::uint8_t>(n);
  return kDiagHeaderBytes + n * kEventBytes;
}
} // namespace ble
