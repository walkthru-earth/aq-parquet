#pragma once
#include "esp_err.h"
#include <cstdint>
using esp_event_base_t = const char *;
using esp_event_handler_instance_t = void *;
using esp_event_handler_t = void (*)(void *, esp_event_base_t, std::int32_t,
                                     void *);
inline const char wifi_event_name[] = "WIFI_EVENT",
                  ip_event_name[] = "IP_EVENT";
inline esp_event_base_t WIFI_EVENT = wifi_event_name, IP_EVENT = ip_event_name;
constexpr std::int32_t ESP_EVENT_ANY_ID = -1;
constexpr std::int32_t WIFI_EVENT_STA_DISCONNECTED = 1, WIFI_EVENT_STA_STOP = 2;
constexpr std::int32_t IP_EVENT_STA_GOT_IP = 1, IP_EVENT_STA_LOST_IP = 2;
inline esp_event_handler_t fake_station_events = nullptr,
                           fake_ip_events = nullptr;
inline esp_err_t esp_event_loop_create_default() { return ESP_OK; }
inline esp_err_t
esp_event_handler_instance_register(esp_event_base_t base, std::int32_t,
                                    esp_event_handler_t handler, void *,
                                    esp_event_handler_instance_t *instance) {
  (base == WIFI_EVENT ? fake_station_events : fake_ip_events) = handler;
  *instance = reinterpret_cast<void *>(1);
  return ESP_OK;
}
inline esp_err_t
esp_event_handler_instance_unregister(esp_event_base_t base, std::int32_t,
                                      esp_event_handler_instance_t) {
  (base == WIFI_EVENT ? fake_station_events : fake_ip_events) = nullptr;
  return ESP_OK;
}
