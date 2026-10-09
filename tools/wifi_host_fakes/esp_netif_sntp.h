#pragma once
#include "esp_err.h"
#include "esp_sntp.h"
#include <string>
#include <sys/time.h>
#include <vector>
struct esp_sntp_config_t {
  bool wait_for_sync = true;
  void (*sync_cb)(struct timeval *) = nullptr;
  unsigned num_of_servers = 0;
  const char *servers[3]{};
};
#define ESP_SNTP_SERVER_LIST(...) {__VA_ARGS__}
#define ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(count, list)                    \
  {true, nullptr, count, list}
inline void (*fake_time_callback)(struct timeval *) = nullptr;
inline unsigned fake_ntp_starts = 0, fake_ntp_stops = 0;
inline esp_err_t fake_ntp_init_result = ESP_OK;
inline std::vector<std::string> fake_ntp_candidates;
inline esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config) {
  ++fake_ntp_starts;
  if (fake_ntp_init_result != ESP_OK)
    return fake_ntp_init_result;
  fake_time_callback = config->sync_cb;
  for (unsigned i = 0; i < config->num_of_servers; ++i) {
    fake_ntp_names[i] = config->servers[i];
    // Native receives numeric literals: no unsafe in-flight SDK DNS query.
    if (!ipaddr_aton(config->servers[i], &fake_ntp_addresses[i]))
      return ESP_FAIL;
  }
  fake_ntp_candidates.emplace_back(config->servers[0]);
  return ESP_OK;
}
inline void esp_netif_sntp_deinit() {
  ++fake_ntp_stops;
  fake_time_callback = nullptr;
}
