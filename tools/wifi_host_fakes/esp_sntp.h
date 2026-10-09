#pragma once
#include "lwip/ip_addr.h"
#include <cstdint>
inline ip_addr_t fake_ntp_addresses[3];
inline const char *fake_ntp_names[3]{};
inline bool fake_dhcp_ntp = false;
inline std::uint32_t fake_sync_interval_ms = 0;
inline void esp_sntp_set_sync_interval(std::uint32_t interval) {
  fake_sync_interval_ms = interval;
}
inline void esp_sntp_servermode_dhcp(bool enabled) { fake_dhcp_ntp = enabled; }
inline void esp_sntp_setserver(unsigned index, const ip_addr_t *address) {
  fake_ntp_addresses[index] = address ? *address : ip_addr_t{};
  fake_ntp_names[index] = nullptr;
}
inline const ip_addr_t *esp_sntp_getserver(unsigned index) {
  return &fake_ntp_addresses[index];
}
inline const char *esp_sntp_getservername(unsigned index) {
  return fake_ntp_names[index];
}
inline void fake_dhcp_ntp_offer(const char *literal, unsigned index = 0) {
  if (!fake_dhcp_ntp)
    return;
  ip_addr_t address{};
  if (ipaddr_aton(literal, &address))
    esp_sntp_setserver(index, &address);
}
