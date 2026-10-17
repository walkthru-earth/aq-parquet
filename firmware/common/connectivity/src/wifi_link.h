#pragma once

// Wi-Fi station + LAN sync server, protocol v3
// (docs/shared/ble-sync-protocol.md, "LAN transport"). One FreeRTOS task owns
// the radio state machine, the mDNS responder, the listening socket and the
// bounded TCP client pool. It never touches the SD card or the display: request
// bodies go onto the same command queue the BLE and serial paths use, and the
// storage worker answers through enqueue_response(). Each peer has its own
// transmit FrameRing that only this task drains, with non-blocking sends, so a
// slow peer never stalls the worker or another peer. Wi-Fi scans run here, not
// on the worker.

#include "ble_sync.h"
#include "disconnect.h"
#include "frame_ring.h"
#include "network_time.h"

#include <cstddef>
#include <cstdint>

namespace lan {
constexpr std::uint16_t kPayloadMax = 1024;
constexpr std::uint8_t kProtocolVersion = ble::kProtocolVersion;
// ERROR frame with the longest detail a LAN ERROR carries.
constexpr std::size_t kErrorFrameBytes = 3 + 64;

// Board-neutral transmit policy, set from the sync service config.
struct Options {
  // Per peer transmit ring, in PSRAM when the board has it.
  std::size_t ring_bytes = 24 * 1024;
  // A peer with queued bytes and no byte accepted for this long is closed.
  std::uint32_t slow_peer_ms = 15000;
  // Budget to deliver BYE and pending frames before the socket closes.
  std::uint32_t close_flush_ms = 500;
  // One worker wait for ring space; samples are serviced between steps.
  std::uint32_t wait_step_ms = 200;
  // LAN task. Above the storage worker (3): it never blocks on a peer, so a
  // reply leaves as soon as the worker queues it. The stack stays internal:
  // esp_wifi_start may store PHY calibration in NVS, a flash write.
  unsigned task_priority = 4;
  std::uint32_t task_stack_bytes = 8192;
  // TCP keepalive: idle seconds, probe interval and probe count before a
  // silent peer closes with KeepaliveFailed.
  int keepalive_idle_s = 30;
  int keepalive_interval_s = 5;
  int keepalive_count = 3;
};

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
           ble::RequestHandler handler, const Options &options = {});
// Ask the task to re-read config::get() (after SET_CONFIG changed wifi/lan).
void apply_settings();
// Refresh NTP policy without reassociation or closing LAN sessions.
void apply_time_settings();
// Ask the task to run a Wi-Fi scan and answer on `link` (WIFI_AP frames then
// WIFI_SCAN_END). At most one scan is pending; a second request is refused.
bool request_scan(ble::Link link, std::uint32_t link_generation,
                  std::uint8_t peer = 0);
// Close every TCP session with a typed BYE, e.g. after the token was rotated
// or before an owner-requested reboot.
void drop_session(
    ble::DisconnectReason reason = ble::DisconnectReason::TokenRotated);

// Queue one frame for an authenticated slot without blocking. Pass the
// request's generation to reject stale replies atomically with the slot
// checks; zero addresses the current session and is reserved for immediate
// callers. Full means retry after wait_for_space(). Closed is final: the peer
// is gone, closing, or replaced, or the frame can never fit its ring (the peer
// is then closed with tx-overflow). Rejected is final for the frame only: it
// is empty or over kPayloadMax, and the peer stays connected.
aqsync::EnqueueResult enqueue_response(const std::uint8_t *frame,
                                       std::size_t length,
                                       std::uint8_t peer = 0,
                                       std::uint32_t expected_generation = 0);
aqsync::EnqueueResult enqueue_error(ble::Op op, ble::Error code,
                                    const char *detail, std::uint8_t peer = 0,
                                    std::uint32_t expected_generation = 0);
// Builds an ERROR frame into kErrorFrameBytes and returns its length.
std::size_t encode_error(std::uint8_t *frame, ble::Op op, ble::Error code,
                         const char *detail);
// Blocks for at most timeout_ms until the LAN task frees ring space or closes
// a peer. Returns true when the LAN task signalled. Never call it from the LAN
// task.
bool wait_for_space(std::uint32_t timeout_ms);
// Storage worker only. Queues a frame, waiting in Options::wait_step_ms steps
// while the ring is full. Between steps `between_steps` runs and may stop the
// wait by returning false (ABORT). Returns true once queued. The slow peer
// rule normally ends the wait because it closes a peer that stops reading.
// That rule runs in the LAN task, so the wait also gives up and returns false
// after 2 * slow_peer_ms without a space wake, which bounds it when the LAN
// task itself stops.
using WaitStep = bool (*)(void *context);
bool deliver(const std::uint8_t *frame, std::size_t length, std::uint8_t peer,
             std::uint32_t expected_generation, WaitStep between_steps,
             void *context);
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
