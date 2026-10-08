#include "ble_sync.h"
#include "debug_log.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ble {
namespace {
constexpr const char *kServiceUuid = "c0a5e9f0-0001-4b1a-9c3e-2d7f8a6b4e01";
constexpr const char *kInfoUuid = "c0a5e9f0-0002-4b1a-9c3e-2d7f8a6b4e01";
constexpr const char *kStatusUuid = "c0a5e9f0-0003-4b1a-9c3e-2d7f8a6b4e01";
constexpr const char *kLiveUuid = "c0a5e9f0-0004-4b1a-9c3e-2d7f8a6b4e01";
constexpr const char *kControlUuid = "c0a5e9f0-0005-4b1a-9c3e-2d7f8a6b4e01";
constexpr const char *kResponseUuid = "c0a5e9f0-0006-4b1a-9c3e-2d7f8a6b4e01";
constexpr std::int64_t kPairingWindowUs = 60LL * 1000000LL;
// Host back-pressure budget: with Wi-Fi active the controller can refuse
// notifications for well over 200 ms at a time (measured 2026-09-17: LIST
// over BLE aborted silently at the old 50 x 4 ms budget). 400 x 10 ms = 4 s.
constexpr unsigned kNotifyRetries = 400;
constexpr TickType_t kNotifyRetryDelay = pdMS_TO_TICKS(10);

char name_text[16]{};
char info_text[400]{};
NimBLEServer *server = nullptr;
NimBLECharacteristic *status_char = nullptr;
NimBLECharacteristic *live_char = nullptr;
NimBLECharacteristic *response_char = nullptr;

std::atomic<bool> enabled{false}, advertising{false}, connected{false},
    authenticated{false}, pairing_active{false};
std::atomic<std::uint16_t> conn_handle{BLE_HS_CONN_HANDLE_NONE}, mtu{0};
std::atomic<std::uint32_t> passkey{0}, bonds{0}, generation{0}, ui{0};
std::atomic<std::int64_t> pairing_deadline{0};
config::PairMode mode = config::PairMode::Random;
std::uint32_t fixed_passkey = 0;
// Advertising service data: flags in bits 0-7, finalized counter in bits 8-39.
// One word so two tasks racing on publish_advert() cannot tear the pair.
std::atomic<std::uint64_t> advert_word{~0ULL};
std::uint16_t boot16 = 0;
NimBLEAdvertising *advertiser = nullptr;
RequestHandler request_handler = nullptr;

std::uint64_t pack_advert(const AdvertState &state) {
  return static_cast<std::uint64_t>(state.flags) |
         (static_cast<std::uint64_t>(state.finalized) << 8);
}

void fill_advert_payload(std::uint8_t *out, const AdvertState &state) {
  out[0] = kAdvertVersion;
  out[1] = state.flags;
  out[2] = static_cast<std::uint8_t>(state.finalized);
  out[3] = static_cast<std::uint8_t>(state.finalized >> 8);
  out[4] = static_cast<std::uint8_t>(state.finalized >> 16);
  out[5] = static_cast<std::uint8_t>(state.finalized >> 24);
  out[6] = static_cast<std::uint8_t>(boot16);
  out[7] = static_cast<std::uint8_t>(boot16 >> 8);
  out[8] = 0;
  out[9] = 0;
}

// ADV PDU: flags (3) + service data (2 + 16 + 10) = 31 bytes exactly. The
// local name and the complete 128-bit UUID list go in the scan response
// (9 + 18 = 27 bytes); Android and bleak merge both into one record, and the
// offloaded filters run over each frame (APCF spec), so a service-UUID filter
// still finds the device and a service-data filter sees the flags.
bool apply_advert_data(const AdvertState &state) {
  if (!advertiser)
    return false;
  std::uint8_t payload[kAdvertPayloadBytes];
  fill_advert_payload(payload, state);
  NimBLEAdvertisementData adv;
  adv.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  if (!adv.setServiceData(NimBLEUUID(kServiceUuid), payload, sizeof(payload)))
    return false;
  return advertiser->setAdvertisementData(adv);
}

void touch_ui() { ++ui; }

bool notify(NimBLECharacteristic *characteristic, const std::uint8_t *data,
            std::size_t length, std::uint32_t expected_generation) {
  if (!characteristic || !connected.load())
    return false;
  const auto session =
      expected_generation ? expected_generation : generation.load();
  const auto handle = conn_handle.load();
  for (unsigned attempt = 0; attempt < kNotifyRetries; ++attempt) {
    if (!connected.load() || generation.load() != session ||
        conn_handle.load() != handle)
      return false;
    if (characteristic->notify(data, length, handle))
      return true;
    vTaskDelay(kNotifyRetryDelay);
  }
  return false;
}

class ServerCallbacks : public NimBLEServerCallbacks {
public:
  void onConnect(NimBLEServer *, NimBLEConnInfo &info) override {
    // Invalidate old worker replies before exposing the new peer handle.
    ++generation;
    conn_handle = info.getConnHandle();
    mtu = info.getMTU();
    advertising = false;
    authenticated = info.isAuthenticated();
    connected = true;
    touch_ui();
    aqlog.printf("BLE CONNECT peer=%s mtu=%u\n",
                 info.getAddress().toString().c_str(), unsigned(mtu.load()));
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int reason) override {
    connected = false;
    authenticated = false;
    pairing_active = false;
    conn_handle = BLE_HS_CONN_HANDLE_NONE;
    mtu = 0;
    ++generation;
    // advertiseOnDisconnect(true) restarts advertising inside the host.
    advertising = true;
    touch_ui();
    aqlog.printf("BLE DISCONNECT reason=%d\n", reason);
  }
  void onMTUChange(std::uint16_t new_mtu, NimBLEConnInfo &) override {
    mtu = new_mtu;
    touch_ui();
    aqlog.printf("BLE MTU mtu=%u\n", unsigned(new_mtu));
  }
  std::uint32_t onPassKeyDisplay() override {
    // `random`: fresh six digits for each pairing attempt, shown by the display
    // loop and printed so a bench log can reproduce the pairing. `fixed`: the
    // owner's random per-device PIN for boards without a screen; still shown
    // when a screen exists. The pairing overlay is drawn in both modes.
    const std::uint32_t key = mode == config::PairMode::Fixed
                                  ? fixed_passkey
                                  : esp_random() % 1000000U;
    passkey = key;
    pairing_deadline = esp_timer_get_time() + kPairingWindowUs;
    pairing_active = true;
    touch_ui();
    if (mode == config::PairMode::Fixed)
      aqlog.println("BLE PAIR passkey=fixed");
    else
      aqlog.printf("BLE PAIR passkey=%06lu\n", static_cast<unsigned long>(key));
    return key;
  }
  void onAuthenticationComplete(NimBLEConnInfo &info) override {
    pairing_active = false;
    // In `none` mode the link is encrypted but never authenticated; treat an
    // encrypted Just-Works link as good enough, as the characteristics do.
    authenticated = info.isEncrypted() &&
                    (mode == config::PairMode::None || info.isAuthenticated());
    bonds = static_cast<std::uint32_t>(NimBLEDevice::getNumBonds());
    touch_ui();
    if (!info.isEncrypted()) {
      aqlog.println("BLE AUTH result=not-encrypted action=disconnect");
      NimBLEDevice::getServer()->disconnect(info);
      return;
    }
    aqlog.printf("BLE BONDED peer=%s bonded=%s authenticated=%s bonds=%lu\n",
                 info.getAddress().toString().c_str(),
                 info.isBonded() ? "true" : "false",
                 info.isAuthenticated() ? "true" : "false",
                 static_cast<unsigned long>(bonds.load()));
  }
};

class ControlCallbacks : public NimBLECharacteristicCallbacks {
public:
  void onWrite(NimBLECharacteristic *characteristic,
               NimBLEConnInfo &info) override {
    const auto received = esp_timer_get_time();
    const NimBLEAttValue value = characteristic->getValue();
    ControlRequest request;
    request.received_mono_us = received;
    request.conn_handle = info.getConnHandle();
    request.link = Link::Ble;
    request.link_generation = generation.load();
    request.length = value.length() > kMaxControlBytes
                         ? kMaxControlBytes
                         : static_cast<std::uint16_t>(value.length());
    if (request.length == 0) {
      send_error(kOpList, kErrMalformed, "empty");
      return;
    }
    std::memcpy(request.bytes, value.data(), request.length);
    aqlog.printf("BLE CMD op=0x%02x bytes=%u\n", unsigned(request.bytes[0]),
                 unsigned(request.length));
    if (!request_handler(request))
      send_error(static_cast<Op>(request.bytes[0]), kErrBusy, "queue-full");
  }
};

ServerCallbacks server_callbacks;
ControlCallbacks control_callbacks;
} // namespace

bool begin(const Identity &identity, config::PairMode pairing_mode,
           std::uint32_t fixed_pin, RequestHandler handler) {
  if (!handler)
    return false;
  request_handler = handler;
  mode = pairing_mode;
  fixed_passkey = fixed_pin % 1000000U;
  const std::size_t device_length = std::strlen(identity.device);
  std::snprintf(name_text, sizeof(name_text), "AQ-%s",
                device_length >= 4 ? identity.device + device_length - 4
                                   : identity.device);
  std::snprintf(info_text, sizeof(info_text),
                "{\"proto\":%u,\"fw\":\"%s\",\"schema\":\"%s\",\"cols\":%u,"
                "\"dict\":\"%s\",\"station\":\"%s\",\"dev\":\"%s\","
                "\"boot\":\"%s\",\"max_read\":%lu}",
                unsigned(kProtocolVersion), identity.firmware, identity.schema,
                identity.columns, identity.dictionary_sha256, identity.station,
                identity.device, identity.boot,
                static_cast<unsigned long>(kMaxRead));

  if (!NimBLEDevice::init(name_text)) {
    aqlog.println("BLE ERROR operation=init");
    return false;
  }
  NimBLEDevice::setMTU(kMaxMtu);
  // bonding, MITM, LE Secure Connections. `none` drops MITM (Just Works) and
  // therefore also the AUTHEN requirement on every characteristic below.
  const bool mitm = mode != config::PairMode::None;
  NimBLEDevice::setSecurityAuth(true, mitm, true);
  NimBLEDevice::setSecurityIOCap(mitm ? BLE_HS_IO_DISPLAY_ONLY
                                      : BLE_HS_IO_NO_INPUT_OUTPUT);
  // NimBLE 2.5.1 bypasses onPassKeyDisplay() when a nondefault static
  // security passkey is set. Keep its callback path for both pairing modes:
  // our callback returns the same per-device fixed PIN while also publishing
  // pairing/UI state. The library default is only a callback sentinel; it is
  // never the passkey returned by this implementation unless that is the
  // intentionally configured per-device value.
  bonds = static_cast<std::uint32_t>(NimBLEDevice::getNumBonds());

  server = NimBLEDevice::createServer();
  server->setCallbacks(&server_callbacks);
  server->advertiseOnDisconnect(true);
  NimBLEService *service = server->createService(kServiceUuid);
  const std::uint32_t kReadSecure =
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC |
      (mitm ? static_cast<std::uint32_t>(NIMBLE_PROPERTY::READ_AUTHEN) : 0U);
  const std::uint32_t kWriteSecure =
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC |
      (mitm ? static_cast<std::uint32_t>(NIMBLE_PROPERTY::WRITE_AUTHEN) : 0U);
  NimBLECharacteristic *info_char =
      service->createCharacteristic(kInfoUuid, kReadSecure, sizeof(info_text));
  info_char->setValue(info_text);
  status_char = service->createCharacteristic(
      kStatusUuid, kReadSecure | NIMBLE_PROPERTY::NOTIFY, kMaxJson + 16);
  status_char->setValue("{}");
  live_char = service->createCharacteristic(
      kLiveUuid, kReadSecure | NIMBLE_PROPERTY::NOTIFY, kMaxJson + 16);
  live_char->setValue("{}");
  NimBLECharacteristic *control_char = service->createCharacteristic(
      kControlUuid, kWriteSecure, kMaxControlBytes);
  control_char->setCallbacks(&control_callbacks);
  response_char = service->createCharacteristic(
      kResponseUuid, NIMBLE_PROPERTY::NOTIFY | kReadSecure, kMaxMtu);
  // NimBLE 2.5.1 starts services with the server; Service::start is a no-op.

  // The last four hex digits of the boot id travel in the advertisement so a
  // phone sees a reboot (the finalized counter restarts) without connecting.
  {
    const std::size_t boot_length = std::strlen(identity.boot);
    const char *tail =
        boot_length >= 4 ? identity.boot + boot_length - 4 : identity.boot;
    boot16 = static_cast<std::uint16_t>(std::strtoul(tail, nullptr, 16));
  }

  advertiser = NimBLEDevice::getAdvertising();
  advertiser->enableScanResponse(true);
  AdvertState initial;
  initial.flags = kAdvNoUtc; // refined by the first publish_advert()
  if (!apply_advert_data(initial)) {
    aqlog.println("BLE ERROR operation=advert-data");
    return false;
  }
  advert_word = pack_advert(initial);
  NimBLEAdvertisementData scan_response;
  scan_response.setName(name_text);
  scan_response.setCompleteServices(NimBLEUUID(kServiceUuid));
  if (!advertiser->setScanResponseData(scan_response)) {
    aqlog.println("BLE ERROR operation=scan-response-data");
    return false;
  }
  if (!advertiser->start()) {
    aqlog.println("BLE ERROR operation=advertise");
    return false;
  }
  enabled = true;
  advertising = true;
  touch_ui();
  aqlog.printf("BLE BEGIN name=%s service=%s mtu_max=%u bonds=%lu pair=%s "
               "adv_ver=%u boot16=%04x\n",
               name_text, kServiceUuid, unsigned(kMaxMtu),
               static_cast<unsigned long>(bonds.load()),
               config::pair_name(mode), unsigned(kAdvertVersion),
               unsigned(boot16));
  return true;
}

const char *local_name() { return name_text; }
const char *info_json() { return info_text; }
config::PairMode pair_mode() { return mode; }

void clear_bonds() {
  NimBLEDevice::deleteAllBonds();
  bonds = static_cast<std::uint32_t>(NimBLEDevice::getNumBonds());
  touch_ui();
  aqlog.printf("BLE BONDS CLEARED bonds=%lu\n",
               static_cast<unsigned long>(bonds.load()));
}

void publish_live(const char *json, std::size_t length) {
  if (!enabled.load() || !live_char)
    return;
  live_char->setValue(reinterpret_cast<const std::uint8_t *>(json), length);
  if (connected.load() && authenticated.load() &&
      snapshot_notification_fits(length, mtu.load()))
    live_char->notify(reinterpret_cast<const std::uint8_t *>(json), length,
                      conn_handle.load());
}

void publish_status(const char *json, std::size_t length) {
  if (!enabled.load() || !status_char)
    return;
  status_char->setValue(reinterpret_cast<const std::uint8_t *>(json), length);
  if (connected.load() && authenticated.load() &&
      snapshot_notification_fits(length, mtu.load()))
    status_char->notify(reinterpret_cast<const std::uint8_t *>(json), length,
                        conn_handle.load());
}

void publish_advert(const AdvertState &state) {
  if (!enabled.load())
    return;
  const std::uint64_t next = pack_advert(state);
  const std::uint64_t previous = advert_word.exchange(next);
  if (previous == next)
    return;
  // Legacy advertising data may be replaced while advertising (and while
  // connected, for the restart on disconnect); NimBLE issues one HCI command.
  if (!apply_advert_data(state)) {
    aqlog.println("BLE ERROR operation=advert-refresh");
    return;
  }
  touch_ui();
  aqlog.printf("BLE ADV flags=0x%02x fin=%lu\n", unsigned(state.flags),
               static_cast<unsigned long>(state.finalized));
}

bool send_response(const std::uint8_t *frame, std::size_t length,
                   std::uint32_t expected_generation) {
  return notify(response_char, frame, length, expected_generation);
}

bool send_error(Op op, Error code, const char *detail,
                std::uint32_t expected_generation) {
  std::uint8_t frame[3 + 64];
  frame[0] = kFrameError;
  frame[1] = static_cast<std::uint8_t>(op);
  frame[2] = static_cast<std::uint8_t>(code);
  std::size_t length = 3;
  if (detail) {
    const std::size_t detail_length = std::strlen(detail);
    const std::size_t copy = detail_length > 64 ? 64 : detail_length;
    std::memcpy(frame + 3, detail, copy);
    length += copy;
  }
  aqlog.printf("BLE ERROR op=0x%02x code=%u\n", unsigned(op), unsigned(code));
  return send_response(frame, length, expected_generation);
}

std::uint16_t payload_max() {
  const auto current = mtu.load();
  return current > 3 ? static_cast<std::uint16_t>(current - 3) : 0;
}

std::uint32_t connection_generation() { return generation.load(); }

PairingState pairing() {
  PairingState state;
  state.active = pairing_active.load();
  state.passkey = passkey.load();
  state.deadline_us = pairing_deadline.load();
  if (state.active && esp_timer_get_time() > state.deadline_us)
    state.active = false;
  return state;
}

LinkState link() {
  LinkState state;
  state.enabled = enabled.load();
  state.advertising = advertising.load();
  state.connected = connected.load();
  state.authenticated = authenticated.load();
  state.mtu = mtu.load();
  state.bonds = bonds.load();
  state.advert_flags = static_cast<std::uint8_t>(advert_word.load() & 0xffU);
  return state;
}

std::uint32_t ui_generation() { return ui.load(); }
} // namespace ble
