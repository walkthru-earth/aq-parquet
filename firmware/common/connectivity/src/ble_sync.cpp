#include "ble_sync.h"
#include "ble_advert.h"
#include "debug_log.h"

#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

extern "C" {
#include <host/ble_hs.h>
#include <host/ble_store.h>
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>
void ble_store_config_init(void);
}

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ble {
namespace {
constexpr const char *kServiceUuid = "c0a5e9f0-0001-4b1a-9c3e-2d7f8a6b4e01";
constexpr std::int64_t kPairingWindowUs = 60LL * 1000000LL;
// Preserve the measured controller back-pressure budget: 400 x 10 ms = 4 s.
constexpr unsigned kNotifyRetries = 400;
constexpr TickType_t kNotifyRetryDelay = pdMS_TO_TICKS(10);
// Native NimBLE UUID bytes use the same Bluetooth wire order as ADV.
ble_uuid128_t native_uuid(std::uint16_t selector) {
  ble_uuid128_t result{};
  result.u.type = BLE_UUID_TYPE_128;
  const auto bytes = wire::uuid(selector);
  std::memcpy(result.value, bytes.data(), bytes.size());
  return result;
}
const ble_uuid128_t uuids[] = {native_uuid(1), native_uuid(2), native_uuid(3),
                               native_uuid(4), native_uuid(5), native_uuid(6)};
enum Characteristic : unsigned { Info, Status, Live, Control, Response, Count };
char name_text[16]{};
char info_text[400]{};
char status_text[kMaxJson + 1] = "{}", live_text[kMaxJson + 1] = "{}";
std::size_t status_length = 2, live_length = 2;
portMUX_TYPE snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
ble_gatt_chr_def characteristics[Count + 1]{};
ble_gatt_svc_def services[2]{};
std::uint16_t value_handles[Count]{};
std::atomic<bool> subscribed[Count]{};
std::atomic<bool> enabled{false}, advertising{false}, connected{false},
    authenticated{false}, pairing_active{false};
std::atomic<std::uint16_t> conn_handle{BLE_HS_CONN_HANDLE_NONE}, mtu{0};
std::atomic<std::uint32_t> passkey{0}, bonds{0}, generation{0}, ui{0};
std::atomic<std::int64_t> pairing_deadline{0};
config::PairMode mode = config::PairMode::Random;
std::uint32_t fixed_passkey = 0;
// Only the host writes applied_advert. Publishers update one atomic word so
// flags/counter cannot tear, and the host event coalesces concurrent updates.
std::atomic<std::uint64_t> desired_advert{0}, applied_advert{~0ULL};
std::uint16_t boot16 = 0;
RequestHandler request_handler = nullptr;
TaskHandle_t host_task_handle = nullptr;
struct ImmediateError {
  Op op;
  Error code;
  const char *detail; // Only constant, secret-free protocol identifiers.
  std::uint32_t generation;
};
QueueHandle_t immediate_errors = nullptr;
SemaphoreHandle_t startup_done = nullptr, response_mutex = nullptr,
                  clear_mutex = nullptr, clear_done = nullptr,
                  response_done = nullptr;
ble_npl_event advert_event{}, clear_event{}, response_event{}, snapshot_event{};
struct ResponseAttempt {
  std::uint8_t frame[kMaxFrame]{};
  std::uint16_t length = 0, handle = 0;
  std::uint32_t session = 0, ticket = 0;
  std::int64_t deadline_us = 0;
};
ResponseAttempt response_attempt;
std::atomic<bool> response_pending{false};
std::atomic<std::uint32_t> completed_ticket{0};
std::atomic<int> response_result{BLE_HS_ENOTCONN};
std::uint32_t next_ticket = 0; // Protected by response_mutex.
std::atomic<unsigned> pending_snapshots{0};
std::atomic<bool> startup_ok{false}, host_requested{false};

void touch_ui() { ++ui; }

std::uint64_t pack_advert(const AdvertState &state) {
  return static_cast<std::uint64_t>(state.flags) |
         (static_cast<std::uint64_t>(state.finalized) << 8);
}

void update_bonds() {
  ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
  int count = 0;
  if (ble_store_util_bonded_peers(peers, &count, CONFIG_BT_NIMBLE_MAX_BONDS) ==
      0)
    bonds = static_cast<std::uint32_t>(count);
}

bool secure(std::uint16_t handle) {
  ble_gap_conn_desc desc{};
  return ble_gap_conn_find(handle, &desc) == 0 && desc.sec_state.encrypted &&
         (mode == config::PairMode::None ||
          (desc.sec_state.authenticated && desc.sec_state.key_size == 16));
}

bool apply_advert_data() {
  const std::uint64_t word = desired_advert.load();
  const auto data =
      wire::legacy_advert(static_cast<std::uint8_t>(word),
                          static_cast<std::uint32_t>(word >> 8), boot16);
  if (ble_gap_adv_set_data(data.data(), data.size()) != 0)
    return false;
  applied_advert = word;
  return true;
}

void refresh_advert(ble_npl_event *) {
  if (desired_advert.load() == applied_advert.load())
    return;
  if (!apply_advert_data()) {
    aqlog.println("BLE ERROR operation=advert-refresh");
    return;
  }
  touch_ui();
  const auto word = applied_advert.load();
  aqlog.printf("BLE ADV flags=0x%02x fin=%lu\n", unsigned(word & 0xffU),
               static_cast<unsigned long>(word >> 8));
}

int gap_event(ble_gap_event *event, void *);

bool start_advertising() {
  ble_gap_adv_params params{};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  params.itvl_min = 48; // 30 ms, matching the existing fast legacy interval.
  params.itvl_max = 96; // 60 ms.
  const int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, nullptr, BLE_HS_FOREVER,
                                   &params, gap_event, nullptr);
  advertising = rc == 0;
  touch_ui();
  if (rc != 0)
    aqlog.printf("BLE ERROR operation=advertise rc=%d\n", rc);
  return rc == 0;
}

// All notification submissions execute on the host task. Its GAP callbacks
// cannot change generation/authentication between the checks and submission,
// even when the controller reuses a connection handle after disconnection.
int notify_on_host(Characteristic which, const std::uint8_t *data,
                   std::size_t length, std::uint32_t session,
                   std::uint16_t handle) {
  if (!data || length == 0 || length > kMaxFrame || length > payload_max())
    return BLE_HS_EMSGSIZE;
  if (!connected.load() || !authenticated.load() || !subscribed[which].load() ||
      generation.load() != session || conn_handle.load() != handle)
    return BLE_HS_ENOTCONN;
  // Native custom notifications consume the mbuf on success AND failure.
  // Each retry must allocate a fresh buffer containing the same whole frame.
  os_mbuf *buffer = ble_hs_mbuf_from_flat(data, length);
  return buffer ? ble_gatts_notify_custom(handle, value_handles[which], buffer)
                : BLE_HS_ENOMEM;
}

void submit_response(ble_npl_event *) {
  const auto &attempt = response_attempt;
  const auto ticket = attempt.ticket;
  response_result =
      esp_timer_get_time() >= attempt.deadline_us
          ? BLE_HS_ETIMEOUT
          : notify_on_host(Response, attempt.frame, attempt.length,
                           attempt.session, attempt.handle);
  response_pending = false;
  completed_ticket = ticket;
  xSemaphoreGive(response_done);
}

bool notify_response(const std::uint8_t *data, std::size_t length,
                     std::uint32_t expected_generation, unsigned retries) {
  if (!data || !length || length > kMaxFrame || length > payload_max() ||
      response_pending.load())
    return false;
  const auto session =
      expected_generation ? expected_generation : generation.load();
  const auto handle = conn_handle.load();
  if (xTaskGetCurrentTaskHandle() == host_task_handle)
    return notify_on_host(Response, data, length, session, handle) == 0;
  std::memcpy(response_attempt.frame, data, length);
  response_attempt.length = static_cast<std::uint16_t>(length);
  response_attempt.session = session;
  response_attempt.handle = handle;
  const auto deadline = esp_timer_get_time() + 4LL * 1000000LL;
  response_attempt.deadline_us = deadline;
  for (unsigned attempt = 0; attempt < retries; ++attempt) {
    if (!connected.load() || !authenticated.load() ||
        !subscribed[Response].load() || generation.load() != session ||
        conn_handle.load() != handle || esp_timer_get_time() >= deadline)
      return false;
    xSemaphoreTake(response_done, 0);
    const auto ticket = ++next_ticket;
    response_attempt.ticket = ticket;
    response_pending = true;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &response_event);
    while (completed_ticket.load() != ticket) {
      const auto remaining = deadline - esp_timer_get_time();
      if (remaining <= 0 ||
          xSemaphoreTake(response_done,
                         pdMS_TO_TICKS((remaining + 999) / 1000)) != pdTRUE) {
        // Leave the static frame untouched until the pending host event has
        // finished. Future sends refuse it while response_pending is true.
        return false;
      }
    }
    const int rc = response_result.load();
    if (rc == 0)
      return true;
    if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY && rc != BLE_HS_EAGAIN)
      return false;
    if (attempt + 1 < retries)
      vTaskDelay(kNotifyRetryDelay);
  }
  return false;
}

void notify_snapshots(ble_npl_event *) {
  const unsigned pending = pending_snapshots.exchange(0);
  for (const auto which : {Status, Live}) {
    if (!(pending & (1U << which)))
      continue;
    char value[kMaxJson];
    portENTER_CRITICAL(&snapshot_lock);
    const auto length = which == Status ? status_length : live_length;
    std::memcpy(value, which == Status ? status_text : live_text, length);
    portEXIT_CRITICAL(&snapshot_lock);
    if (snapshot_notification_fits(length, mtu.load()))
      notify_on_host(which, reinterpret_cast<const std::uint8_t *>(value),
                     length, generation.load(), conn_handle.load());
  }
}

bool defer_error(Op op, Error code, const char *detail, std::uint32_t session) {
  const ImmediateError error{op, code, detail, session};
  return immediate_errors && xQueueSend(immediate_errors, &error, 0) == pdTRUE;
}

void error_task(void *) {
  ImmediateError error{};
  for (;;) {
    if (xQueueReceive(immediate_errors, &error, portMAX_DELAY) == pdTRUE)
      send_error(error.op, error.code, error.detail, error.generation);
  }
}

// Native NimBLE requires a mutable void* in the callback signature.
int access(std::uint16_t handle, std::uint16_t, ble_gatt_access_ctxt *ctxt,
           // cppcheck-suppress constParameterCallback
           void *arg) {
  const auto which =
      static_cast<Characteristic>(reinterpret_cast<std::uintptr_t>(arg));
  if (!secure(handle))
    return mode == config::PairMode::None ? BLE_ATT_ERR_INSUFFICIENT_ENC
                                          : BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    if (which == Info)
      return os_mbuf_append(ctxt->om, info_text, std::strlen(info_text)) == 0
                 ? 0
                 : BLE_ATT_ERR_INSUFFICIENT_RES;
    if (which == Status || which == Live) {
      // ATT performs Read Blob offset slicing after the callback. Append the
      // full cached snapshot, including values too large for notifications.
      char value[kMaxJson];
      portENTER_CRITICAL(&snapshot_lock);
      const auto length = which == Status ? status_length : live_length;
      std::memcpy(value, which == Status ? status_text : live_text, length);
      portEXIT_CRITICAL(&snapshot_lock);
      return os_mbuf_append(ctxt->om, value, length) == 0
                 ? 0
                 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return which == Response ? 0 : BLE_ATT_ERR_READ_NOT_PERMITTED;
  }
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || which != Control)
    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
  ControlRequest request;
  request.received_mono_us = esp_timer_get_time();
  request.conn_handle = handle;
  request.link_generation = generation.load();
  const auto length = OS_MBUF_PKTLEN(ctxt->om);
  if (length > kMaxControlBytes)
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  request.length = static_cast<std::uint16_t>(length);
  if (length == 0) {
    return defer_error(kOpList, kErrMalformed, "empty", request.link_generation)
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (os_mbuf_copydata(ctxt->om, 0, length, request.bytes) != 0)
    return BLE_ATT_ERR_UNLIKELY;
  aqlog.record_only().printf("BLE CMD op=0x%02x bytes=%u\n",
                             unsigned(request.bytes[0]),
                             unsigned(request.length));
  if (!request_handler(request) &&
      !defer_error(static_cast<Op>(request.bytes[0]), kErrBusy, "queue-full",
                   request.link_generation))
    return BLE_ATT_ERR_INSUFFICIENT_RES;
  return 0;
}

int gap_event(ble_gap_event *event, void *) {
  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT:
    if (event->connect.status != 0) {
      if (enabled.load() && host_requested.load())
        start_advertising();
      return 0;
    }
    if (!host_requested.load()) {
      ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
      return 0;
    }
    ++generation; // Invalidate previous worker replies before exposing handle.
    conn_handle = event->connect.conn_handle;
    mtu = ble_att_mtu(event->connect.conn_handle);
    advertising = false;
    authenticated = secure(event->connect.conn_handle);
    connected = true;
    std::fill(std::begin(subscribed), std::end(subscribed), false);
    touch_ui();
    aqlog.printf("BLE CONNECT mtu=%u\n", unsigned(mtu.load()));
    return 0;
  case BLE_GAP_EVENT_DISCONNECT:
    connected = false;
    authenticated = false;
    pairing_active = false;
    passkey = 0;
    conn_handle = BLE_HS_CONN_HANDLE_NONE;
    mtu = 0;
    ++generation;
    std::fill(std::begin(subscribed), std::end(subscribed), false);
    aqlog.printf("BLE DISCONNECT reason=%d\n", event->disconnect.reason);
    if (enabled.load() && host_requested.load())
      start_advertising();
    else {
      advertising = false;
      touch_ui();
    }
    return 0;
  case BLE_GAP_EVENT_MTU:
    if (event->mtu.conn_handle == conn_handle.load()) {
      mtu = event->mtu.value;
      touch_ui();
      aqlog.printf("BLE MTU mtu=%u\n", unsigned(event->mtu.value));
    }
    return 0;
  case BLE_GAP_EVENT_SUBSCRIBE:
    if (event->subscribe.conn_handle == conn_handle.load())
      for (unsigned i = 0; i < Count; ++i)
        if (event->subscribe.attr_handle == value_handles[i])
          subscribed[i] = event->subscribe.cur_notify != 0;
    return 0;
  case BLE_GAP_EVENT_ENC_CHANGE:
    if (event->enc_change.conn_handle != conn_handle.load())
      return 0;
    pairing_active = false;
    passkey = 0;
    authenticated =
        event->enc_change.status == 0 && secure(event->enc_change.conn_handle);
    update_bonds();
    touch_ui();
    if (!authenticated.load()) {
      aqlog.println("BLE AUTH result=failed action=disconnect");
      ble_gap_terminate(event->enc_change.conn_handle,
                        BLE_ERR_REM_USER_CONN_TERM);
    } else {
      aqlog.printf("BLE BONDED authenticated=true bonds=%lu\n",
                   static_cast<unsigned long>(bonds.load()));
    }
    return 0;
  case BLE_GAP_EVENT_PASSKEY_ACTION: {
    if (mode == config::PairMode::None ||
        event->passkey.params.action != BLE_SM_IOACT_DISP)
      return BLE_HS_ENOTSUP;
    ble_sm_io io{};
    io.action = BLE_SM_IOACT_DISP;
    io.passkey = mode == config::PairMode::Fixed ? fixed_passkey
                                                 : esp_random() % 1000000U;
    passkey = io.passkey;
    pairing_deadline = esp_timer_get_time() + kPairingWindowUs;
    pairing_active = true;
    touch_ui();
    if (mode == config::PairMode::Fixed)
      aqlog.println("BLE PAIR passkey=fixed");
    else
      aqlog.record_only().println("BLE PAIR passkey=random");
    return ble_sm_inject_io(event->passkey.conn_handle, &io);
  }
  case BLE_GAP_EVENT_REPEAT_PAIRING: {
    // A phone can delete its own bond; a new authenticated pairing may replace
    // that obsolete record without clearing bonds belonging to other phones.
    ble_gap_conn_desc desc{};
    if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0 &&
        ble_store_util_delete_peer(&desc.peer_id_addr) == 0)
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    return BLE_GAP_REPEAT_PAIRING_IGNORE;
  }
  case BLE_GAP_EVENT_ADV_COMPLETE:
    advertising = false;
    if (enabled.load() && !connected.load())
      start_advertising();
    return 0;
  default:
    return 0;
  }
}

void clear_bonds_on_host(ble_npl_event *) {
  const int rc = ble_store_clear();
  update_bonds();
  touch_ui();
  aqlog.printf("BLE BONDS CLEARED bonds=%lu rc=%d\n",
               static_cast<unsigned long>(bonds.load()), rc);
  xSemaphoreGive(clear_done);
}

void on_reset(int reason) {
  connected = false;
  authenticated = false;
  advertising = false;
  pairing_active = false;
  passkey = 0;
  conn_handle = BLE_HS_CONN_HANDLE_NONE;
  mtu = 0;
  ++generation;
  std::fill(std::begin(subscribed), std::end(subscribed), false);
  touch_ui();
  aqlog.printf("BLE RESET reason=%d\n", reason);
}

void on_sync() {
  if (!host_requested.load()) {
    startup_ok = false;
    xSemaphoreGive(startup_done);
    return;
  }
  // Deliberately require the factory public address, never infer a random/RPA
  // fallback: Android Companion Device Manager keys identity on this address.
  std::uint8_t public_address[6];
  bool ok = ble_hs_util_ensure_addr(0) == 0 &&
            ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, public_address, nullptr) == 0;
  wire::ScanResponse scan_response;
  ok = ok && wire::legacy_scan_response(name_text, scan_response) &&
       apply_advert_data() &&
       ble_gap_adv_rsp_set_data(scan_response.bytes.data(),
                                scan_response.length) == 0;
  update_bonds();
  if (ok) {
    enabled = true;
    ok = start_advertising();
  }
  // A late controller synchronization must not enable the service after
  // begin() has returned failure to the application.
  if (!host_requested.load()) {
    ble_gap_adv_stop();
    enabled = false;
    advertising = false;
    ok = false;
  }
  startup_ok = ok;
  if (!ok)
    aqlog.println("BLE ERROR operation=sync");
  xSemaphoreGive(startup_done);
}

void host_task(void *) {
  host_task_handle = xTaskGetCurrentTaskHandle();
  nimble_port_run();
  nimble_port_freertos_deinit();
}
} // namespace

bool begin(const Identity &identity, config::PairMode pairing_mode,
           std::uint32_t fixed_pin, RequestHandler handler) {
  if (!handler || startup_done || !identity.station || !identity.device ||
      !identity.boot || !identity.schema || !identity.dictionary_sha256 ||
      !identity.firmware)
    return false;
  request_handler = handler;
  mode = pairing_mode;
  fixed_passkey = fixed_pin % 1000000U;
  const auto device_length = std::strlen(identity.device);
  std::snprintf(name_text, sizeof(name_text), "AQ-%s",
                device_length >= 4 ? identity.device + device_length - 4
                                   : identity.device);
  // Leave enough legacy scan-response space for the complete service UUID.
  if (std::strlen(name_text) > 11)
    return false;
  const int info_length = std::snprintf(
      info_text, sizeof(info_text),
      "{\"proto\":%u,\"fw\":\"%s\",\"schema\":\"%s\",\"cols\":%u,"
      "\"dict\":\"%s\",\"station\":\"%s\",\"dev\":\"%s\","
      "\"boot\":\"%s\",\"max_read\":%lu}",
      unsigned(kProtocolVersion), identity.firmware, identity.schema,
      identity.columns, identity.dictionary_sha256, identity.station,
      identity.device, identity.boot, static_cast<unsigned long>(kMaxRead));
  if (info_length < 0 ||
      static_cast<std::size_t>(info_length) >= sizeof(info_text))
    return false;
  const auto boot_length = std::strlen(identity.boot);
  boot16 = static_cast<std::uint16_t>(std::strtoul(
      boot_length >= 4 ? identity.boot + boot_length - 4 : identity.boot,
      nullptr, 16));
  AdvertState initial;
  initial.flags = kAdvNoUtc;
  desired_advert = pack_advert(initial);
  startup_done = xSemaphoreCreateBinary();
  response_mutex = xSemaphoreCreateMutex();
  clear_mutex = xSemaphoreCreateMutex();
  clear_done = xSemaphoreCreateBinary();
  response_done = xSemaphoreCreateBinary();
  immediate_errors = xQueueCreate(8, sizeof(ImmediateError));
  if (!startup_done || !response_mutex || !clear_mutex || !clear_done ||
      !immediate_errors || !response_done)
    return false;
  // config::load() owns NVS initialization. Do not erase settings on failure.
  const auto rc = nimble_port_init();
  if (rc != ESP_OK) {
    aqlog.printf("BLE ERROR operation=init rc=%d\n", int(rc));
    return false;
  }
  ble_hs_cfg.reset_cb = on_reset;
  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr; // Evict oldest at 3.
  const bool mitm = mode != config::PairMode::None;
  ble_hs_cfg.sm_io_cap =
      mitm ? BLE_HS_IO_DISPLAY_ONLY : BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = mitm;
  ble_hs_cfg.sm_sc = 1;
  // Native SC-only also forces authenticated ATT access, which excludes
  // encrypted Just Works. Legacy pairing stays disabled in sdkconfig, so
  // `none` still negotiates Secure Connections without requiring MITM.
  ble_hs_cfg.sm_sc_only = mitm ? 1 : 0;
  ble_hs_cfg.sm_our_key_dist =
      BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist =
      BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_svc_gap_device_name_set(name_text);
  if (ble_att_set_preferred_mtu(kMaxMtu) != 0)
    return false;
  const auto read_flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                          (mitm ? BLE_GATT_CHR_F_READ_AUTHEN : 0);
  const auto write_flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                           (mitm ? BLE_GATT_CHR_F_WRITE_AUTHEN : 0);
  for (unsigned i = 0; i < Count; ++i) {
    auto &chr = characteristics[i];
    chr.uuid = &uuids[i + 1].u;
    chr.access_cb = access;
    chr.arg = reinterpret_cast<void *>(static_cast<std::uintptr_t>(i));
    chr.val_handle = &value_handles[i];
    chr.flags = i == Control ? write_flags : read_flags;
    if (i == Status || i == Live || i == Response)
      chr.flags |= BLE_GATT_CHR_F_NOTIFY;
  }
  services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
  services[0].uuid = &uuids[0].u;
  services[0].characteristics = characteristics;
  if (ble_gatts_count_cfg(services) != 0 || ble_gatts_add_svcs(services) != 0)
    return false;
  // Native IDF persists NimBLE keys via its NVS storage port. Retention of
  // existing Arduino NimBLE bonds is not qualified until board readback;
  // owner re-pairing may be required. Do not erase the namespace at startup.
  ble_store_config_init();
  ble_npl_event_init(&advert_event, refresh_advert, nullptr);
  ble_npl_event_init(&clear_event, clear_bonds_on_host, nullptr);
  ble_npl_event_init(&response_event, submit_response, nullptr);
  ble_npl_event_init(&snapshot_event, notify_snapshots, nullptr);
  if (xTaskCreate(error_task, "aq_ble_error", 3072, nullptr, 4, nullptr) !=
      pdPASS)
    return false;
  host_requested = true;
  nimble_port_freertos_init(host_task);
  if (xSemaphoreTake(startup_done, pdMS_TO_TICKS(5000)) != pdTRUE ||
      !startup_ok.load()) {
    host_requested = false;
    enabled = false;
    ble_gap_adv_stop();
    if (connected.load())
      ble_gap_terminate(conn_handle.load(), BLE_ERR_REM_USER_CONN_TERM);
    advertising = false;
    aqlog.println("BLE ERROR operation=host-start");
    return false;
  }
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
  if (!enabled.load() || !clear_mutex)
    return;
  if (xTaskGetCurrentTaskHandle() == host_task_handle) {
    clear_bonds_on_host(nullptr);
    return;
  }
  xSemaphoreTake(clear_mutex, portMAX_DELAY);
  // Drain a late completion before scheduling a new host operation.
  xSemaphoreTake(clear_done, 0);
  ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &clear_event);
  xSemaphoreTake(clear_done, portMAX_DELAY);
  xSemaphoreGive(clear_mutex);
}

void publish_snapshot(Characteristic which, const char *json,
                      std::size_t length) {
  if (!enabled.load() || !json || length > kMaxJson)
    return;
  portENTER_CRITICAL(&snapshot_lock);
  char *value = which == Status ? status_text : live_text;
  std::memcpy(value, json, length);
  value[length] = '\0';
  (which == Status ? status_length : live_length) = length;
  portEXIT_CRITICAL(&snapshot_lock);
  pending_snapshots.fetch_or(1U << which);
  ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &snapshot_event);
}

void publish_live(const char *json, std::size_t length) {
  publish_snapshot(Live, json, length);
}
void publish_status(const char *json, std::size_t length) {
  publish_snapshot(Status, json, length);
}

void publish_advert(const AdvertState &state) {
  if (!enabled.load())
    return;
  const auto next = pack_advert(state);
  if (desired_advert.exchange(next) != next || applied_advert.load() != next)
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &advert_event);
}

bool send_response(const std::uint8_t *frame, std::size_t length,
                   std::uint32_t expected_generation) {
  if (!enabled.load() || !response_mutex)
    return false;
  const bool on_host = xTaskGetCurrentTaskHandle() == host_task_handle;
  if (xSemaphoreTake(response_mutex, on_host ? 0 : pdMS_TO_TICKS(4000)) !=
      pdTRUE)
    return false;
  const bool ok = notify_response(frame, length, expected_generation,
                                  on_host ? 1 : kNotifyRetries);
  xSemaphoreGive(response_mutex);
  return ok;
}

bool send_error(Op op, Error code, const char *detail,
                std::uint32_t expected_generation) {
  std::uint8_t frame[3 + 64] = {kFrameError, static_cast<std::uint8_t>(op),
                                static_cast<std::uint8_t>(code)};
  std::size_t length = 3;
  if (detail) {
    const auto detail_length = std::strlen(detail);
    const auto copy = detail_length > 64 ? 64 : detail_length;
    std::memcpy(frame + 3, detail, copy);
    length += copy;
  }
  aqlog.printf("BLE ERROR op=0x%02x code=%u\n", unsigned(op), unsigned(code));
  return send_response(frame, length, expected_generation);
}

std::uint16_t payload_max() { return wire::response_capacity(mtu.load()); }
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
  state.advert_flags = static_cast<std::uint8_t>(applied_advert.load() & 0xffU);
  return state;
}
std::uint32_t ui_generation() { return ui.load(); }
} // namespace ble
