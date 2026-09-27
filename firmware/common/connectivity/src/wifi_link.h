#pragma once

// Wi-Fi station + LAN sync server, protocol v2
// (docs/shared/ble-sync-protocol.md, "LAN transport"). One FreeRTOS task owns
// the radio state machine, the mDNS responder, the listening socket and the
// single TCP client. It never touches the SD card or the display: request
// bodies go onto the same command queue the BLE and serial paths use, and the
// storage worker answers through send_response(). Wi-Fi scans run here, not on
// the worker.

#include "ble_sync.h"

#include <cstddef>
#include <cstdint>

namespace lan {
constexpr std::uint16_t kPayloadMax = 1024;
constexpr std::uint8_t kProtocolVersion = ble::kProtocolVersion;

struct Status {
  const char *state = "off"; // off | connecting | connected | failed
  char ip[16] = "";
  int rssi = 0;
  char mac[18] = "";
  char host[24] = "";
  bool mdns = false;
  bool client = false;        // a TCP client is connected
  bool authenticated = false; // ...and passed the token handshake
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
// Ask the task to run a Wi-Fi scan and answer on `link` (WIFI_AP frames then
// WIFI_SCAN_END). At most one scan is pending; a second request is refused.
bool request_scan(ble::Link link, std::uint32_t link_generation);
// Close the current TCP session, e.g. after the token was rotated.
void drop_session();

// From the storage worker: send one frame to the authenticated TCP client.
bool send_response(const std::uint8_t *frame, std::size_t length);
bool send_error(ble::Op op, ble::Error code, const char *detail);
// Latest status/live JSON; pushed by the LAN task on its next tick.
void publish_status(const char *json, std::size_t length);
void publish_live(const char *json, std::size_t length);
// kPayloadMax while an authenticated client is connected, else 0.
std::uint16_t payload_max();
// Increments on every session start/end so the worker can drop stale handles.
std::uint32_t connection_generation();

Status status();
std::uint32_t ui_generation();
} // namespace lan
