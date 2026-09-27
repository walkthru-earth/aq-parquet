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

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace lan {
namespace {
constexpr TickType_t kTick = pdMS_TO_TICKS(100);
constexpr std::int64_t kConnectTimeoutUs = 30LL * 1000000LL;
constexpr std::int64_t kRetryUs = 30LL * 1000000LL;
constexpr std::int64_t kHandshakeUs = 5LL * 1000000LL;
constexpr std::int64_t kIdleUs = 300LL * 1000000LL;
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
std::atomic<int> client_fd{-1};
std::atomic<bool> client_authenticated{false};
std::atomic<std::uint32_t> generation{0}, ui{0}, sessions{0}, bytes_out{0};

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
  current.client = client_fd.load() >= 0;
  current.authenticated = client_authenticated.load();
  current.sessions = sessions.load();
  current.bytes_out = bytes_out.load();
  portEXIT_CRITICAL(&status_mutex);
  touch_ui();
}

// ---- socket helpers (all writes go through here, under socket_mutex)

bool write_all(int fd, const std::uint8_t *data, std::size_t length) {
  std::size_t sent = 0;
  while (sent < length) {
    const int count = ::send(fd, data + sent, length - sent, 0);
    if (count <= 0) {
      // A framed stream must never skip a frame. If the peer stopped
      // draining (phone Wi-Fi in power save, measured 2026-09-17: the
      // OPENED reply vanished and the phone waited 60 s), end the session so
      // the peer sees EOF at once and reconnects/resumes.
      aqlog.printf(
          "LAN SEND FAILED errno=%d sent=%u of=%u action=drop-session\n", errno,
          unsigned(sent), unsigned(length));
      drop_requested = true;
      return false;
    }
    sent += static_cast<std::size_t>(count);
  }
  bytes_out += static_cast<std::uint32_t>(length);
  return true;
}

bool send_framed(int fd, const std::uint8_t *frame, std::size_t length) {
  if (fd < 0 || length == 0 || length > kPayloadMax)
    return false;
  std::uint8_t buffer[2 + kPayloadMax];
  buffer[0] = length & 0xff;
  buffer[1] = (length >> 8) & 0xff;
  std::memcpy(buffer + 2, frame, length);
  if (xSemaphoreTake(socket_mutex, pdMS_TO_TICKS(5000)) != pdTRUE)
    return false;
  const bool ok = write_all(fd, buffer, 2 + length);
  xSemaphoreGive(socket_mutex);
  return ok;
}

bool send_error_to(int fd, ble::Op op, ble::Error code, const char *detail) {
  std::uint8_t frame[3 + 64];
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
  aqlog.printf("LAN ERROR op=0x%02x code=%u\n", unsigned(op), unsigned(code));
  return send_framed(fd, frame, length);
}

void close_client(const char *reason) {
  const int fd = client_fd.exchange(-1);
  const bool was_authenticated = client_authenticated.exchange(false);
  if (fd >= 0) {
    if (xSemaphoreTake(socket_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
      ::close(fd);
      xSemaphoreGive(socket_mutex);
    } else {
      ::close(fd);
    }
    aqlog.printf("LAN DISCONNECT reason=%s authenticated=%s\n", reason,
                 was_authenticated ? "true" : "false");
  }
  if (was_authenticated)
    ++generation;
  touch_ui();
}

// ---- server task state

int listen_fd = -1;
bool radio_on = false, connected = false, mdns_on = false;
std::int64_t connect_started_us = 0, retry_at_us = 0;
config::Settings settings;

// Handshake / frame parser for the one client.
std::uint8_t rx[2 + ble::kMaxControlBytes + kHandshakeBytes];
std::size_t rx_length = 0;
bool handshake_done = false;
std::int64_t client_since_us = 0, last_rx_us = 0;

// A second connection while a session is active. It gets kHandshakeUs to
// present the token; a valid token takes the slot over (the old session is
// most often half-open: Android destroys a backgrounded app's sockets without
// a FIN, so the device would otherwise hold the slot until the idle timeout).
int challenger_fd = -1;
std::uint8_t challenger_rx[kHandshakeBytes];
std::size_t challenger_length = 0;
std::int64_t challenger_since_us = 0;
char challenger_peer[INET_ADDRSTRLEN] = "?";
void close_challenger(const char *reason);

void start_server() {
  if (listen_fd >= 0 || !settings.lan_on)
    return;
  listen_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_fd < 0) {
    aqlog.printf("LAN ERROR operation=socket errno=%d\n", errno);
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
      ::listen(listen_fd, 1) != 0) {
    aqlog.printf("LAN ERROR operation=bind-listen errno=%d\n", errno);
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
    aqlog.println("LAN ERROR operation=mdns");
  }
  portENTER_CRITICAL(&status_mutex);
  current.mdns = mdns_on;
  portEXIT_CRITICAL(&status_mutex);
  aqlog.printf(
      "LAN LISTEN port=%u host=%s.local mdns=%s service=_aqsync._tcp\n",
      unsigned(config::kLanPort), host_label, mdns_on ? "true" : "false");
}

void stop_server(const char *reason) {
  close_client(reason);
  close_challenger(reason);
  if (mdns_on) {
    MDNS.end();
    mdns_on = false;
  }
  if (listen_fd >= 0) {
    ::close(listen_fd);
    listen_fd = -1;
    aqlog.printf("LAN STOP reason=%s\n", reason);
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
    aqlog.printf("WIFI OFF reason=%s\n", reason);
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
  aqlog.printf("WIFI CONNECT ssid=%s\n", settings.ssid);
}

void poll_radio() {
  const std::int64_t now = esp_timer_get_time();
  if (reapply.exchange(false)) {
    settings = config::get();
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
    aqlog.printf("WIFI CONNECTED ssid=%s ip=%s rssi=%d\n", settings.ssid,
                 WiFi.localIP().toString().c_str(), WiFi.RSSI());
    start_server();
  } else if (!up && connected) {
    connected = false;
    stop_server("wifi-lost");
    aqlog.println("WIFI LOST");
    radio_connect();
  } else if (!up && retry_at_us == 0 &&
             now - connect_started_us > kConnectTimeoutUs) {
    set_status("failed", false);
    retry_at_us = now + kRetryUs;
    aqlog.printf("WIFI FAILED ssid=%s status=%d retry_s=%lld\n", settings.ssid,
                 int(WiFi.status()),
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

bool respond(ble::Link link, std::uint32_t link_generation,
             const std::uint8_t *frame, std::size_t length) {
  if (link == ble::Link::Lan) {
    if (link_generation != generation.load())
      return false;
    return send_response(frame, length);
  }
  return ble::send_response(frame, length);
}

void run_scan() {
  const auto link = static_cast<ble::Link>(scan_link.load());
  const auto link_generation = scan_generation.load();
  const bool temporary = !radio_on;
  if (temporary) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
  }
  aqlog.println("WIFI SCAN BEGIN");
  const int found = WiFi.scanNetworks(false, false);
  if (found < 0) {
    aqlog.printf("WIFI SCAN FAILED code=%d\n", found);
    std::uint8_t frame[4];
    frame[0] = ble::kFrameWifiScanEnd;
    frame[1] = 0;
    frame[2] = 0;
    frame[3] = ble::kErrWifiUnavailable;
    respond(link, link_generation, frame, sizeof(frame));
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
    if (!respond(link, link_generation, frame, 4 + ssid.length()))
      break;
    ++count;
  }
  WiFi.scanDelete();
  std::uint8_t end[4];
  end[0] = ble::kFrameWifiScanEnd;
  end[1] = count & 0xff;
  end[2] = (count >> 8) & 0xff;
  end[3] = 0;
  respond(link, link_generation, end, sizeof(end));
  aqlog.printf("WIFI SCAN END found=%d reported=%u\n", found, unsigned(count));
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
  timeval send_timeout{15, 0}; // sleepy phones stall for seconds, not minutes
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
               sizeof(send_timeout));
  int nodelay = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
}

void close_challenger(const char *reason) {
  if (challenger_fd < 0)
    return;
  ::close(challenger_fd);
  challenger_fd = -1;
  challenger_length = 0;
  aqlog.printf("LAN CHALLENGER peer=%s result=%s\n", challenger_peer, reason);
}

void accept_client() {
  sockaddr_in peer{};
  socklen_t peer_length = sizeof(peer);
  const int fd =
      ::accept(listen_fd, reinterpret_cast<sockaddr *>(&peer), &peer_length);
  if (fd < 0)
    return;
  char peer_text[INET_ADDRSTRLEN] = "?";
  inet_ntop(AF_INET, &peer.sin_addr, peer_text, sizeof(peer_text));
  if (client_fd.load() >= 0) {
    if (challenger_fd >= 0) {
      aqlog.printf("LAN REFUSE peer=%s reason=busy\n", peer_text);
      send_error_to(fd, static_cast<ble::Op>(0), ble::kErrBusy,
                    "session-active");
      ::close(fd);
      return;
    }
    // Hold the newcomer aside: with the token it takes the slot over.
    configure_client_socket(fd);
    challenger_fd = fd;
    challenger_length = 0;
    challenger_since_us = esp_timer_get_time();
    std::memcpy(challenger_peer, peer_text, sizeof(challenger_peer));
    aqlog.printf("LAN CHALLENGER peer=%s result=waiting-for-token\n",
                 peer_text);
    return;
  }
  configure_client_socket(fd);
  rx_length = 0;
  handshake_done = false;
  client_since_us = last_rx_us = esp_timer_get_time();
  client_fd = fd;
  client_authenticated = false;
  set_status(connected ? "connected" : "connecting", connected);
  aqlog.printf("LAN CONNECT peer=%s\n", peer_text);
}

void send_hello(int fd) {
  const char *info = ble::info_json();
  const std::size_t info_length = std::strlen(info);
  std::uint8_t frame[4 + 400];
  frame[0] = ble::kFrameHello;
  frame[1] = kProtocolVersion;
  frame[2] = kPayloadMax & 0xff;
  frame[3] = (kPayloadMax >> 8) & 0xff;
  std::memcpy(frame + 4, info, info_length);
  send_framed(fd, frame, 4 + info_length);
}

// The client behind fd has just proven the token: HELLO, then the current
// STATUS/LIVE documents so the new session need not wait for the next push.
void open_session(int fd) {
  handshake_done = true;
  client_authenticated = true;
  ++generation;
  ++sessions;
  send_hello(fd);
  status_dirty = status_length > 0;
  live_dirty = live_length > 0;
  set_status("connected", true);
  aqlog.printf("LAN AUTH result=ok session=%lu\n",
               static_cast<unsigned long>(sessions.load()));
}

void handle_challenger_bytes() {
  const int count = ::recv(challenger_fd, challenger_rx + challenger_length,
                           kHandshakeBytes - challenger_length, 0);
  if (count <= 0) {
    close_challenger(count == 0 ? "peer-closed" : "recv-error");
    return;
  }
  challenger_length += static_cast<std::size_t>(count);
  if (challenger_length < kHandshakeBytes)
    return;
  if (!token_matches(challenger_rx)) {
    send_error_to(challenger_fd, static_cast<ble::Op>(0), ble::kErrAuth,
                  "token");
    close_challenger("auth-rejected");
    return;
  }
  // Valid token: the newcomer wins, the old session is closed without a reply
  // (its pending worker replies are dropped by the generation check).
  close_client("preempted");
  const int fd = challenger_fd;
  challenger_fd = -1;
  challenger_length = 0;
  rx_length = 0;
  client_since_us = last_rx_us = esp_timer_get_time();
  client_fd = fd;
  aqlog.printf("LAN TAKEOVER peer=%s\n", challenger_peer);
  open_session(fd);
}

void handle_client_bytes() {
  const int fd = client_fd.load();
  const int count = ::recv(fd, rx + rx_length, sizeof(rx) - rx_length, 0);
  if (count <= 0) {
    close_client(count == 0 ? "peer-closed" : "recv-error");
    return;
  }
  rx_length += static_cast<std::size_t>(count);
  last_rx_us = esp_timer_get_time();
  if (!handshake_done) {
    if (rx_length < kHandshakeBytes)
      return;
    if (!token_matches(rx)) {
      aqlog.println("LAN AUTH result=rejected");
      send_error_to(fd, static_cast<ble::Op>(0), ble::kErrAuth, "token");
      close_client("auth");
      return;
    }
    std::memmove(rx, rx + kHandshakeBytes, rx_length - kHandshakeBytes);
    rx_length -= kHandshakeBytes;
    open_session(fd);
  }
  // Framed requests: u16 length + body.
  for (;;) {
    if (rx_length < 2)
      return;
    const std::size_t body = rx[0] | (rx[1] << 8);
    if (body == 0 || body > ble::kMaxControlBytes) {
      send_error_to(fd, static_cast<ble::Op>(0), ble::kErrMalformed, "length");
      close_client("bad-frame");
      return;
    }
    if (rx_length < 2 + body)
      return;
    ble::ControlRequest request;
    request.received_mono_us = esp_timer_get_time();
    request.link = ble::Link::Lan;
    request.link_generation = generation.load();
    request.length = static_cast<std::uint16_t>(body);
    std::memcpy(request.bytes, rx + 2, body);
    std::memmove(rx, rx + 2 + body, rx_length - 2 - body);
    rx_length -= 2 + body;
    aqlog.printf("LAN CMD op=0x%02x bytes=%u\n", unsigned(request.bytes[0]),
                 unsigned(request.length));
    if (!request_handler(request))
      send_error_to(fd, static_cast<ble::Op>(request.bytes[0]), ble::kErrBusy,
                    "queue-full");
  }
}

void push_documents() {
  const int fd = client_fd.load();
  if (fd < 0 || !client_authenticated.load())
    return;
  std::uint8_t frame[1 + ble::kMaxJson + 16];
  if (status_dirty.exchange(false)) {
    std::size_t length;
    portENTER_CRITICAL(&push_mutex);
    length = status_length;
    std::memcpy(frame + 1, status_json, length);
    portEXIT_CRITICAL(&push_mutex);
    frame[0] = ble::kFrameStatus;
    send_framed(fd, frame, 1 + length);
  }
  if (live_dirty.exchange(false)) {
    std::size_t length;
    portENTER_CRITICAL(&push_mutex);
    length = live_length;
    std::memcpy(frame + 1, live_json, length);
    portEXIT_CRITICAL(&push_mutex);
    frame[0] = ble::kFrameLive;
    send_framed(fd, frame, 1 + length);
  }
}

void poll_server() {
  if (drop_requested.exchange(false))
    close_client("dropped");
  if (listen_fd < 0)
    return;
  const int fd = client_fd.load();
  const int challenger = challenger_fd;
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(listen_fd, &readable);
  int highest = listen_fd;
  if (fd >= 0) {
    FD_SET(fd, &readable);
    if (fd > highest)
      highest = fd;
  }
  if (challenger >= 0) {
    FD_SET(challenger, &readable);
    if (challenger > highest)
      highest = challenger;
  }
  timeval wait{0, 0};
  if (::select(highest + 1, &readable, nullptr, nullptr, &wait) > 0) {
    if (FD_ISSET(listen_fd, &readable))
      accept_client();
    if (fd >= 0 && FD_ISSET(fd, &readable))
      handle_client_bytes();
    if (challenger >= 0 && challenger_fd == challenger &&
        FD_ISSET(challenger, &readable))
      handle_challenger_bytes();
  }
  const std::int64_t now = esp_timer_get_time();
  if (challenger_fd >= 0 && now - challenger_since_us > kHandshakeUs)
    close_challenger("handshake-timeout");
  const int current_fd = client_fd.load();
  if (current_fd >= 0) {
    if (!handshake_done && now - client_since_us > kHandshakeUs)
      close_client("handshake-timeout");
    else if (now - last_rx_us > kIdleUs)
      close_client("idle");
    else
      push_documents();
  }
}

void lan_task(void *) {
  for (;;) {
    poll_radio();
    if (scan_pending.load()) {
      run_scan();
      scan_pending = false;
    }
    poll_server();
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
    aqlog.println("LAN ERROR operation=task");
    return false;
  }
  aqlog.printf("LAN BEGIN host=%s port=%u\n", host_label,
               unsigned(config::kLanPort));
  return true;
}

void apply_settings() { reapply = true; }

bool request_scan(ble::Link link, std::uint32_t link_generation) {
  // Both transport tasks may request a scan. Publish its owner before making
  // it visible to the LAN task, while excluding a second concurrent caller.
  portENTER_CRITICAL(&scan_mutex);
  if (scan_pending.load()) {
    portEXIT_CRITICAL(&scan_mutex);
    return false;
  }
  scan_link = static_cast<std::uint8_t>(link);
  scan_generation = link_generation;
  scan_pending = true;
  portEXIT_CRITICAL(&scan_mutex);
  return true;
}

void drop_session() { drop_requested = true; }

bool send_response(const std::uint8_t *frame, std::size_t length) {
  const int fd = client_fd.load();
  if (fd < 0 || !client_authenticated.load())
    return false;
  return send_framed(fd, frame, length);
}

bool send_error(ble::Op op, ble::Error code, const char *detail) {
  const int fd = client_fd.load();
  if (fd < 0 || !client_authenticated.load())
    return false;
  return send_error_to(fd, op, code, detail);
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

std::uint16_t payload_max() {
  return client_fd.load() >= 0 && client_authenticated.load() ? kPayloadMax : 0;
}

std::uint32_t connection_generation() { return generation.load(); }

Status status() {
  Status copy;
  portENTER_CRITICAL(&status_mutex);
  copy = current;
  portEXIT_CRITICAL(&status_mutex);
  copy.client = client_fd.load() >= 0;
  copy.authenticated = client_authenticated.load();
  copy.sessions = sessions.load();
  copy.bytes_out = bytes_out.load();
  return copy;
}

std::uint32_t ui_generation() { return ui.load(); }
} // namespace lan
