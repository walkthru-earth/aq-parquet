#pragma once

// Wi-Fi station + LAN sync server, protocol v2
// (docs/shared/ble-sync-protocol.md, "LAN transport"). One FreeRTOS task owns
// the radio state machine, the mDNS responder, the listening socket and the
// bounded TCP client pool. It never touches the SD card or the display: request
// bodies go onto the same command queue the BLE and serial paths use, and the
// storage worker answers through send_response(). Wi-Fi scans run here, not on
// the worker.

#include "ble_sync.h"
#include "network_time.h"

#include <cstddef>
#include <cstdint>

namespace lan {
constexpr std::uint16_t kPayloadMax = 1024;
constexpr std::uint8_t kProtocolVersion = ble::kProtocolVersion;

using TimeAnchor = aq::network_time::Anchor;
// One bounded pending SNTP result, consumed by the logger main loop. No IO.
bool take_time_anchor(TimeAnchor &anchor);

struct Status {
  const char *state = "off"; // off | connecting | connected | failed
  char ip[16] = "";
  int rssi = 0;
  char mac[18] = "";
  char host[24] = "";
  bool mdns = false;
  bool client = false;        // a TCP client is connected
  bool authenticated = false; // ...and passed the token handshake
  std::uint8_t clients = 0; // authenticated TCP clients (up to kMaxLanClients)
  std::uint32_t sessions = 0; // authenticated sessions since boot
  std::uint32_t bytes_out = 0;
};

// Starts the LAN task; the radio itself only comes up when the stored settings
// say `wifi.on` with an SSID. `host` is the mDNS label (`aq-xxxx`).
// The identity strings and handler must outlive the LAN task. The handler
// enqueues/copies requests and may be invoked concurrently with BLE.
bool begin(const char *host, const ble::Identity &identity,
           ble::RequestHandler handler);
// Ask the task to re-read config::get() (after SET_CONFIG changed wifi/lan).
void apply_settings();
// Refresh NTP policy without reassociation or closing LAN sessions.
void apply_time_settings();
// Ask the task to run a Wi-Fi scan and answer on `link` (WIFI_AP frames then
// WIFI_SCAN_END). At most one scan is pending; a second request is refused.
bool request_scan(ble::Link link, std::uint32_t link_generation,
                  std::uint8_t peer = 0);
// Close every TCP session, e.g. after the token was rotated.
void drop_session();

// From the storage worker: send to one authenticated slot. Pass the request's
// generation to reject stale replies atomically with socket identity checks;
// zero addresses the current session and is reserved for immediate callers.
bool send_response(const std::uint8_t *frame, std::size_t length,
                   std::uint8_t peer = 0,
                   std::uint32_t expected_generation = 0);
bool send_error(ble::Op op, ble::Error code, const char *detail,
                std::uint8_t peer = 0, std::uint32_t expected_generation = 0);
// Latest status/live JSON; pushed by the LAN task on its next tick.
void publish_status(const char *json, std::size_t length);
void publish_live(const char *json, std::size_t length);
// kPayloadMax while an authenticated client is connected, else 0.
std::uint16_t payload_max(std::uint8_t peer = 0);
// Increments on every session start/end so the worker can drop stale handles.
std::uint32_t connection_generation(std::uint8_t peer = 0);

Status status();
std::uint32_t ui_generation();
} // namespace lan
