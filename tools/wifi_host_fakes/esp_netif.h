#pragma once
#include "esp_event.h"
#include <cstdio>
struct esp_netif_t {};
struct esp_ip4_addr_t {
  unsigned addr = 0;
};
struct esp_netif_ip_info_t {
  esp_ip4_addr_t ip;
};
inline esp_netif_t fake_station_netif;
inline esp_err_t esp_netif_init() { return ESP_OK; }
inline esp_netif_t *esp_netif_create_default_wifi_sta() {
  return &fake_station_netif;
}
inline esp_err_t esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *) {
  return ESP_OK;
}
inline char *esp_ip4addr_ntoa(const esp_ip4_addr_t *, char *out, int length) {
  std::snprintf(out, length, "127.0.0.1");
  return out;
}
inline esp_err_t esp_netif_set_hostname(esp_netif_t *, const char *) {
  return ESP_OK;
}
inline void esp_netif_destroy_default_wifi(esp_netif_t *) {}
