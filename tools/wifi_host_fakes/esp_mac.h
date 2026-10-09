#pragma once
#include "esp_err.h"
#include <cstdint>
#include <cstring>
constexpr int ESP_MAC_WIFI_STA = 0;
inline esp_err_t esp_read_mac(std::uint8_t *mac, int) {
  const std::uint8_t bytes[] = {0, 0, 0, 0, 0, 1};
  std::memcpy(mac, bytes, sizeof(bytes));
  return ESP_OK;
}
