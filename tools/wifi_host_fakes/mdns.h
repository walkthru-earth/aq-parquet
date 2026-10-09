#pragma once
#include "esp_err.h"
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
struct mdns_txt_item_t {
  const char *key;
  const char *value;
};
inline bool fake_mdns_started = false;
inline std::string fake_mdns_host, fake_mdns_instance, fake_mdns_service,
    fake_mdns_transport;
inline std::uint16_t fake_mdns_port = 0;
inline std::map<std::string, std::string> fake_mdns_txt;
inline esp_err_t mdns_init() {
  fake_mdns_started = true;
  return ESP_OK;
}
inline esp_err_t mdns_hostname_set(const char *host) {
  fake_mdns_host = host;
  return ESP_OK;
}
inline esp_err_t mdns_instance_name_set(const char *name) {
  fake_mdns_instance = name;
  return ESP_OK;
}
inline esp_err_t mdns_service_add(const char *, const char *service,
                                  const char *transport, std::uint16_t port,
                                  mdns_txt_item_t *txt, std::size_t length) {
  fake_mdns_service = service;
  fake_mdns_transport = transport;
  fake_mdns_port = port;
  fake_mdns_txt.clear();
  for (std::size_t i = 0; i < length; ++i)
    fake_mdns_txt[txt[i].key] = txt[i].value;
  return ESP_OK;
}
inline void mdns_free() { fake_mdns_started = false; }
