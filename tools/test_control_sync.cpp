#include <control_sync.h>
#include <debug_log.h>
#include <device_config.h>
#include <sync_codec.h>
#include <wifi_link.h>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {
unsigned bonds_cleared = 0, sessions_dropped = 0, wifi_reapplied = 0,
         time_reapplied = 0;
struct Reply {
  ble::Link link;
  std::vector<std::uint8_t> bytes;
};
struct Failure {
  ble::Link link;
  ble::Op op;
  ble::Error code;
};
struct Peer {
  std::uint16_t payload = 600;
  unsigned attempts = 0;
  unsigned fail_attempt = 0;
  std::vector<Reply> successful;
  std::vector<Failure> errors;

  aqsync::ControlReplies callbacks() {
    return {this, payload_max, respond, error};
  }
  static std::uint16_t payload_max(void *context, ble::Link) {
    return static_cast<Peer *>(context)->payload;
  }
  static bool respond(void *context, ble::Link link, const std::uint8_t *bytes,
                      std::size_t size) {
    auto &peer = *static_cast<Peer *>(context);
    if (++peer.attempts == peer.fail_attempt)
      return false;
    peer.successful.push_back({link, {bytes, bytes + size}});
    return true;
  }
  static bool error(void *context, ble::Link link, ble::Op op, ble::Error code,
                    const char *) {
    static_cast<Peer *>(context)->errors.push_back({link, op, code});
    return true;
  }
};

ble::ControlRequest request(ble::Op op, ble::Link link = ble::Link::Ble,
                            const std::string &body = {}) {
  ble::ControlRequest request;
  assert(body.size() + 1 <= ble::kMaxControlBytes);
  request.bytes[0] = op;
  std::memcpy(request.bytes + 1, body.data(), body.size());
  request.length = static_cast<std::uint16_t>(body.size() + 1);
  request.link = link;
  return request;
}
void handle(const ble::ControlRequest &request, Peer &peer) {
  assert(aqsync::handle_common_control(request, peer.callbacks()));
}

void paged_configuration() {
  for (std::uint8_t page = 0; page <= 4; ++page) {
    Peer peer;
    handle(
        request(ble::kOpGetConfig, ble::Link::Lan, std::string(1, char(page))),
        peer);
    assert(peer.errors.empty() && peer.successful.size() == 1);
    const auto &frame = peer.successful[0].bytes;
    assert(frame[0] == ble::kFrameConfig && frame.size() < 482);
    const std::string json(frame.begin() + 2, frame.end());
    assert(json.find(page == 0   ? "\"ble\""
                     : page == 1 ? "\"location\""
                                 : "\"ntp\"") != std::string::npos);
  }
  for (const auto &body : {std::string(1, char(5)), std::string(2, char(0))}) {
    Peer peer;
    handle(request(ble::kOpGetConfig, ble::Link::Ble, body), peer);
    assert(peer.successful.empty() && peer.errors.size() == 1);
    assert(peer.errors[0].code == ble::kErrMalformed);
  }
  const unsigned previous_wifi = wifi_reapplied, previous_time = time_reapplied;
  Peer peer;
  handle(request(ble::kOpSetConfig, ble::Link::Lan, "ntp.interval_s=7200"),
         peer);
  assert(peer.errors.empty() && time_reapplied == previous_time + 1);
  assert(wifi_reapplied == previous_wifi);
  peer = Peer{};
  handle(request(ble::kOpSetConfig, ble::Link::Lan, "location.country=EG"),
         peer);
  assert(peer.errors.empty() && time_reapplied == previous_time + 1);
  assert(wifi_reapplied == previous_wifi);
}

void configuration() {
  config::set_sensor_hardware("Plantower", "PMS5003T");
  assert(config::load(false));
  Peer peer;
  handle(request(ble::kOpGetConfig, ble::Link::Lan), peer);
  assert(peer.successful.size() == 1 && peer.errors.empty());
  const auto &frame = peer.successful[0];
  assert(frame.link == ble::Link::Lan && frame.bytes[0] == ble::kFrameConfig);
  assert(frame.bytes[1] == 0);
  const std::string json(frame.bytes.begin() + 2, frame.bytes.end());
  assert(json.find("\"pin\":") == std::string::npos);
  assert(json.find("\"psk\":") == std::string::npos);
  assert(json.find("\"token\":") == std::string::npos);
  assert(json.find("\"bonds\":3") != std::string::npos);
  assert(json.find("\"clients\":3") != std::string::npos);
  const auto before = config::get();
  peer = Peer{};
  handle(request(ble::kOpSetConfig, ble::Link::Ble,
                 "ble.clear_bonds=1\nlan.rotate_token=1\nwifi.ssid=fixture"
                 "\nwifi.on=1\nble.pin=000001"),
         peer);
  assert(bonds_cleared == 1 && sessions_dropped == 1 && wifi_reapplied == 1);
  assert(peer.successful.size() == 1 && peer.errors.empty());
  assert(peer.successful[0].bytes[0] == ble::kFrameConfig);
  assert(peer.successful[0].bytes[1] == 1); // reboot required for pairing PIN
  assert(config::get().wifi_on && config::get().pin == 1);
  assert(std::memcmp(before.token, config::get().token, config::kTokenBytes) !=
         0);
  peer = Peer{};
  handle(request(ble::kOpSetConfig, ble::Link::Lan,
                 "ble.clear_bonds=1\nlan.rotate_token=1\nwifi.on=bad"),
         peer);
  assert(peer.successful.empty() && peer.errors.size() == 1);
  assert(peer.errors[0].link == ble::Link::Lan &&
         peer.errors[0].code == ble::kErrInvalidConfig);
  assert(bonds_cleared == 1 && sessions_dropped == 1 && wifi_reapplied == 1);
}

void config_chunks() {
  config::Actions actions;
  char bad_key[48];
  const std::string profile =
      "sensor.vendor=Plantower\nsensor.model=PMS5003T\n"
      "sensor.serial=PMS5003T-202604081332\nsensor.batch_candidate=1";
  assert(config::apply_lines(profile.data(), profile.size(), bad_key,
                             sizeof(bad_key), actions));
  Peer large;
  std::string expected;
  // Choose a valid UTF-8 SSID that crosses a 128-byte CONFIG_CHUNK boundary.
  for (std::size_t prefix = 0; prefix <= 30; ++prefix) {
    const std::string setting =
        "wifi.ssid=" + std::string(prefix, 'x') + "\xC3\xA9";
    assert(config::apply_lines(setting.data(), setting.size(), bad_key,
                               sizeof(bad_key), actions));
    large = Peer{};
    handle(request(ble::kOpGetConfig, ble::Link::Lan), large);
    assert(large.successful.size() == 1 &&
           large.successful[0].bytes[0] == ble::kFrameConfig);
    expected.assign(large.successful[0].bytes.begin() + 2,
                    large.successful[0].bytes.end());
    const auto at = expected.find("\xC3\xA9");
    if (at != std::string::npos && at % 128 == 127)
      break;
  }
  const auto unicode_at = expected.find("\xC3\xA9");
  assert(unicode_at != std::string::npos && unicode_at % 128 == 127);

  Peer small;
  small.payload = 244; // MTU 247: CONFIG cannot fit one ATT notification.
  handle(request(ble::kOpGetConfig), small);
  assert(small.errors.empty() && small.successful.size() >= 3);
  std::string actual;
  for (const auto &reply : small.successful) {
    const auto &bytes = reply.bytes;
    assert(reply.link == ble::Link::Ble && bytes[0] == ble::kFrameConfigChunk &&
           bytes.size() <= 244);
    assert(bytes[1] == large.successful[0].bytes[1] &&
           aq::sync::get_u16(bytes.data() + 2) == expected.size() &&
           aq::sync::get_u16(bytes.data() + 4) == actual.size());
    actual.append(bytes.begin() + 6, bytes.end());
  }
  assert(actual == expected);

  Peer failed;
  failed.payload = 244;
  failed.fail_attempt = 2;
  handle(request(ble::kOpGetConfig), failed);
  assert(failed.successful.size() == 1 && failed.attempts == 2);

  Peer full;
  full.payload = 514; // MTU 517 carries legacy single-frame CONFIG.
  handle(request(ble::kOpGetConfig), full);
  assert(full.successful.size() == 1 &&
         full.successful[0].bytes[0] == ble::kFrameConfig);
}

void token_guard() {
  Peer peer;
  handle(request(ble::kOpGetToken, ble::Link::Lan), peer);
  assert(peer.successful.empty() && peer.errors.size() == 1);
  assert(peer.errors[0].code == ble::kErrNotOnThisLink);
  assert(peer.errors[0].link == ble::Link::Lan);
  peer = Peer{};
  handle(request(ble::kOpGetToken, ble::Link::Ble), peer);
  assert(peer.errors.empty() && peer.successful.size() == 1);
  const auto &frame = peer.successful[0];
  assert(frame.link == ble::Link::Ble && frame.bytes[0] == ble::kFrameToken);
  assert(frame.bytes.size() == 3 + config::kTokenBytes);
  assert(aq::sync::get_u16(frame.bytes.data() + 1) == config::kLanPort);
  assert(std::memcmp(frame.bytes.data() + 3, config::get().token,
                     config::kTokenBytes) == 0);
}

void logs() {
  const std::string contents(DebugLog::kRingBytes, 'L');
  assert(aqlog.write(reinterpret_cast<const std::uint8_t *>(contents.data()),
                     contents.size()) == contents.size());
  std::string length(2, '\0');
  aq::sync::put_u16(reinterpret_cast<std::uint8_t *>(length.data()), 1500);
  for (const auto link : {ble::Link::Ble, ble::Link::Lan}) {
    Peer peer;
    peer.payload = 2000; // cap BLE at 512, LAN at 1024
    handle(request(ble::kOpLogTail, link, length), peer);
    const auto cap = link == ble::Link::Ble ? 512U : 1024U;
    assert(peer.errors.empty());
    std::size_t count = 0;
    for (std::size_t i = 0; i + 1 < peer.successful.size(); ++i) {
      const auto &frame = peer.successful[i];
      assert(frame.link == link && frame.bytes[0] == ble::kFrameLog);
      assert(frame.bytes.size() <= cap);
      for (std::size_t byte = 1; byte < frame.bytes.size(); ++byte)
        assert(frame.bytes[byte] == 'L');
      count += frame.bytes.size() - 1;
    }
    const auto &end = peer.successful.back().bytes;
    assert(end.size() == 7 && end[0] == ble::kFrameLogEnd);
    assert(aq::sync::get_u16(end.data() + 5) == count && count == 1500);
    assert(aq::sync::get_u32(end.data() + 1) >= DebugLog::kRingBytes);
  }
  Peer failed;
  failed.fail_attempt = 2;
  handle(request(ble::kOpLogTail, ble::Link::Ble, length), failed);
  assert(failed.successful.size() ==
         2); // first slice + LOG_END, no further data
  const auto &end = failed.successful.back().bytes;
  assert(end[0] == ble::kFrameLogEnd);
  assert(aq::sync::get_u16(end.data() + 5) == 511);
  Peer malformed;
  handle(request(ble::kOpLogTail, ble::Link::Lan, "x"), malformed);
  assert(malformed.successful.empty() && malformed.errors.size() == 1);
  assert(malformed.errors[0].code == ble::kErrMalformed);
}

void dispatch() {
  Peer peer;
  const auto callbacks = peer.callbacks();
  assert(!aqsync::handle_common_control(request(ble::kOpSetTime), callbacks));
  assert(!aqsync::handle_common_control(request(ble::kOpOpen), callbacks));
  ble::ControlRequest empty;
  assert(!aqsync::handle_common_control(empty, callbacks));
  assert(!aqsync::handle_common_control(request(ble::kOpGetConfig), {}));
  handle(request(ble::kOpWifiScan, ble::Link::Lan), peer);
  assert(peer.errors.size() == 1 && peer.errors[0].code == ble::kErrBusy);
  assert(peer.errors[0].link == ble::Link::Lan);
}
} // namespace

// These are transport observations/actions, not radio or protocol mirrors.
namespace ble {
LinkState link() {
  LinkState state;
  state.bonds = 3;
  return state;
}
void clear_bonds() { ++bonds_cleared; }
} // namespace ble
namespace lan {
Status status() {
  Status value;
  value.state = "connected";
  std::strcpy(value.ip, "192.0.2.1");
  std::strcpy(value.host, "aq-fixture");
  value.authenticated = true;
  value.clients = 3;
  return value;
}
void drop_session() { ++sessions_dropped; }
void apply_time_settings() { ++time_reapplied; }
void apply_settings() { ++wifi_reapplied; }
} // namespace lan

int main() {
  configuration();
  paged_configuration();
  config_chunks();
  token_guard();
  logs();
  dispatch();
}

namespace aq::network_time {
Status status() { return Status{}; }
} // namespace aq::network_time
