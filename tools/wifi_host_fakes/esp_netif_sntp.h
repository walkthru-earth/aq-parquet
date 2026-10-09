#pragma once
#include "esp_err.h"
#include <sys/time.h>
struct esp_sntp_config_t {
  bool wait_for_sync = true;
  void (*sync_cb)(struct timeval *) = nullptr;
  unsigned num_of_servers = 0;
  const char *servers[2]{};
};
#define ESP_SNTP_SERVER_LIST(...) {__VA_ARGS__}
#define ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(count, list)                    \
  {true, nullptr, count, list}
inline void (*fake_time_callback)(struct timeval *) = nullptr;
inline unsigned fake_ntp_starts = 0, fake_ntp_stops = 0;
inline esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config) {
  fake_time_callback = config->sync_cb;
  ++fake_ntp_starts;
  return ESP_OK;
}
inline void esp_netif_sntp_deinit() { ++fake_ntp_stops; }
