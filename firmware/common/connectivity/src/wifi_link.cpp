#include "wifi_link.h"
#include "debug_log.h"
#include "device_config.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace lan {
namespace {
constexpr TickType_t kTick = pdMS_TO_TICKS(100);
constexpr std::int64_t kConnectTimeoutUs = 30LL * 1000000LL;
constexpr std::int64_t kRetryUs = 30LL * 1000000LL;
constexpr std::int64_t kHandshakeUs = 5LL * 1000000LL;
constexpr std::int64_t kIdleUs = 300LL * 1000000LL;
constexpr std::int64_t kSendUs = 1000000LL;
constexpr std::size_t kHandshakeBytes = 4 + config::kTokenBytes;
constexpr std::size_t kMaxScanEntries = 48;

char host_label[24]{};
ble::Identity device_identity{};
ble::RequestHandler request_handler = nullptr;
Status current;
portMUX_TYPE status_mutex = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t socket_mutex = nullptr;

std::atomic<bool> reapply{true}, drop_requested{false};
std::atomic<bool> scan_pending{false};
std::atomic<std::uint8_t> scan_link{0};
std::atomic<std::uint32_t> scan_generation{0};
std::atomic<std::uint8_t> scan_peer{0};
std::atomic<std::uint32_t> ui{0}, sessions{0}, bytes_out{0};

// Only the LAN task changes parser/timing state. Socket identity,
// authentication and generation are changed under socket_mutex; worker sends
// validate there.
struct Client {
  std::atomic<int> fd{-1};
  std::atomic<bool> authenticated{false}, drop{false};
  std::atomic<std::uint32_t> generation{0};
  std::uint8_t rx[2 + ble::kMaxControlBytes + kHandshakeBytes]{};
  std::size_t rx_length = 0;
  std::int64_t since_us = 0, last_rx_us = 0;
  bool needs_snapshots = false;
};
Client clients[ble::kMaxLanClients];

std::uint8_t authenticated_clients() {
  return static_cast<std::uint8_t>(std::count_if(
      std::begin(clients), std::end(clients),
      [](const Client &client) { return client.authenticated.load(); }));
}

bool any_client() {
  return std::any_of(
      std::begin(clients), std::end(clients),
      [](const Client &client) { return client.fd.load() >= 0; });
}

// Latest pushes, copied by the sampling loop and sent by the LAN task.
char status_json[ble::kMaxJson + 16]{};
char live_json[ble::kMaxJson + 16]{};
std::size_t status_length = 0, live_length = 0;
std::atomic<bool> status_dirty{false}, live_dirty{false};
portMUX_TYPE push_mutex = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE scan_mutex = portMUX_INITIALIZER_UNLOCKED;

void touch_ui() { ++ui; }

void set_status(const char *state, bool link_up) {
  // Strings are built before the critical section: no heap use with
  // interrupts masked.
  char ip[16] = "";
  char mac[18] = "";
  int rssi = 0;
  if (link_up) {
    std::snprintf(ip, sizeof(ip), "%s", WiFi.localIP().toString().c_str());
    rssi = WiFi.RSSI();
  }
  std::snprintf(mac, sizeof(mac), "%s", WiFi.macAddress().c_str());
  portENTER_CRITICAL(&status_mutex);
  current.state = state;
  std::memcpy(current.ip, ip, sizeof(ip));
  std::memcpy(current.mac, mac, sizeof(mac));
  current.rssi = rssi;
  std::snprintf(current.host, sizeof(current.host), "%s", host_label);
  current.client = any_client();
  current.clients = authenticated_clients();
  current.authenticated = current.clients > 0;
  current.sessions = sessions.load();
  current.bytes_out = bytes_out.load();
  portEXIT_CRITICAL(&status_mutex);
  touch_ui();
}

// ---- socket helpers. An fd never escapes the socket_mutex for a send/close.

bool write_all(int fd, const std::uint8_t *data, std::size_t length) {
  std::size_t sent = 0;
  const auto deadline_us = esp_timer_get_time() + kSendUs;
  while (sent < length) {
    // SO_SNDTIMEO bounds one send; this also bounds partial-progress loops.
    int count;
    if (esp_timer_get_time() >= deadline_us) {
      errno = ETIMEDOUT;
      count = -1;
    } else {
      count = ::send(fd, data + sent, length - sent, 0);
    }
    if (count <= 0) {
      aqlog.record_only().printf("LAN SEND FAILED errno=%d sent=%u of=%u\n",
                                 errno, unsigned(sent), unsigned(length));
      return false;
    }
    sent += static_cast<std::size_t>(count);
  }
  bytes_out += static_cast<std::uint32_t>(length);
  return true;
}

// Called with socket_mutex held, including for the unassigned busy socket.
bool send_framed_locked(int fd, const std::uint8_t *frame, std::size_t length) {
  if (fd < 0 || length == 0 || length > kPayloadMax)
    return false;
  std::uint8_t buffer[2 + kPayloadMax];
  buffer[0] = length & 0xff;
  buffer[1] = (length >> 8) & 0xff;
  std::memcpy(buffer + 2, frame, length);
  return write_all(fd, buffer, 2 + length);
}

bool send_to(std::uint8_t peer, const std::uint8_t *frame, std::size_t length,
             std::uint32_t expected_generation = 0,
             bool require_authenticated = true) {
  if (peer >= ble::kMaxLanClients || !socket_mutex)
    return false;
  // Every holder is bounded by the cumulative write deadline above. Wait for
  // frame ownership rather than silently losing a reply on lock contention.
  xSemaphoreTake(socket_mutex, portMAX_DELAY);
  auto &client = clients[peer];
  const int fd = client.fd.load();
  const bool valid =
      fd >= 0 && !drop_requested.load() && !client.drop.load() &&
      (!require_authenticated || client.authenticated.load()) &&
      (!expected_generation || client.generation.load() == expected_generation);
  const bool ok = valid && send_framed_locked(fd, frame, length);
  if (valid && !ok)
    client.drop = true;
  xSemaphoreGive(socket_mutex);
  return ok;
}

std::size_t error_frame(std::uint8_t *frame, ble::Op op, ble::Error code,
                        const char *detail) {
  frame[0] = ble::kFrameError;
  frame[1] = static_cast<std::uint8_t>(op);
  frame[2] = static_cast<std::uint8_t>(code);
  std::size_t length = 3;
  if (detail) {
    const std::size_t copy =
        std::strlen(detail) > 64 ? 64 : std::strlen(detail);
    std::memcpy(frame + 3, detail, copy);
    length += copy;
  }
  aqlog.record_only().printf("LAN ERROR op=0x%02x code=%u\n", unsigned(op),
                             unsigned(code));
  return length;
}

bool send_error_to(std::uint8_t peer, ble::Op op, ble::Error code,
                   const char *detail) {
  std::uint8_t frame[3 + 64];
  const auto length = error_frame(frame, op, code, detail);
  return send_to(peer, frame, length, 0, false);
}

void close_client(std::uint8_t peer, const char *reason) {
  auto &client = clients[peer];
  // Wait for any whole-frame send to finish before closing; no unlocked
  // fallback can close an fd that another task is still using.
  xSemaphoreTake(socket_mutex, portMAX_DELAY);
  const int fd = client.fd.exchange(-1);
  const bool was_authenticated = client.authenticated.exchange(false);
  if (fd >= 0) {
    ::close(fd);
    ++client.generation;
  }
  client.drop = false;
  xSemaphoreGive(socket_mutex);
  client.rx_length = 0;
  client.needs_snapshots = false;
  if (fd >= 0) {
    aqlog.record_only().printf(
        "LAN DISCONNECT slot=%u reason=%s authenticated=%s\n", unsigned(peer),
        reason, was_authenticated ? "true" : "false");
    touch_ui();
  }
}

// ---- server task state

int listen_fd = -1;
bool radio_on = false, connected = false, mdns_on = false;
std::int64_t connect_started_us = 0, retry_at_us = 0;
config::Settings settings;

void start_server() {
  if (listen_fd >= 0 || !settings.lan_on)
    return;
  listen_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_fd < 0) {
    aqlog.record_only().printf("LAN ERROR operation=socket errno=%d\n", errno);
    return;
  }
  int reuse = 1;
  ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(config::kLanPort);
  if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) != 0 ||
      ::listen(listen_fd, ble::kMaxLanClients) != 0) {
    aqlog.record_only().printf("LAN ERROR operation=bind-listen errno=%d\n",
                               errno);
    ::close(listen_fd);
    listen_fd = -1;
    return;
  }
  if (MDNS.begin(host_label)) {
    MDNS.addService("aqsync", "tcp", config::kLanPort);
    MDNS.addServiceTxt("aqsync", "tcp", "proto", String(kProtocolVersion));
    MDNS.addServiceTxt("aqsync", "tcp", "station", device_identity.station);
    MDNS.addServiceTxt("aqsync", "tcp", "dev", device_identity.device);
    MDNS.addServiceTxt("aqsync", "tcp", "fw", device_identity.firmware);
    mdns_on = true;
  } else {
    aqlog.record_only().println("LAN ERROR operation=mdns");
  }
  portENTER_CRITICAL(&status_mutex);
  current.mdns = mdns_on;
  portEXIT_CRITICAL(&status_mutex);
  aqlog.record_only().printf(
      "LAN LISTEN port=%u host=%s.local mdns=%s service=_aqsync._tcp\n",
      unsigned(config::kLanPort), host_label, mdns_on ? "true" : "false");
}

void stop_server(const char *reason) {
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer)
    close_client(peer, reason);
  if (mdns_on) {
    MDNS.end();
    mdns_on = false;
  }
  if (listen_fd >= 0) {
    ::close(listen_fd);
    listen_fd = -1;
    aqlog.record_only().printf("LAN STOP reason=%s\n", reason);
  }
  portENTER_CRITICAL(&status_mutex);
  current.mdns = false;
  portEXIT_CRITICAL(&status_mutex);
}

void radio_off(const char *reason) {
  stop_server(reason);
  if (radio_on) {
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    radio_on = false;
    aqlog.record_only().printf("WIFI OFF reason=%s\n", reason);
  }
  connected = false;
  set_status("off", false);
}

void radio_connect() {
  if (!radio_on) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true); // modem sleep keeps BLE coexistence workable
    WiFi.setAutoReconnect(false);
    radio_on = true;
  }
  WiFi.begin(settings.ssid, settings.psk[0] ? settings.psk : nullptr);
  connect_started_us = esp_timer_get_time();
  retry_at_us = 0;
  set_status("connecting", false);
  aqlog.record_only().printf("WIFI CONNECT ssid=%s\n", settings.ssid);
}

void poll_radio() {
  const std::int64_t now = esp_timer_get_time();
  if (reapply.exchange(false)) {
    settings = config::get();
    // Reconcile lan.on and invalidate old sessions even when the station
    // stays associated through WiFi.begin() with the same network.
    stop_server("settings");
    connected = false;
    if (!settings.wifi_on || !settings.ssid[0]) {
      radio_off("settings");
      return;
    }
    radio_connect();
    return;
  }
  if (!radio_on)
    return;
  const bool up = WiFi.status() == WL_CONNECTED;
  if (up && !connected) {
    connected = true;
    set_status("connected", true);
    aqlog.record_only().printf("WIFI CONNECTED ssid=%s ip=%s rssi=%d\n",
                               settings.ssid, WiFi.localIP().toString().c_str(),
                               WiFi.RSSI());
    start_server();
  } else if (!up && connected) {
    connected = false;
    stop_server("wifi-lost");
    aqlog.record_only().println("WIFI LOST");
    radio_connect();
  } else if (!up && retry_at_us == 0 &&
             now - connect_started_us > kConnectTimeoutUs) {
    set_status("failed", false);
    retry_at_us = now + kRetryUs;
    aqlog.record_only().printf("WIFI FAILED ssid=%s status=%d retry_s=%lld\n",
                               settings.ssid, int(WiFi.status()),
                               static_cast<long long>(kRetryUs / 1000000));
    WiFi.disconnect();
  } else if (!up && retry_at_us && now >= retry_at_us) {
    radio_connect();
  } else if (up) {
    // Periodic RSSI refresh for the UI/CONFIG document; no UI event, so the
    // display loop's own 10 s cadence (and its serial print) is untouched.
    static std::int64_t last_refresh = 0;
    if (now - last_refresh > 10000000LL) {
      last_refresh = now;
      const int rssi = WiFi.RSSI();
      portENTER_CRITICAL(&status_mutex);
      current.rssi = rssi;
      portEXIT_CRITICAL(&status_mutex);
    }
  }
}

std::uint8_t auth_code(wifi_auth_mode_t mode) {
  switch (mode) {
  case WIFI_AUTH_OPEN:
    return 0;
  case WIFI_AUTH_WEP:
    return 1;
  case WIFI_AUTH_WPA_PSK:
    return 2;
  case WIFI_AUTH_WPA2_PSK:
    return 3;
  case WIFI_AUTH_WPA_WPA2_PSK:
    return 4;
  case WIFI_AUTH_WPA2_ENTERPRISE:
    return 5;
  case WIFI_AUTH_WPA3_PSK:
    return 6;
  case WIFI_AUTH_WPA2_WPA3_PSK:
    return 7;
  default:
    return 255;
  }
}

bool respond(ble::Link link, std::uint32_t link_generation, std::uint8_t peer,
             const std::uint8_t *frame, std::size_t length) {
  if (link == ble::Link::Lan)
    return send_response(frame, length, peer, link_generation);
  return ble::send_response(frame, length, link_generation);
}

void run_scan() {
  const auto link = static_cast<ble::Link>(scan_link.load());
  const auto link_generation = scan_generation.load();
  const auto peer = scan_peer.load();
  const bool temporary = !radio_on;
  if (temporary) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
  }
  aqlog.record_only().println("WIFI SCAN BEGIN");
  const int found = WiFi.scanNetworks(false, false);
  if (found < 0) {
    aqlog.record_only().printf("WIFI SCAN FAILED code=%d\n", found);
    std::uint8_t frame[4];
    frame[0] = ble::kFrameWifiScanEnd;
    frame[1] = 0;
    frame[2] = 0;
    frame[3] = ble::kErrWifiUnavailable;
    respond(link, link_generation, peer, frame, sizeof(frame));
    if (temporary)
      WiFi.mode(WIFI_OFF);
    return;
  }
  // Report each SSID once with its strongest access point; skip hidden.
  bool reported[kMaxScanEntries]{};
  std::uint16_t count = 0;
  for (int i = 0; i < found && i < int(kMaxScanEntries); ++i) {
    if (reported[i])
      continue;
    const String ssid = WiFi.SSID(i);
    if (ssid.length() == 0 || ssid.length() > config::kSsidMax)
      continue;
    int best = i;
    for (int j = i + 1; j < found && j < int(kMaxScanEntries); ++j) {
      if (!reported[j] && WiFi.SSID(j) == ssid) {
        reported[j] = true;
        if (WiFi.RSSI(j) > WiFi.RSSI(best))
          best = j;
      }
    }
    std::uint8_t frame[4 + config::kSsidMax];
    frame[0] = ble::kFrameWifiAp;
    frame[1] = static_cast<std::uint8_t>(static_cast<std::int8_t>(
        WiFi.RSSI(best) < -127 ? -127 : WiFi.RSSI(best)));
    frame[2] = auth_code(WiFi.encryptionType(best));
    frame[3] = static_cast<std::uint8_t>(WiFi.channel(best));
    std::memcpy(frame + 4, ssid.c_str(), ssid.length());
    if (!respond(link, link_generation, peer, frame, 4 + ssid.length()))
      break;
    ++count;
  }
  WiFi.scanDelete();
  std::uint8_t end[4];
  end[0] = ble::kFrameWifiScanEnd;
  end[1] = count & 0xff;
  end[2] = (count >> 8) & 0xff;
  end[3] = 0;
  respond(link, link_generation, peer, end, sizeof(end));
  aqlog.record_only().printf("WIFI SCAN END found=%d reported=%u\n", found,
                             unsigned(count));
  if (temporary)
    WiFi.mode(WIFI_OFF);
}

bool constant_time_equal(const std::uint8_t *a, const std::uint8_t *b,
                         std::size_t length) {
  std::uint8_t diff = 0;
  for (std::size_t i = 0; i < length; ++i)
    diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

bool token_matches(const std::uint8_t *handshake) {
  const config::Settings now = config::get();
  return std::memcmp(handshake, "AQS1", 4) == 0 &&
         constant_time_equal(handshake + 4, now.token, config::kTokenBytes);
}

void configure_client_socket(int fd) {
  timeval send_timeout{1,
                       0}; // bound one stalled peer's impact on other sessions
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
               sizeof(send_timeout));
  int nodelay = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
}

void accept_client() {
  sockaddr_in address{};
  socklen_t address_length = sizeof(address);
  const int fd = ::accept(listen_fd, reinterpret_cast<sockaddr *>(&address),
                          &address_length);
  if (fd < 0)
    return;
  configure_client_socket(fd);
  char peer_text[INET_ADDRSTRLEN] = "?";
  inet_ntop(AF_INET, &address.sin_addr, peer_text, sizeof(peer_text));
  std::uint8_t peer = 0;
  for (; peer < ble::kMaxLanClients; ++peer)
    if (clients[peer].fd.load() < 0)
      break;
  if (peer == ble::kMaxLanClients) {
    aqlog.record_only().printf("LAN REFUSE peer=%s reason=busy\n", peer_text);
    std::uint8_t frame[3 + 64];
    const auto length = error_frame(frame, static_cast<ble::Op>(0),
                                    ble::kErrBusy, "sessions-full");
    xSemaphoreTake(socket_mutex, portMAX_DELAY);
    send_framed_locked(fd, frame, length);
    ::close(fd);
    xSemaphoreGive(socket_mutex);
    return;
  }
  auto &client = clients[peer];
  client.rx_length = 0;
  client.since_us = client.last_rx_us = esp_timer_get_time();
  client.needs_snapshots = false;
  xSemaphoreTake(socket_mutex, portMAX_DELAY);
  client.drop = false;
  client.authenticated = false;
  ++client.generation;
  client.fd = fd;
  xSemaphoreGive(socket_mutex);
  set_status(connected ? "connected" : "connecting", connected);
  aqlog.record_only().printf("LAN CONNECT peer=%s slot=%u\n", peer_text,
                             unsigned(peer));
}

bool send_hello(std::uint8_t peer) {
  const char *info = ble::info_json();
  const std::size_t info_length = std::strlen(info);
  std::uint8_t frame[4 + 400];
  if (info_length > 400)
    return false;
  frame[0] = ble::kFrameHello;
  frame[1] = kProtocolVersion;
  frame[2] = kPayloadMax & 0xff;
  frame[3] = (kPayloadMax >> 8) & 0xff;
  std::memcpy(frame + 4, info, info_length);
  return send_to(peer, frame, 4 + info_length);
}

bool open_session(std::uint8_t peer) {
  auto &client = clients[peer];
  xSemaphoreTake(socket_mutex, portMAX_DELAY);
  ++client.generation;
  client.authenticated = true;
  xSemaphoreGive(socket_mutex);
  ++sessions;
  if (!send_hello(peer)) {
    close_client(peer, "hello-failed");
    return false;
  }
  client.needs_snapshots = true;
  set_status("connected", true);
  aqlog.record_only().printf("LAN AUTH result=ok slot=%u session=%lu\n",
                             unsigned(peer),
                             static_cast<unsigned long>(sessions.load()));
  return true;
}

void handle_client_bytes(std::uint8_t peer) {
  auto &client = clients[peer];
  const int count = ::recv(client.fd.load(), client.rx + client.rx_length,
                           sizeof(client.rx) - client.rx_length, 0);
  if (count <= 0) {
    close_client(peer, count == 0 ? "peer-closed" : "recv-error");
    return;
  }
  client.rx_length += static_cast<std::size_t>(count);
  client.last_rx_us = esp_timer_get_time();
  if (!client.authenticated.load()) {
    if (client.rx_length < kHandshakeBytes)
      return;
    if (!token_matches(client.rx)) {
      aqlog.record_only().println("LAN AUTH result=rejected");
      send_error_to(peer, static_cast<ble::Op>(0), ble::kErrAuth, "token");
      close_client(peer, "auth");
      return;
    }
    std::memmove(client.rx, client.rx + kHandshakeBytes,
                 client.rx_length - kHandshakeBytes);
    client.rx_length -= kHandshakeBytes;
    if (!open_session(peer))
      return;
  }
  // Framed requests: u16 length + body. Parser storage is bounded per slot.
  for (;;) {
    if (client.rx_length < 2 || client.drop.load())
      return;
    const std::size_t body = client.rx[0] | (client.rx[1] << 8);
    if (body == 0 || body > ble::kMaxControlBytes) {
      send_error_to(peer, static_cast<ble::Op>(0), ble::kErrMalformed,
                    "length");
      close_client(peer, "bad-frame");
      return;
    }
    if (client.rx_length < 2 + body)
      return;
    ble::ControlRequest request;
    request.received_mono_us = esp_timer_get_time();
    request.link = ble::Link::Lan;
    request.peer = peer;
    request.link_generation = client.generation.load();
    request.length = static_cast<std::uint16_t>(body);
    std::memcpy(request.bytes, client.rx + 2, body);
    std::memmove(client.rx, client.rx + 2 + body, client.rx_length - 2 - body);
    client.rx_length -= 2 + body;
    aqlog.record_only().printf("LAN CMD slot=%u op=0x%02x bytes=%u\n",
                               unsigned(peer), unsigned(request.bytes[0]),
                               unsigned(request.length));
    if (!request_handler(request))
      send_error_to(peer, static_cast<ble::Op>(request.bytes[0]), ble::kErrBusy,
                    "queue-full");
  }
}

void push_documents() {
  const bool new_status = status_dirty.exchange(false);
  const bool new_live = live_dirty.exchange(false);
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer) {
    auto &client = clients[peer];
    if (!client.authenticated.load() || client.drop.load())
      continue;
    std::uint8_t frame[1 + ble::kMaxJson + 16];
    if (new_status || client.needs_snapshots) {
      std::size_t length;
      portENTER_CRITICAL(&push_mutex);
      length = status_length;
      std::memcpy(frame + 1, status_json, length);
      portEXIT_CRITICAL(&push_mutex);
      frame[0] = ble::kFrameStatus;
      if (length)
        send_to(peer, frame, 1 + length);
    }
    if (new_live || client.needs_snapshots) {
      std::size_t length;
      portENTER_CRITICAL(&push_mutex);
      length = live_length;
      std::memcpy(frame + 1, live_json, length);
      portEXIT_CRITICAL(&push_mutex);
      frame[0] = ble::kFrameLive;
      if (length)
        send_to(peer, frame, 1 + length);
    }
    client.needs_snapshots = false;
  }
}

void poll_server(int wait_us = 0) {
  const bool drop_all = drop_requested.exchange(false);
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer)
    if (drop_all || clients[peer].drop.load())
      close_client(peer, "dropped");
  if (listen_fd < 0)
    return;
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(listen_fd, &readable);
  int highest = listen_fd;
  int fds[ble::kMaxLanClients];
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer) {
    fds[peer] = clients[peer].fd.load();
    if (fds[peer] >= 0) {
      FD_SET(fds[peer], &readable);
      if (fds[peer] > highest)
        highest = fds[peer];
    }
  }
  // Socket readiness wakes the task immediately; the timeout only bounds
  // radio/settings, expiry and snapshot housekeeping when the network is idle.
  timeval wait{0, wait_us};
  const int ready = ::select(highest + 1, &readable, nullptr, nullptr, &wait);
  if (ready < 0 && wait_us)
    vTaskDelay(kTick); // avoid a busy loop if the socket layer fails
  if (ready > 0) {
    if (FD_ISSET(listen_fd, &readable))
      accept_client();
    for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer)
      if (fds[peer] >= 0 && FD_ISSET(fds[peer], &readable))
        handle_client_bytes(peer);
  }
  const std::int64_t now = esp_timer_get_time();
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer) {
    const auto &client = clients[peer];
    if (client.fd.load() < 0)
      continue;
    if (!client.authenticated.load() && now - client.since_us > kHandshakeUs)
      close_client(peer, "handshake-timeout");
    else if (now - client.last_rx_us > kIdleUs)
      close_client(peer, "idle");
  }
  push_documents();
}

void lan_task(void *) {
  for (;;) {
    poll_radio();
    if (scan_pending.load()) {
      run_scan();
      scan_pending = false;
    }
    if (listen_fd >= 0)
      poll_server(100000);
    else
      vTaskDelay(kTick);
  }
}
} // namespace

bool begin(const char *host, const ble::Identity &identity,
           ble::RequestHandler handler) {
  if (!handler)
    return false;
  device_identity = identity;
  request_handler = handler;
  std::snprintf(host_label, sizeof(host_label), "%s", host);
  socket_mutex = xSemaphoreCreateMutex();
  if (!socket_mutex)
    return false;
  std::snprintf(current.host, sizeof(current.host), "%s", host_label);
  std::snprintf(current.mac, sizeof(current.mac), "%s",
                WiFi.macAddress().c_str());
  reapply = true;
  if (xTaskCreate(lan_task, "aq-lan", 8192, nullptr, 1, nullptr) != pdPASS) {
    aqlog.record_only().println("LAN ERROR operation=task");
    return false;
  }
  aqlog.record_only().printf("LAN BEGIN host=%s port=%u\n", host_label,
                             unsigned(config::kLanPort));
  return true;
}

void apply_settings() { reapply = true; }

bool request_scan(ble::Link link, std::uint32_t link_generation,
                  std::uint8_t peer) {
  // Both transport tasks may request a scan. Publish its owner before making
  // it visible to the LAN task, while excluding a second concurrent caller.
  portENTER_CRITICAL(&scan_mutex);
  if (scan_pending.load()) {
    portEXIT_CRITICAL(&scan_mutex);
    return false;
  }
  scan_link = static_cast<std::uint8_t>(link);
  scan_generation = link_generation;
  scan_peer = peer;
  scan_pending = true;
  portEXIT_CRITICAL(&scan_mutex);
  return true;
}

void drop_session() { drop_requested = true; }

bool send_response(const std::uint8_t *frame, std::size_t length,
                   std::uint8_t peer, std::uint32_t expected_generation) {
  return send_to(peer, frame, length, expected_generation);
}

bool send_error(ble::Op op, ble::Error code, const char *detail,
                std::uint8_t peer, std::uint32_t expected_generation) {
  std::uint8_t frame[3 + 64];
  const auto length = error_frame(frame, op, code, detail);
  return send_to(peer, frame, length, expected_generation);
}

void publish_status(const char *json, std::size_t length) {
  if (length > ble::kMaxJson + 16)
    return;
  portENTER_CRITICAL(&push_mutex);
  std::memcpy(status_json, json, length);
  status_length = length;
  portEXIT_CRITICAL(&push_mutex);
  status_dirty = true;
}

void publish_live(const char *json, std::size_t length) {
  if (length > ble::kMaxJson + 16)
    return;
  portENTER_CRITICAL(&push_mutex);
  std::memcpy(live_json, json, length);
  live_length = length;
  portEXIT_CRITICAL(&push_mutex);
  live_dirty = true;
}

std::uint16_t payload_max(std::uint8_t peer) {
  return peer < ble::kMaxLanClients && !drop_requested.load() &&
                 clients[peer].fd.load() >= 0 &&
                 clients[peer].authenticated.load() &&
                 !clients[peer].drop.load()
             ? kPayloadMax
             : 0;
}

std::uint32_t connection_generation(std::uint8_t peer) {
  return peer < ble::kMaxLanClients ? clients[peer].generation.load() : 0;
}

Status status() {
  Status copy;
  portENTER_CRITICAL(&status_mutex);
  copy = current;
  portEXIT_CRITICAL(&status_mutex);
  copy.client = any_client();
  copy.clients = authenticated_clients();
  copy.authenticated = copy.clients > 0;
  copy.sessions = sessions.load();
  copy.bytes_out = bytes_out.load();
  return copy;
}

std::uint32_t ui_generation() { return ui.load(); }
} // namespace lan
