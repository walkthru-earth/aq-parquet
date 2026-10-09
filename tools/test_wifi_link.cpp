// Exercise the real Wi-Fi transport over host TCP sockets. Wi-Fi/mDNS and
// task creation are faked; framing, authentication, routing and sockets are
// not.
#include "../firmware/common/connectivity/src/wifi_link.cpp"

#include <cassert>
#include <chrono>
#include <csignal>
#include <thread>
#include <vector>

namespace aq::console {
std::size_t write(const std::uint8_t *, std::size_t length) { return length; }
} // namespace aq::console

namespace {
config::Settings fixture_settings;
std::vector<ble::ControlRequest> requests;
bool accept_requests = true;
std::uint32_t ble_reply_generation = 0;
std::vector<std::vector<std::uint8_t>> ble_frames;
bool enqueue(const ble::ControlRequest &request) {
  if (!accept_requests)
    return false;
  requests.push_back(request);
  return true;
}

int connect_phone() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  assert(fd >= 0);
  timeval timeout{1, 0};
  assert(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
         0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(config::kLanPort);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(::connect(fd, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) == 0);
  lan::accept_client();
  return fd;
}

void write_bytes(int fd, const std::uint8_t *data, std::size_t length) {
  assert(::send(fd, data, length, 0) == static_cast<int>(length));
}

void read_exact(int fd, std::uint8_t *data, std::size_t length) {
  while (length) {
    const auto count = ::recv(fd, data, length, 0);
    assert(count > 0);
    data += count;
    length -= count;
  }
}

std::vector<std::uint8_t> read_frame(int fd) {
  std::uint8_t prefix[2];
  read_exact(fd, prefix, sizeof(prefix));
  const auto length = unsigned(prefix[0]) | (unsigned(prefix[1]) << 8);
  assert(length > 0 && length <= lan::kPayloadMax);
  std::vector<std::uint8_t> frame(length);
  read_exact(fd, frame.data(), frame.size());
  return frame;
}

void authenticate(int fd, std::uint8_t peer, bool fragmented = false) {
  std::uint8_t bytes[lan::kHandshakeBytes];
  std::memcpy(bytes, "AQS1", 4);
  std::memcpy(bytes + 4, fixture_settings.token, config::kTokenBytes);
  if (fragmented) {
    write_bytes(fd, bytes, 7);
    lan::handle_client_bytes(peer);
    assert(lan::payload_max(peer) == 0);
    write_bytes(fd, bytes + 7, sizeof(bytes) - 7);
  } else {
    write_bytes(fd, bytes, sizeof(bytes));
  }
  lan::handle_client_bytes(peer);
  const auto hello = read_frame(fd);
  assert(hello[0] == ble::kFrameHello && hello[1] == ble::kProtocolVersion);
  assert(lan::payload_max(peer) == lan::kPayloadMax);
}

void request_status(int fd, std::uint8_t peer) {
  const std::uint8_t bytes[] = {1, 0, ble::kOpStatus};
  write_bytes(fd, bytes, sizeof(bytes));
  lan::handle_client_bytes(peer);
}
} // namespace

namespace config {
Settings get() { return fixture_settings; }
} // namespace config
namespace ble {
const char *local_name() { return "AQ-fixture"; }
const char *info_json() { return "{\"station\":\"fixture\"}"; }
bool send_response(const std::uint8_t *frame, std::size_t length,
                   std::uint32_t generation) {
  ble_frames.emplace_back(frame, frame + length);
  ble_reply_generation = generation;
  return true;
}
} // namespace ble

int main() {
  lan::TimeAnchor time_anchor;
  assert(!lan::take_time_anchor(time_anchor));
  timeval actual_time{1800000000, 123456};
  lan::network_time_synced(&actual_time);
  assert(lan::take_time_anchor(time_anchor));
  assert(time_anchor.seconds == 1800000000 &&
         time_anchor.subsecond_us == 123456);
  assert(!lan::take_time_anchor(time_anchor));
  std::signal(SIGPIPE, SIG_IGN);
  const ble::Identity identity{"fixture", "device", "boot", "schema",
                               0,         "",       "test"};
  fake_init_result = ESP_FAIL;
  assert(!lan::begin("aq-fixture", identity, enqueue));
  assert(!lan::station_netif && !lan::wifi_initialized);
  assert(!lan::request_scan(ble::Link::Ble, 1));
  fake_init_result = ESP_OK;
  fake_task_result = 0;
  assert(!lan::begin("aq-fixture", identity, enqueue));
  assert(!lan::station_netif && !lan::wifi_initialized && !lan::socket_mutex &&
         !fake_station_events && !fake_ip_events && fake_wifi_deinits == 1);
  assert(!lan::request_scan(ble::Link::Ble, 1));
  fake_task_result = pdPASS;
  assert(lan::begin("aq-fixture", identity, enqueue));
  assert(!lan::begin("aq-fixture", identity, enqueue));
  assert(fake_wifi_storage == WIFI_STORAGE_RAM);
  assert(!fake_wifi_started);
  lan::settings = fixture_settings;
  lan::connected = true;
  lan::start_server();
  if (lan::listen_fd < 0)
    std::fprintf(stderr, "LAN fixture listener failed: errno=%d (%s)\n", errno,
                 std::strerror(errno));
  assert(lan::listen_fd >= 0);
  assert(fake_mdns_started && fake_mdns_host == "aq-fixture" &&
         fake_mdns_instance == "AQ-fixture" && fake_mdns_service == "_aqsync" &&
         fake_mdns_transport == "_tcp" && fake_mdns_port == config::kLanPort);
  assert(fake_mdns_txt ==
         (std::map<std::string, std::string>{{"proto", "2"},
                                             {"station", "fixture"},
                                             {"dev", "device"},
                                             {"fw", "test"}}));
  int phones[ble::kMaxLanClients];
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer) {
    phones[peer] = connect_phone();
    authenticate(phones[peer], peer, peer == 1);
  }
  assert(lan::status().clients == 3 && lan::status().authenticated);
  const int fourth = connect_phone();
  const auto busy = read_frame(fourth);
  assert(busy[0] == ble::kFrameError && busy[2] == ble::kErrBusy);
  std::uint8_t byte;
  assert(::recv(fourth, &byte, 1, 0) == 0);
  ::close(fourth);
  assert(lan::status().clients == 3);

  // Each parsed request and response stays on the phone that sent it.
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer) {
    request_status(phones[peer], peer);
    assert(requests.back().peer == peer);
    const std::uint8_t frame[] = {ble::kFrameStatus, peer};
    assert(lan::send_response(frame, sizeof(frame), peer,
                              requests.back().link_generation));
    assert(read_frame(phones[peer]) ==
           std::vector<std::uint8_t>(frame, frame + 2));
  }
  // The task waits on socket readiness, rather than sleeping after each
  // request. Use a longer idle timeout here to tolerate host scheduling noise;
  // arriving bytes must interrupt it, including on a nonzero peer slot.
  for (const std::uint8_t peer : {std::uint8_t(0), std::uint8_t(2)}) {
    const auto before = requests.size();
    std::thread phone([&, peer] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      const std::uint8_t bytes[] = {1, 0, ble::kOpStatus};
      write_bytes(phones[peer], bytes, sizeof(bytes));
    });
    const auto started = std::chrono::steady_clock::now();
    lan::poll_server(500000);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    phone.join();
    assert(requests.size() == before + 1 && requests.back().peer == peer);
    assert(elapsed < std::chrono::milliseconds(400));
  }
  // Socket waiting is bounded when no request arrives (radio housekeeping
  // and idle expiry must still run); no spin loop replaces the fixed sleep.
  const auto idle_started = std::chrono::steady_clock::now();
  lan::poll_server(20000);
  assert(std::chrono::steady_clock::now() - idle_started >=
         std::chrono::milliseconds(15));

  // TCP can split a full-size body at any byte or coalesce several frames.
  std::vector<std::uint8_t> full(2 + ble::kMaxControlBytes, 0x55);
  full[0] = 0;
  full[1] = 2;
  full[2] = ble::kOpList;
  const auto before_fragment = requests.size();
  write_bytes(phones[0], full.data(), 9);
  lan::handle_client_bytes(0);
  assert(requests.size() == before_fragment);
  write_bytes(phones[0], full.data() + 9, full.size() - 9);
  lan::handle_client_bytes(0);
  assert(requests.back().length == ble::kMaxControlBytes &&
         requests.back().bytes[511] == 0x55);
  const std::uint8_t coalesced[] = {1, 0, ble::kOpStatus, 1, 0, ble::kOpClose};
  const auto before_coalesced = requests.size();
  write_bytes(phones[2], coalesced, sizeof(coalesced));
  lan::handle_client_bytes(2);
  assert(requests.size() == before_coalesced + 2 && requests.back().peer == 2);
  accept_requests = false;
  request_status(phones[2], 2);
  const auto queue_busy = read_frame(phones[2]);
  assert(queue_busy[0] == ble::kFrameError && queue_busy[1] == ble::kOpStatus &&
         queue_busy[2] == ble::kErrBusy);
  accept_requests = true;

  lan::publish_status("{}", 2);
  lan::publish_live("{\"x\":1}", 7);
  lan::push_documents();
  for (int fd : phones) {
    assert(read_frame(fd)[0] == ble::kFrameStatus);
    assert(read_frame(fd)[0] == ble::kFrameLive);
  }

  const auto old_generation = lan::connection_generation(1);
  const auto other_generation = lan::connection_generation(2);
  ::close(phones[1]);
  lan::handle_client_bytes(1);
  assert(lan::status().clients == 2);
  assert(lan::connection_generation(2) == other_generation);
  phones[1] = connect_phone();
  authenticate(phones[1], 1);
  const std::uint8_t response[] = {ble::kFrameClosed, 42};
  assert(!lan::send_response(response, sizeof(response), 1, old_generation));
  assert(!lan::send_error(ble::kOpClose, ble::kErrBusy, "stale", 1,
                          old_generation));
  assert(lan::send_response(response, sizeof(response), 1,
                            lan::connection_generation(1)));
  assert(read_frame(phones[1]) ==
         std::vector<std::uint8_t>(response, response + 2));

  // Malformed/auth-failing clients cannot terminate their neighbors.
  const std::uint8_t malformed[] = {1, 2}; // 513 exceeds bounded control size
  write_bytes(phones[0], malformed, sizeof(malformed));
  lan::handle_client_bytes(0);
  assert(read_frame(phones[0])[2] == ble::kErrMalformed);
  ::close(phones[0]);
  phones[0] = connect_phone();
  std::uint8_t invalid[lan::kHandshakeBytes]{};
  write_bytes(phones[0], invalid, sizeof(invalid));
  lan::handle_client_bytes(0);
  assert(read_frame(phones[0])[2] == ble::kErrAuth);
  ::close(phones[0]);
  assert(lan::status().clients == 2);
  phones[0] = connect_phone();
  fake_now_us += lan::kHandshakeUs + 1;
  // Keep authenticated neighbors alive while the unauthenticated slot expires.
  request_status(phones[1], 1);
  request_status(phones[2], 2);
  lan::poll_server();
  assert(lan::clients[0].fd.load() < 0 && lan::status().clients == 2);
  ::close(phones[0]);
  // Reconnecting slot 1 received its cached initial snapshots on poll.
  assert(read_frame(phones[1])[0] == ble::kFrameStatus);
  assert(read_frame(phones[1])[0] == ble::kFrameLive);

  assert(lan::respond(ble::Link::Ble, 99, 0, response, sizeof(response)));
  assert(ble_reply_generation == 99);

  // A send failure marks only its own slot, then poll closes it.
  linger reset{1, 0};
  assert(::setsockopt(phones[1], SOL_SOCKET, SO_LINGER, &reset,
                      sizeof(reset)) == 0);
  ::close(phones[1]);
  // Wait until the local socket receives the reset before exercising send.
  fd_set reset_readable;
  FD_ZERO(&reset_readable);
  const auto server_fd = lan::clients[1].fd.load();
  FD_SET(server_fd, &reset_readable);
  timeval reset_wait{1, 0};
  assert(::select(server_fd + 1, &reset_readable, nullptr, nullptr,
                  &reset_wait) > 0);
  bool failed = false;
  for (unsigned attempt = 0; attempt < 4 && !failed; ++attempt)
    failed = !lan::send_response(response, sizeof(response), 1);
  assert(failed && lan::clients[1].drop.load());
  lan::poll_server();
  assert(lan::status().clients == 1 && lan::payload_max(2) == lan::kPayloadMax);
  lan::drop_session();
  assert(!lan::send_response(response, sizeof(response), 2));
  assert(lan::payload_max(2) == 0);
  lan::poll_server();
  assert(lan::status().clients == 0 && !lan::status().client);
  ::close(phones[2]);
  const int idle = connect_phone();
  authenticate(idle, 0);
  const auto idle_generation = lan::connection_generation(0);
  fake_now_us += lan::kIdleUs + 1;
  lan::poll_server();
  assert(!lan::status().client &&
         lan::connection_generation(0) != idle_generation);
  assert(::recv(idle, &byte, 1, 0) == 0);
  ::close(idle);

  fixture_settings.wifi_on = true;
  std::memcpy(fixture_settings.ssid, "fixture", 8);
  fixture_settings.lan_on = false;
  lan::apply_settings();
  lan::poll_radio();
  assert(lan::listen_fd < 0);
  lan::poll_radio();
  assert(lan::connected && lan::listen_fd < 0);
  assert(fake_ntp_starts > 0 && fake_time_callback != nullptr);
  // Association without a successful SNTP callback is not time evidence.
  assert(!lan::take_time_anchor(time_anchor));
  fake_now_us += 123456;
  fake_time_callback(&actual_time);
  assert(lan::take_time_anchor(time_anchor));
  assert(time_anchor.monotonic_us == fake_now_us &&
         time_anchor.seconds == actual_time.tv_sec &&
         time_anchor.subsecond_us == 123456);
  assert(!lan::take_time_anchor(time_anchor));
  const auto starts_before_reconnect = fake_ntp_starts;
  const auto stops_before_disconnect = fake_ntp_stops;
  fake_wifi_autoconnect = false;
  fake_wifi_lost();
  lan::poll_radio();
  assert(!lan::connected && fake_ntp_stops == stops_before_disconnect + 1);
  fake_wifi_autoconnect = true;
  fake_wifi_got_ip();
  lan::poll_radio();
  assert(lan::connected && fake_ntp_starts == starts_before_reconnect + 1);
  // Reassociation restarts SNTP but never manufactures a new clock anchor.
  assert(!lan::take_time_anchor(time_anchor));
  fixture_settings.lan_on = true;
  lan::apply_settings();
  lan::poll_radio();
  lan::poll_radio();
  assert(lan::listen_fd >= 0);
  const int restored = connect_phone();
  authenticate(restored, 0);
  fixture_settings.lan_on = false;
  lan::apply_settings();
  lan::poll_radio();
  assert(lan::listen_fd < 0 && !lan::status().client);
  assert(::recv(restored, &byte, 1, 0) == 0);
  ::close(restored);
  lan::stop_server("test-end");

  // Native station events drive link state; failed association keeps the
  // existing bounded 30 second connect / 30 second retry cadence.
  fake_wifi_autoconnect = false;
  lan::apply_settings();
  lan::poll_radio();
  assert(std::strcmp(lan::status().state, "connecting") == 0 &&
         fake_wifi_power_save == WIFI_PS_MIN_MODEM);
  const auto attempts = fake_wifi_connects;
  fake_now_us += lan::kConnectTimeoutUs + 1;
  lan::poll_radio();
  assert(std::strcmp(lan::status().state, "failed") == 0);
  fake_now_us += lan::kRetryUs - 1;
  lan::poll_radio();
  assert(fake_wifi_connects == attempts);
  ++fake_now_us;
  lan::poll_radio();
  assert(fake_wifi_connects == attempts + 1);
  assert(!lan::take_time_anchor(time_anchor));

  // A start error must retry as well, even though the radio never started.
  fake_start_result = ESP_FAIL;
  lan::apply_settings();
  lan::poll_radio();
  assert(!lan::radio_on && std::strcmp(lan::status().state, "failed") == 0);
  fake_start_result = ESP_OK;
  fake_now_us += lan::kRetryUs;
  lan::poll_radio();
  assert(lan::radio_on && std::strcmp(lan::status().state, "connecting") == 0);

  fixture_settings.wifi_on = false;
  lan::apply_settings();
  lan::poll_radio();
  assert(!fake_wifi_started && !fake_mdns_started &&
         std::strcmp(lan::status().state, "off") == 0);

  // Off-radio scans start/stop temporarily, deduplicate SSIDs by strongest
  // RSSI, encode security/channel, preserve 32-byte names and skip hidden APs.
  auto ap = [](const char *ssid, std::int8_t rssi, wifi_auth_mode_t auth,
               std::uint8_t channel) {
    wifi_ap_record_t record{};
    std::snprintf(reinterpret_cast<char *>(record.ssid), sizeof(record.ssid),
                  "%s", ssid);
    record.rssi = rssi;
    record.authmode = auth;
    record.primary = channel;
    return record;
  };
  fake_scan_records = {
      ap("", -20, WIFI_AUTH_OPEN, 1),
      ap("fixture-ap", -85, WIFI_AUTH_WPA2_PSK, 1),
      ap("fixture-ap", -35, WIFI_AUTH_WPA3_PSK, 6),
      ap("12345678901234567890123456789012", -128, WIFI_AUTH_OPEN, 11)};
  ble_frames.clear();
  assert(lan::request_scan(ble::Link::Ble, 123));
  assert(!lan::request_scan(ble::Link::Ble, 456));
  lan::run_scan();
  lan::scan_pending = false;
  assert(ble_reply_generation == 123 && ble_frames.size() == 3);
  assert(ble_frames[0][0] == ble::kFrameWifiAp &&
         static_cast<std::int8_t>(ble_frames[0][1]) == -35 &&
         ble_frames[0][2] == 6 && ble_frames[0][3] == 6);
  assert(ble_frames[1].size() == 36 &&
         static_cast<std::int8_t>(ble_frames[1][1]) == -127);
  assert(ble_frames[2] ==
         (std::vector<std::uint8_t>{ble::kFrameWifiScanEnd, 2, 0, 0}));
  assert(fake_scan_records.empty() && !fake_wifi_started && !lan::radio_on);

  // Bounded scans release the complete driver list; failure emits one end
  // frame and also frees results so repeated scans cannot retain AP memory.
  for (unsigned i = 0; i < 80; ++i) {
    const std::string name = "ap" + std::to_string(i);
    fake_scan_records.push_back(ap(name.c_str(), -60, WIFI_AUTH_WPA2_PSK, 1));
  }
  ble_frames.clear();
  lan::run_scan();
  assert(ble_frames.size() == lan::kMaxScanEntries + 1 &&
         ble_frames.back()[1] == lan::kMaxScanEntries &&
         fake_scan_records.empty());
  fake_scan_result = ESP_FAIL;
  fake_scan_records.push_back(ap("unavailable", -50, WIFI_AUTH_OPEN, 1));
  ble_frames.clear();
  lan::run_scan();
  assert(ble_frames ==
         (std::vector<std::vector<std::uint8_t>>{
             {ble::kFrameWifiScanEnd, 0, 0, ble::kErrWifiUnavailable}}));
  assert(fake_scan_records.empty() && fake_scan_clears == 1 &&
         !fake_wifi_started);
  fake_scan_result = ESP_OK;

  // The driver sees credentials in RAM only; diagnostics never include PSK.
  // This is a generated fixture value, never a real owner's network secret.
  std::memset(fixture_settings.ssid, 's', config::kSsidMax);
  fixture_settings.ssid[config::kSsidMax] = '\0';
  std::memset(fixture_settings.psk, 'z', config::kPskMax);
  fixture_settings.psk[config::kPskMax] = '\0';
  fixture_settings.wifi_on = true;
  lan::apply_settings();
  lan::poll_radio();
  assert(std::memcmp(fake_wifi_config.sta.ssid, fixture_settings.ssid,
                     config::kSsidMax) == 0);
  assert(fake_wifi_config.sta.threshold.authmode == WIFI_AUTH_WPA2_PSK);
  assert(std::memcmp(fake_wifi_config.sta.password, fixture_settings.psk,
                     config::kPskMax) == 0 &&
         fake_wifi_config.sta.password[config::kPskMax] == 0);
  char log[DebugLog::kRingBytes];
  std::uint32_t logged;
  const auto length = aqlog.tail(log, sizeof(log), logged);
  assert(std::string(log, length).find(fixture_settings.psk) ==
         std::string::npos);
  fixture_settings.wifi_on = false;
  lan::apply_settings();
  lan::poll_radio();
  vSemaphoreDelete(lan::socket_mutex);
  lan::socket_mutex = nullptr;
  lan::release_station();
}
