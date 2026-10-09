#pragma once
#include "esp_netif.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
enum wifi_auth_mode_t {
  WIFI_AUTH_OPEN,
  WIFI_AUTH_WEP,
  WIFI_AUTH_WPA_PSK,
  WIFI_AUTH_WPA2_PSK,
  WIFI_AUTH_WPA_WPA2_PSK,
  WIFI_AUTH_WPA2_ENTERPRISE,
  WIFI_AUTH_WPA3_PSK,
  WIFI_AUTH_WPA2_WPA3_PSK
};
struct wifi_event_sta_disconnected_t {
  std::uint8_t reason = 201;
};
struct wifi_init_config_t {};
#define WIFI_INIT_CONFIG_DEFAULT()                                             \
  {                                                                            \
  }
constexpr int WIFI_STORAGE_RAM = 1, WIFI_MODE_STA = 1, WIFI_IF_STA = 0,
              WIFI_PS_MIN_MODEM = 1;
struct wifi_config_t {
  struct {
    std::uint8_t ssid[32]{};
    std::uint8_t password[64]{};
    struct {
      wifi_auth_mode_t authmode = WIFI_AUTH_OPEN;
    } threshold;
  } sta;
};
struct wifi_ap_record_t {
  std::uint8_t ssid[33]{};
  std::int8_t rssi = -40;
  wifi_auth_mode_t authmode = WIFI_AUTH_WPA2_PSK;
  std::uint8_t primary = 1;
};
struct wifi_scan_config_t {
  bool show_hidden = false;
};
inline bool fake_wifi_started = false, fake_wifi_autoconnect = true;
inline int fake_wifi_storage = -1, fake_wifi_power_save = -1;
inline unsigned fake_wifi_connects = 0, fake_scan_clears = 0;
inline esp_err_t fake_scan_result = ESP_OK, fake_start_result = ESP_OK;
inline wifi_config_t fake_wifi_config;
inline std::vector<wifi_ap_record_t> fake_scan_records;
inline void fake_wifi_got_ip() {
  if (fake_ip_events)
    fake_ip_events(nullptr, IP_EVENT, IP_EVENT_STA_GOT_IP, nullptr);
}
inline void fake_wifi_lost() {
  wifi_event_sta_disconnected_t event;
  if (fake_station_events)
    fake_station_events(nullptr, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                        &event);
}
inline esp_err_t fake_init_result = ESP_OK;
inline unsigned fake_wifi_deinits = 0;
inline esp_err_t esp_wifi_init(const wifi_init_config_t *) {
  return fake_init_result;
}
inline esp_err_t esp_wifi_deinit() {
  ++fake_wifi_deinits;
  return ESP_OK;
}
inline esp_err_t esp_wifi_set_storage(int storage) {
  fake_wifi_storage = storage;
  return ESP_OK;
}
inline esp_err_t esp_wifi_set_mode(int) { return ESP_OK; }
inline esp_err_t esp_wifi_set_config(int, const wifi_config_t *config) {
  fake_wifi_config = *config;
  return ESP_OK;
}
inline esp_err_t esp_wifi_start() {
  fake_wifi_started = fake_start_result == ESP_OK;
  return fake_start_result;
}
inline esp_err_t esp_wifi_stop() {
  fake_wifi_started = false;
  if (fake_station_events)
    fake_station_events(nullptr, WIFI_EVENT, WIFI_EVENT_STA_STOP, nullptr);
  return ESP_OK;
}
inline esp_err_t esp_wifi_connect() {
  ++fake_wifi_connects;
  if (fake_wifi_autoconnect)
    fake_wifi_got_ip();
  return ESP_OK;
}
inline esp_err_t esp_wifi_disconnect() {
  fake_wifi_lost();
  return ESP_OK;
}
inline esp_err_t esp_wifi_set_ps(int mode) {
  fake_wifi_power_save = mode;
  return ESP_OK;
}
inline esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *record) {
  *record = {};
  return ESP_OK;
}
inline esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *, bool) {
  return fake_scan_result;
}
inline esp_err_t esp_wifi_scan_get_ap_records(std::uint16_t *count,
                                              wifi_ap_record_t *records) {
  *count = std::min<std::size_t>(*count, fake_scan_records.size());
  std::copy_n(fake_scan_records.begin(), *count, records);
  fake_scan_records.clear();
  return ESP_OK;
}
inline esp_err_t esp_wifi_clear_ap_list() {
  fake_scan_records.clear();
  ++fake_scan_clears;
  return ESP_OK;
}
