#pragma once

// Bluetooth LE sync service, protocol v2: docs/shared/ble-sync-protocol.md.
//
// Threading contract. NimBLE callbacks run on the host task and never touch the
// SD card or the display. `control` writes become ControlRequest values that
// the storage worker dequeues and executes; the worker emits `response` frames
// through send_response(). The sampling loop publishes `live`/`status` JSON.
// The display loop reads pairing()/link() and draws; nothing here draws.

#include "device_config.h"

#include <cstddef>
#include <cstdint>

namespace ble {
constexpr std::uint8_t kProtocolVersion = 2;
constexpr std::size_t kMaxControlBytes = 512;
constexpr std::uint32_t kMaxRead = 16384;
constexpr std::uint16_t kMaxMtu = 517;
// Largest frame ever notified: the GATT attribute-value limit, not MTU - 3.
constexpr std::uint16_t kMaxFrame = 512;
constexpr std::size_t kMaxJson = 240;

enum Op : std::uint8_t {
  kOpList = 0x01,
  kOpOpen = 0x02,
  kOpRead = 0x03,
  kOpClose = 0x04,
  kOpSetTime = 0x05,
  kOpFlush = 0x06,
  kOpStatus = 0x07,
  kOpGetConfig = 0x08,
  kOpSetConfig = 0x09,
  kOpWifiScan = 0x0a,
  kOpReboot = 0x0b,
  kOpLogTail = 0x0c,
  kOpGetToken = 0x0d,
};
enum Frame : std::uint8_t {
  kFrameFile = 0x10,
  kFrameListEnd = 0x11,
  kFrameOpened = 0x20,
  kFrameChunk = 0x21,
  kFrameReadEnd = 0x22,
  kFrameClosed = 0x23,
  kFrameTimeSet = 0x30,
  kFrameFlushed = 0x31,
  kFrameConfig = 0x40,
  kFrameWifiAp = 0x41,
  kFrameWifiScanEnd = 0x42,
  kFrameRebooting = 0x43,
  kFrameLog = 0x44,
  kFrameLogEnd = 0x45,
  kFrameToken = 0x46,
  kFrameHello = 0x47,
  kFrameStatus = 0x48,
  kFrameLive = 0x49,
  kFrameError = 0x7f,
};
enum Error : std::uint8_t {
  kErrMalformed = 1,
  kErrInvalidName = 2,
  kErrNotFinalized = 3,
  kErrOpenFailed = 4,
  kErrBadHandle = 5,
  kErrRange = 6,
  kErrBusy = 7,
  kErrStorage = 8,
  kErrInvalidEpoch = 9,
  kErrNothingToFlush = 10,
  kErrUnknownOp = 11,
  kErrInvalidConfig = 12,
  kErrNotOnThisLink = 13,
  kErrAuth = 14,
  kErrWifiUnavailable = 15,
};

// Advertising service data (protocol v2.1, docs/shared/ble-sync-protocol.md
// "Advertising payload"). Ten bytes under the service UUID in the ADV PDU so a
// phone's offloaded scan filter can wake its app on a flag bit; the name and
// the 128-bit UUID list ride in the scan response. Little-endian.
constexpr std::uint8_t kAdvertVersion = 1;
constexpr std::size_t kAdvertPayloadBytes = 10;
enum AdvertFlag : std::uint8_t {
  kAdvNoUtc = 1U << 0,         // no UTC anchor this boot: rows go to `unsynced`
  kAdvNewFiles = 1U << 1,      // a file finalized since the last completed LIST
  kAdvSd = 1U << 2,            // card mounted, output directory exists
  kAdvLan = 1U << 3,           // Wi-Fi connected and the LAN server is up
  kAdvFail = 1U << 4,          // storage worker stopped after an error
  kAdvClockRestored = 1U << 5, // clock_status == 2: dated from the RTC only
};
struct AdvertState {
  std::uint8_t flags = 0;
  std::uint32_t finalized = 0; // same counter as status.fin
};

// Which transport a control request arrived on. The storage worker answers on
// the same link (ble::send_response or lan::send_response) and a file handle
// is only honoured on the link that opened it.
enum class Link : std::uint8_t { Ble = 0, Lan = 1 };

struct ControlRequest {
  std::uint8_t bytes[kMaxControlBytes]{};
  std::uint16_t length = 0;
  std::uint16_t conn_handle = 0;
  std::int64_t received_mono_us = 0;
  Link link = Link::Ble;
  std::uint32_t link_generation = 0;
};

using RequestHandler = bool (*)(const ControlRequest &request);

struct Identity {
  const char *station;
  const char *device;
  const char *boot;
  const char *schema;
  unsigned columns;
  const char *dictionary_sha256;
  const char *firmware;
};

struct PairingState {
  bool active = false;
  std::uint32_t passkey = 0;
  std::int64_t deadline_us = 0;
};
struct LinkState {
  bool enabled = false;
  bool advertising = false;
  bool connected = false;
  bool authenticated = false;
  std::uint16_t mtu = 0;
  std::uint32_t bonds = 0;
  std::uint8_t advert_flags = 0; // last advertised AdvertFlag bits
};

// Call once after the firmware command queue and config::load(); identities
// must outlive the program. `mode`/`fixed_pin` are the pairing settings the
// stack starts with (they cannot change until the next boot). Returns false
// when the stack could not start (logging continues).
bool begin(const Identity &identity, config::PairMode mode,
           std::uint32_t fixed_pin, RequestHandler handler);
const char *local_name();
const char *info_json();
config::PairMode pair_mode();
// Forgets every bonded peer (ble.clear_bonds). Safe from any task.
void clear_bonds();

// From the sampling loop, after the row was queued. Payloads <= kMaxJson.
void publish_live(const char *json, std::size_t length);
void publish_status(const char *json, std::size_t length);
// Alongside publish_status(): refreshes the advertising service data when the
// flags or the finalized-file counter changed (one HCI command), otherwise a
// compare. Safe from any task; a no-op until begin() succeeded.
void publish_advert(const AdvertState &state);

// From the storage worker. Blocks briefly on host back-pressure; false when
// the peer is gone or the host kept refusing the notification.
bool send_response(const std::uint8_t *frame, std::size_t length);
bool send_error(Op op, Error code, const char *detail);
// Largest `response` payload for the current link (MTU - 3), or 0 if none.
std::uint16_t payload_max();
// Increments on every connect/disconnect so the worker can drop stale handles.
std::uint32_t connection_generation();

PairingState pairing();
LinkState link();
// Increments whenever pairing()/link() changes; cheap to poll from the UI.
std::uint32_t ui_generation();
} // namespace ble
