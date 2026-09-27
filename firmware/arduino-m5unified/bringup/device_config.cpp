#include "device_config.h"
#include "debug_log.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>

#include <cstdio>
#include <cstring>

namespace config {
namespace {
constexpr const char *kNamespace = "aqcfg";
portMUX_TYPE settings_mutex = portMUX_INITIALIZER_UNLOCKED;
Settings current;
Settings booted; // pair/pin the running BLE stack was started with
bool persistent = false;

std::uint32_t random_pin() { return esp_random() % 1000000U; }

bool save_locked(const Settings &settings) {
  Preferences store;
  if (!store.begin(kNamespace, false))
    return false;
  bool ok = store.putUChar("pair", static_cast<std::uint8_t>(settings.pair));
  ok = store.putUInt("pin", settings.pin) && ok;
  ok = store.putUChar("disp", settings.display ? 1 : 0) && ok;
  ok = store.putUChar("wifion", settings.wifi_on ? 1 : 0) && ok;
  ok = store.putString("ssid", settings.ssid) >= 0 && ok;
  ok = store.putString("psk", settings.psk) >= 0 && ok;
  ok = store.putUChar("lanon", settings.lan_on ? 1 : 0) && ok;
  ok =
      store.putBytes("token", settings.token, kTokenBytes) == kTokenBytes && ok;
  store.end();
  return ok;
}

void publish(const Settings &settings) {
  portENTER_CRITICAL(&settings_mutex);
  current = settings;
  portEXIT_CRITICAL(&settings_mutex);
}

bool parse_bool(const char *value, bool &out) {
  if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0) {
    out = false;
    return true;
  }
  if (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0) {
    out = true;
    return true;
  }
  return false;
}

bool parse_pin(const char *value, std::uint32_t &out) {
  if (std::strlen(value) != 6)
    return false;
  std::uint32_t pin = 0;
  for (const char *p = value; *p; ++p) {
    if (*p < '0' || *p > '9')
      return false;
    pin = pin * 10 + static_cast<std::uint32_t>(*p - '0');
  }
  out = pin;
  return true;
}

bool printable(const char *value) {
  for (const char *p = value; *p; ++p)
    if (static_cast<unsigned char>(*p) < 32 || *p == 127)
      return false;
  return true;
}

std::size_t json_escape(char *out, std::size_t size, const char *text) {
  std::size_t used = 0;
  for (const char *p = text; *p && used + 7 < size; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c == '"' || c == '\\') {
      out[used++] = '\\';
      out[used++] = static_cast<char>(c);
    } else if (c < 32) {
      used += static_cast<std::size_t>(
          std::snprintf(out + used, size - used, "\\u%04x", c));
    } else {
      out[used++] = static_cast<char>(c);
    }
  }
  out[used] = '\0';
  return used;
}
} // namespace

const char *pair_name(PairMode mode) {
  switch (mode) {
  case PairMode::Fixed:
    return "fixed";
  case PairMode::None:
    return "none";
  default:
    return "random";
  }
}

bool load(bool display_detected) {
  Settings settings;
  Preferences store;
  if (!store.begin(kNamespace, false)) {
    aqlog.println("CONFIG ERROR operation=nvs-open persistent=false");
    settings.display = display_detected;
    settings.pair = display_detected ? PairMode::Random : PairMode::Fixed;
    settings.pin = random_pin();
    esp_fill_random(settings.token, kTokenBytes);
    publish(settings);
    booted = settings;
    return false;
  }
  const bool first_boot = !store.isKey("pair");
  bool migrated_pin = false;
  if (first_boot) {
    settings.display = display_detected;
    settings.pair = display_detected ? PairMode::Random : PairMode::Fixed;
    settings.pin = random_pin();
    esp_fill_random(settings.token, kTokenBytes);
  } else {
    const auto pair = store.getUChar("pair", 0);
    settings.pair = pair > 2 ? PairMode::Random : static_cast<PairMode>(pair);
    settings.pin = store.getUInt("pin", kLegacyDefaultPin) % 1000000U;
    if (settings.pin == kLegacyDefaultPin) {
      settings.pin = random_pin();
      migrated_pin = true;
      aqlog.println("CONFIG SECURITY legacy_pin_rotated=true");
    }
    settings.display = store.getUChar("disp", display_detected ? 1 : 0) != 0;
    settings.wifi_on = store.getUChar("wifion", 0) != 0;
    store.getString("ssid", settings.ssid, sizeof(settings.ssid));
    store.getString("psk", settings.psk, sizeof(settings.psk));
    settings.lan_on = store.getUChar("lanon", 1) != 0;
    if (store.getBytes("token", settings.token, kTokenBytes) != kTokenBytes)
      esp_fill_random(settings.token, kTokenBytes);
  }
  store.end();
  persistent = true;
  if ((first_boot || migrated_pin) && !save_locked(settings))
    aqlog.println("CONFIG ERROR operation=nvs-seed-or-migrate");
  publish(settings);
  booted = settings;
  aqlog.printf("CONFIG LOADED first_boot=%s display=%s pair=%s pin_default=%s "
               "wifi_on=%s ssid_set=%s lan_on=%s\n",
               first_boot ? "true" : "false",
               settings.display ? "true" : "false", pair_name(settings.pair),
               settings.pin == kLegacyDefaultPin ? "true" : "false",
               settings.wifi_on ? "true" : "false",
               settings.ssid[0] ? "true" : "false",
               settings.lan_on ? "true" : "false");
  return true;
}

Settings get() {
  Settings copy;
  portENTER_CRITICAL(&settings_mutex);
  copy = current;
  portEXIT_CRITICAL(&settings_mutex);
  return copy;
}

bool reboot_required() {
  const Settings now = get();
  return now.pair != booted.pair || now.pin != booted.pin;
}

bool apply_lines(const char *text, std::size_t length, char *bad_key,
                 std::size_t bad_key_size, Actions &actions) {
  Settings next = get();
  Actions pending;
  bad_key[0] = '\0';
  std::size_t position = 0;
  bool any = false;
  while (position < length) {
    // One line: key '=' value, terminated by '\n' or end of text.
    std::size_t end = position;
    while (end < length && text[end] != '\n')
      ++end;
    std::size_t line_length = end - position;
    if (line_length && text[position + line_length - 1] == '\r')
      --line_length;
    if (line_length == 0) {
      position = end + 1;
      continue;
    }
    char line[128];
    if (line_length >= sizeof(line)) {
      std::snprintf(bad_key, bad_key_size, "line-too-long");
      return false;
    }
    std::memcpy(line, text + position, line_length);
    line[line_length] = '\0';
    position = end + 1;
    char *equals = std::strchr(line, '=');
    if (!equals) {
      std::snprintf(bad_key, bad_key_size, "%s", line);
      return false;
    }
    *equals = '\0';
    const char *key = line;
    const char *value = equals + 1;
    auto reject = [&]() {
      std::snprintf(bad_key, bad_key_size, "%s", key);
      return false;
    };
    any = true;
    if (std::strcmp(key, "ble.pair") == 0) {
      PairMode mode;
      if (std::strcmp(value, "random") == 0)
        mode = PairMode::Random;
      else if (std::strcmp(value, "fixed") == 0)
        mode = PairMode::Fixed;
      else if (std::strcmp(value, "none") == 0)
        mode = PairMode::None;
      else
        return reject();
      pending.ble_changed = pending.ble_changed || mode != next.pair;
      next.pair = mode;
    } else if (std::strcmp(key, "ble.pin") == 0) {
      std::uint32_t pin;
      if (!parse_pin(value, pin))
        return reject();
      pending.ble_changed = pending.ble_changed || pin != next.pin;
      next.pin = pin;
    } else if (std::strcmp(key, "ble.clear_bonds") == 0) {
      bool flag;
      if (!parse_bool(value, flag))
        return reject();
      pending.clear_bonds = flag;
    } else if (std::strcmp(key, "wifi.on") == 0) {
      bool flag;
      if (!parse_bool(value, flag))
        return reject();
      pending.wifi_changed = pending.wifi_changed || flag != next.wifi_on;
      next.wifi_on = flag;
    } else if (std::strcmp(key, "wifi.ssid") == 0) {
      if (std::strlen(value) > kSsidMax || !printable(value))
        return reject();
      pending.wifi_changed =
          pending.wifi_changed || std::strcmp(value, next.ssid) != 0;
      std::snprintf(next.ssid, sizeof(next.ssid), "%s", value);
    } else if (std::strcmp(key, "wifi.psk") == 0) {
      const std::size_t psk_length = std::strlen(value);
      if (psk_length > kPskMax || (psk_length && psk_length < 8) ||
          !printable(value))
        return reject();
      pending.wifi_changed =
          pending.wifi_changed || std::strcmp(value, next.psk) != 0;
      std::snprintf(next.psk, sizeof(next.psk), "%s", value);
    } else if (std::strcmp(key, "lan.on") == 0) {
      bool flag;
      if (!parse_bool(value, flag))
        return reject();
      pending.wifi_changed = pending.wifi_changed || flag != next.lan_on;
      next.lan_on = flag;
    } else if (std::strcmp(key, "lan.rotate_token") == 0) {
      bool flag;
      if (!parse_bool(value, flag))
        return reject();
      pending.rotate_token = flag;
    } else {
      return reject();
    }
  }
  if (!any) {
    std::snprintf(bad_key, bad_key_size, "empty");
    return false;
  }
  if (next.wifi_on && !next.ssid[0]) {
    std::snprintf(bad_key, bad_key_size, "wifi.ssid");
    return false;
  }
  if (pending.rotate_token)
    esp_fill_random(next.token, kTokenBytes);
  if (persistent && !save_locked(next)) {
    std::snprintf(bad_key, bad_key_size, "nvs");
    return false;
  }
  publish(next);
  actions = pending;
  aqlog.printf(
      "CONFIG SET pair=%s pin_default=%s wifi_on=%s ssid_set=%s "
      "lan_on=%s clear_bonds=%s rotate_token=%s reboot_required=%s\n",
      pair_name(next.pair), next.pin == kLegacyDefaultPin ? "true" : "false",
      next.wifi_on ? "true" : "false", next.ssid[0] ? "true" : "false",
      next.lan_on ? "true" : "false", pending.clear_bonds ? "true" : "false",
      pending.rotate_token ? "true" : "false",
      reboot_required() ? "true" : "false");
  return true;
}

bool rotate_token() {
  Settings next = get();
  esp_fill_random(next.token, kTokenBytes);
  if (persistent && !save_locked(next))
    return false;
  publish(next);
  return true;
}

std::size_t build_json(char *out, std::size_t size, const WifiView &wifi) {
  const Settings settings = get();
  char ssid[2 * kSsidMax + 8];
  json_escape(ssid, sizeof(ssid), settings.ssid);
  const int written = std::snprintf(
      out, size,
      "{\"ble\":{\"pair\":\"%s\",\"pin_set\":%u,\"pin_default\":%u,\"bonds\":%"
      "u,"
      "\"display\":%u},\"wifi\":{\"on\":%u,\"ssid\":\"%s\",\"psk_set\":%u,"
      "\"state\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"mac\":\"%s\"},"
      "\"lan\":{\"on\":%u,\"port\":%u,\"host\":\"%s\",\"clients\":%u}}",
      pair_name(settings.pair), settings.pin <= 999999U ? 1U : 0U,
      settings.pin == kLegacyDefaultPin ? 1U : 0U, wifi.bonds,
      settings.display ? 1U : 0U, settings.wifi_on ? 1U : 0U, ssid,
      settings.psk[0] ? 1U : 0U, wifi.state, wifi.ip, wifi.rssi, wifi.mac,
      settings.lan_on ? 1U : 0U, unsigned(kLanPort), wifi.host, wifi.clients);
  return written > 0 && std::size_t(written) < size ? std::size_t(written) : 0;
}
} // namespace config
