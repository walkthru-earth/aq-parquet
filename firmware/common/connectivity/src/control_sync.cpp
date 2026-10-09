#include "control_sync.h"
#include "debug_log.h"
#include "device_config.h"
#include "wifi_link.h"
#include <sync_codec.h>

#include <cstdio>
#include <cstring>

namespace aqsync {
namespace {
using namespace aq::sync;
constexpr std::size_t kPayloadCapacity = 1024;

void send_config(ble::Link link, ble::Op op, const ControlReplies &replies,
                 std::uint8_t page = 0) {
  const lan::Status wifi = lan::status();
  config::WifiView view{wifi.state,   wifi.ip,   wifi.rssi,        wifi.mac,
                        wifi.clients, wifi.host, ble::link().bonds};
  std::uint8_t frame[2 + ble::kMaxJson];
  std::size_t length =
      page ? config::build_page(reinterpret_cast<char *>(frame + 2),
                                ble::kMaxJson, page)
           : config::build_json(reinterpret_cast<char *>(frame + 2),
                                ble::kMaxJson, view);
  if (page == 2 && length >= 2) {
    const auto time = aq::network_time::status();
    char age[24] = "null";
    if (time.has_anchor)
      std::snprintf(age, sizeof(age), "%llu",
                    static_cast<unsigned long long>(time.age_s));
    const auto prefix = length - 2; // append inside the existing ntp object
    const int added = std::snprintf(
        reinterpret_cast<char *>(frame + 2) + prefix, ble::kMaxJson - prefix,
        ",\"state\":\"%s\",\"running\":%u,\"anchored\":%u,\"age_s\":%s,"
        "\"stale\":%u,\"source\":%ld,\"last_public\":%u,\"sync_count\":%lu,"
        "\"error\":%d}}",
        time.state, time.running ? 1U : 0U, time.has_anchor ? 1U : 0U, age,
        time.stale ? 1U : 0U, static_cast<long>(time.source),
        time.public_active ? 1U : 0U,
        static_cast<unsigned long>(time.sync_count), time.error);
    length =
        added > 0 && static_cast<std::size_t>(added) < ble::kMaxJson - prefix
            ? prefix + static_cast<std::size_t>(added)
            : 0;
  }
  if (!length) {
    replies.error(replies.context, link, op, ble::kErrMalformed, "json");
    return;
  }
  frame[0] = ble::kFrameConfig;
  frame[1] = config::reboot_required() ? 1 : 0;
  const auto payload_max = replies.payload_max(replies.context, link);
  if (link == ble::Link::Lan || 2 + length <= payload_max) {
    replies.respond(replies.context, link, frame, 2 + length);
    return;
  }
  constexpr std::size_t kHeader = 6;
  if (payload_max <= kHeader) {
    replies.error(replies.context, link, op, ble::kErrBusy, "mtu");
    return;
  }
  std::uint8_t chunk[ble::kMaxFrame];
  chunk[0] = ble::kFrameConfigChunk;
  chunk[1] = frame[1];
  put_u16(chunk + 2, static_cast<std::uint16_t>(length));
  constexpr std::size_t kSliceCap = 128;
  const auto slice_max =
      payload_max - kHeader < kSliceCap ? payload_max - kHeader : kSliceCap;
  for (std::size_t offset = 0; offset < length; offset += slice_max) {
    const auto count =
        length - offset < slice_max ? length - offset : slice_max;
    put_u16(chunk + 4, static_cast<std::uint16_t>(offset));
    std::memcpy(chunk + kHeader, frame + 2 + offset, count);
    if (!replies.respond(replies.context, link, chunk, kHeader + count))
      return;
  }
}

void send_log_tail(ble::Link link, std::uint16_t max_bytes,
                   const ControlReplies &replies) {
  static char text[DebugLog::kRingBytes];
  std::uint32_t total = 0;
  const std::size_t wanted =
      max_bytes > DebugLog::kRingBytes ? DebugLog::kRingBytes : max_bytes;
  const std::size_t count = aqlog.tail(text, wanted, total);
  std::size_t payload_max = replies.payload_max(replies.context, link);
  const std::size_t cap =
      link == ble::Link::Ble ? ble::kMaxFrame : kPayloadCapacity;
  if (payload_max > cap)
    payload_max = cap;
  std::uint8_t frame[1 + kPayloadCapacity];
  std::size_t sent = 0;
  if (payload_max > 1) {
    const std::size_t slice_max = payload_max - 1;
    while (sent < count) {
      const std::size_t slice =
          count - sent < slice_max ? count - sent : slice_max;
      frame[0] = ble::kFrameLog;
      std::memcpy(frame + 1, text + sent, slice);
      if (!replies.respond(replies.context, link, frame, 1 + slice))
        break;
      sent += slice;
    }
  }
  std::uint8_t end[7];
  end[0] = ble::kFrameLogEnd;
  put_u32(end + 1, total);
  put_u16(end + 5, static_cast<std::uint16_t>(sent));
  replies.respond(replies.context, link, end, sizeof(end));
}
} // namespace

bool handle_common_control(const ble::ControlRequest &request,
                           const ControlReplies &replies) {
  if (!request.length || request.length > ble::kMaxControlBytes ||
      !replies.respond || !replies.error || !replies.payload_max)
    return false;
  const std::uint8_t *body = request.bytes + 1;
  const std::size_t length = request.length - 1;
  switch (request.bytes[0]) {
  case ble::kOpGetConfig:
    if (length > 1 || (length == 1 && body[0] > 4))
      replies.error(replies.context, request.link, ble::kOpGetConfig,
                    ble::kErrMalformed, "page");
    else
      send_config(request.link, ble::kOpGetConfig, replies,
                  length ? body[0] : 0);
    return true;
  case ble::kOpSetConfig: {
    char bad_key[48];
    config::Actions actions;
    if (!config::apply_lines(reinterpret_cast<const char *>(body), length,
                             bad_key, sizeof(bad_key), actions)) {
      replies.error(replies.context, request.link, ble::kOpSetConfig,
                    ble::kErrInvalidConfig, bad_key);
      return true;
    }
    if (actions.clear_bonds)
      ble::clear_bonds();
    if (actions.rotate_token)
      lan::drop_session();
    if (actions.wifi_changed)
      lan::apply_settings();
    else if (actions.ntp_changed)
      lan::apply_time_settings();
    send_config(request.link, ble::kOpSetConfig, replies);
    return true;
  }
  case ble::kOpLogTail:
    if (length != 2)
      replies.error(replies.context, request.link, ble::kOpLogTail,
                    ble::kErrMalformed, nullptr);
    else
      send_log_tail(request.link, get_u16(body), replies);
    return true;
  case ble::kOpGetToken: {
    if (request.link != ble::Link::Ble) {
      replies.error(replies.context, request.link, ble::kOpGetToken,
                    ble::kErrNotOnThisLink, "ble-only");
      return true;
    }
    const config::Settings settings = config::get();
    std::uint8_t frame[3 + config::kTokenBytes];
    frame[0] = ble::kFrameToken;
    put_u16(frame + 1, config::kLanPort);
    std::memcpy(frame + 3, settings.token, config::kTokenBytes);
    replies.respond(replies.context, request.link, frame, sizeof(frame));
    aqlog.println("LAN TOKEN issued=ble");
    return true;
  }
  case ble::kOpWifiScan:
    // Normal scans are dispatched on the LAN task. A queued scan means it
    // already had one pending, so answer busy on the originating transport.
    replies.error(replies.context, request.link, ble::kOpWifiScan,
                  ble::kErrBusy, "scan-pending");
    return true;
  default:
    return false;
  }
}
} // namespace aqsync
