#include "device_config.h"
#include "debug_log.h"

#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <nvs.h>

#include <cstdio>
#include <cstring>

namespace config {
namespace {
constexpr const char *kNamespace = "aqcfg";
portMUX_TYPE settings_mutex = portMUX_INITIALIZER_UNLOCKED;
Settings current;
Settings booted; // pair/pin the running BLE stack was started with
bool persistent = false;
char hardware_vendor[16]{};
char hardware_model[16]{};

std::uint32_t random_pin() { return esp_random() % 1000000U; }

bool committed(nvs_handle_t store, esp_err_t result) {
  return result == ESP_OK && nvs_commit(store) == ESP_OK;
}

bool save_string(nvs_handle_t store, const char *key, const char *value) {
  if (!committed(store, nvs_set_str(store, key, value)))
    return false;
  char readback[96]{};
  std::size_t length = sizeof(readback);
  return nvs_get_str(store, key, readback, &length) == ESP_OK &&
         std::strcmp(readback, value) == 0;
}

std::uint8_t read_u8(nvs_handle_t store, const char *key,
                     std::uint8_t fallback) {
  std::uint8_t value;
  return nvs_get_u8(store, key, &value) == ESP_OK ? value : fallback;
}

std::uint32_t read_u32(nvs_handle_t store, const char *key,
                       std::uint32_t fallback) {
  std::uint32_t value;
  return nvs_get_u32(store, key, &value) == ESP_OK ? value : fallback;
}

bool read_string(nvs_handle_t store, const char *key, char *out,
                 std::size_t size) {
  return nvs_get_str(store, key, out, &size) == ESP_OK;
}

bool sensor_serial_chars(const char *value) {
  if (std::strlen(value) > kSensorSerialMax)
    return false;
  for (const char *p = value; *p; ++p)
    if (!(*p >= 'A' && *p <= 'Z') && !(*p >= '0' && *p <= '9') && *p != '-')
      return false;
  return true;
}

bool valid_pms5003t_serial(const char *serial) {
  const auto length = std::strlen(serial);
  if (length < 18 || length > 26 || std::strncmp(serial, "PMS5003T-", 9) != 0)
    return false;
  for (const char *p = serial + 9; *p; ++p)
    if (*p < '0' || *p > '9')
      return false;
  const int year = (serial[9] - '0') * 1000 + (serial[10] - '0') * 100 +
                   (serial[11] - '0') * 10 + serial[12] - '0';
  const int month = (serial[13] - '0') * 10 + serial[14] - '0';
  const int day = (serial[15] - '0') * 10 + serial[16] - '0';
  if (year < 2000 || month < 1 || month > 12)
    return false;
  constexpr int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  return day >= 1 && day <= days[month] + (month == 2 && leap ? 1 : 0);
}

bool exact_batch_candidate(const Settings &settings) {
  if (std::strcmp(settings.sensor_vendor, "Plantower") != 0 ||
      std::strcmp(settings.sensor_model, "PMS5003T") != 0 ||
      !valid_pms5003t_serial(settings.sensor_serial) ||
      std::strncmp(settings.sensor_serial, "PMS5003T-20260408", 17) != 0)
    return false;
  return true;
}

// Sensor identity and opt-in share one NVS value so a failed multi-key save
// cannot pair an old opt-in with a replacement unit's new serial on reboot.
void encode_sensor(char *out, std::size_t size, const Settings &settings) {
  std::snprintf(out, size, "%s|%s|%s|%u", settings.sensor_vendor,
                settings.sensor_model, settings.sensor_serial,
                settings.sensor_batch_candidate ? 1U : 0U);
}

bool decode_sensor(char *value, Settings &settings) {
  char *vendor = value;
  char *first = std::strchr(vendor, '|');
  if (!first)
    return false;
  *first++ = '\0';
  char *model = first;
  char *second = std::strchr(model, '|');
  if (!second)
    return false;
  *second++ = '\0';
  char *serial = second;
  char *third = std::strchr(serial, '|');
  if (!third)
    return false;
  *third++ = '\0';
  if (std::strchr(third, '|') ||
      std::strlen(vendor) >= sizeof(settings.sensor_vendor) ||
      std::strlen(model) >= sizeof(settings.sensor_model) ||
      !sensor_serial_chars(serial) ||
      (*serial && !valid_pms5003t_serial(serial)) ||
      (std::strcmp(third, "0") != 0 && std::strcmp(third, "1") != 0))
    return false;
  if (*vendor && (std::strcmp(vendor, hardware_vendor) != 0 ||
                  std::strcmp(model, hardware_model) != 0))
    return false;
  if ((*vendor == '\0') != (*model == '\0') || (*serial && !*vendor))
    return false;
  std::memcpy(settings.sensor_vendor, vendor, std::strlen(vendor) + 1);
  std::memcpy(settings.sensor_model, model, std::strlen(model) + 1);
  std::memcpy(settings.sensor_serial, serial, std::strlen(serial) + 1);
  settings.sensor_batch_candidate = std::strcmp(third, "1") == 0;
  if (settings.sensor_batch_candidate && !exact_batch_candidate(settings))
    return false;
  return true;
}

bool save_locked(const Settings &settings) {
  nvs_handle_t store;
  if (nvs_open(kNamespace, NVS_READWRITE, &store) != ESP_OK)
    return false;
  // Preserve the existing per-key persistence contract and NVS value types.
  bool ok =
      committed(store, nvs_set_u8(store, "pair",
                                  static_cast<std::uint8_t>(settings.pair)));
  ok = committed(store, nvs_set_u32(store, "pin", settings.pin)) && ok;
  ok = committed(store, nvs_set_u8(store, "disp", settings.display ? 1 : 0)) &&
       ok;
  ok =
      committed(store, nvs_set_u8(store, "wifion", settings.wifi_on ? 1 : 0)) &&
      ok;
  ok = save_string(store, "ssid", settings.ssid) && ok;
  ok = save_string(store, "psk", settings.psk) && ok;
  ok = committed(store, nvs_set_u8(store, "lanon", settings.lan_on ? 1 : 0)) &&
       ok;
  ok = committed(store,
                 nvs_set_blob(store, "token", settings.token, kTokenBytes)) &&
       ok;
  char sensor[80];
  encode_sensor(sensor, sizeof(sensor), settings);
  ok = ok && save_string(store, "sensor", sensor);
  nvs_close(store);
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

void set_sensor_hardware(const char *vendor, const char *model) {
  std::snprintf(hardware_vendor, sizeof(hardware_vendor), "%s",
                vendor ? vendor : "");
  std::snprintf(hardware_model, sizeof(hardware_model), "%s",
                model ? model : "");
}

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
  persistent = false;
  nvs_handle_t store;
  if (nvs_open(kNamespace, NVS_READWRITE, &store) != ESP_OK) {
    aqlog.println("CONFIG ERROR operation=nvs-open persistent=false");
    settings.display = display_detected;
    settings.pair = display_detected ? PairMode::Random : PairMode::Fixed;
    settings.pin = random_pin();
    esp_fill_random(settings.token, kTokenBytes);
    publish(settings);
    booted = settings;
    return false;
  }
  std::uint8_t stored_pair;
  const bool first_boot =
      nvs_get_u8(store, "pair", &stored_pair) == ESP_ERR_NVS_NOT_FOUND;
  bool migrated_pin = false;
  if (first_boot) {
    settings.display = display_detected;
    settings.pair = display_detected ? PairMode::Random : PairMode::Fixed;
    settings.pin = random_pin();
    esp_fill_random(settings.token, kTokenBytes);
  } else {
    const auto pair = read_u8(store, "pair", 0);
    settings.pair = pair > 2 ? PairMode::Random : static_cast<PairMode>(pair);
    settings.pin = read_u32(store, "pin", kLegacyDefaultPin) % 1000000U;
    if (settings.pin == kLegacyDefaultPin) {
      settings.pin = random_pin();
      migrated_pin = true;
      aqlog.println("CONFIG SECURITY legacy_pin_rotated=true");
    }
    settings.display = read_u8(store, "disp", display_detected ? 1 : 0) != 0;
    settings.wifi_on = read_u8(store, "wifion", 0) != 0;
    read_string(store, "ssid", settings.ssid, sizeof(settings.ssid));
    read_string(store, "psk", settings.psk, sizeof(settings.psk));
    settings.lan_on = read_u8(store, "lanon", 1) != 0;
    std::size_t token_size = kTokenBytes;
    if (nvs_get_blob(store, "token", settings.token, &token_size) != ESP_OK ||
        token_size != kTokenBytes)
      esp_fill_random(settings.token, kTokenBytes);
    char sensor[80]{};
    if (read_string(store, "sensor", sensor, sizeof(sensor)) &&
        !decode_sensor(sensor, settings)) {
      settings.sensor_vendor[0] = '\0';
      settings.sensor_model[0] = '\0';
      settings.sensor_serial[0] = '\0';
      settings.sensor_batch_candidate = false;
      aqlog.println("CONFIG SENSOR invalid-stored-profile=true");
    }
  }
  nvs_close(store);
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
  bool sensor_identity_changed = false;
  bool sensor_candidate_explicit = false;
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
    if (std::memchr(text + position, '\0', line_length)) {
      std::snprintf(bad_key, bad_key_size, "malformed");
      return false;
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
    } else if (std::strcmp(key, "sensor.vendor") == 0) {
      if (std::strlen(value) >= sizeof(next.sensor_vendor) ||
          (value[0] && std::strcmp(value, hardware_vendor) != 0))
        return reject();
      sensor_identity_changed = sensor_identity_changed ||
                                std::strcmp(value, next.sensor_vendor) != 0;
      std::snprintf(next.sensor_vendor, sizeof(next.sensor_vendor), "%s",
                    value);
    } else if (std::strcmp(key, "sensor.model") == 0) {
      if (std::strlen(value) >= sizeof(next.sensor_model) ||
          (value[0] && std::strcmp(value, hardware_model) != 0))
        return reject();
      sensor_identity_changed =
          sensor_identity_changed || std::strcmp(value, next.sensor_model) != 0;
      std::snprintf(next.sensor_model, sizeof(next.sensor_model), "%s", value);
    } else if (std::strcmp(key, "sensor.serial") == 0) {
      if (!sensor_serial_chars(value))
        return reject();
      sensor_identity_changed = sensor_identity_changed ||
                                std::strcmp(value, next.sensor_serial) != 0;
      std::snprintf(next.sensor_serial, sizeof(next.sensor_serial), "%s",
                    value);
    } else if (std::strcmp(key, "sensor.batch_candidate") == 0) {
      bool flag;
      if (!parse_bool(value, flag))
        return reject();
      sensor_candidate_explicit = true;
      next.sensor_batch_candidate = flag;
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
  if (sensor_identity_changed && !sensor_candidate_explicit)
    next.sensor_batch_candidate = false;
  if ((next.sensor_vendor[0] == '\0') != (next.sensor_model[0] == '\0') ||
      (next.sensor_serial[0] && !next.sensor_vendor[0])) {
    std::snprintf(bad_key, bad_key_size, "sensor.identity");
    return false;
  }
  if (next.sensor_serial[0] && !valid_pms5003t_serial(next.sensor_serial)) {
    std::snprintf(bad_key, bad_key_size, "sensor.serial");
    return false;
  }
  if (next.sensor_batch_candidate && !exact_batch_candidate(next)) {
    std::snprintf(bad_key, bad_key_size, "sensor.batch_candidate");
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
      "\"lan\":{\"on\":%u,\"port\":%u,\"host\":\"%s\",\"clients\":%u},"
      "\"sensor\":{\"vendor\":\"%s\",\"model\":\"%s\",\"serial\":\"%s\","
      "\"batch_candidate\":%u}}",
      pair_name(settings.pair), settings.pin <= 999999U ? 1U : 0U,
      settings.pin == kLegacyDefaultPin ? 1U : 0U, wifi.bonds,
      settings.display ? 1U : 0U, settings.wifi_on ? 1U : 0U, ssid,
      settings.psk[0] ? 1U : 0U, wifi.state, wifi.ip, wifi.rssi, wifi.mac,
      settings.lan_on ? 1U : 0U, unsigned(kLanPort), wifi.host, wifi.clients,
      settings.sensor_vendor, settings.sensor_model, settings.sensor_serial,
      settings.sensor_batch_candidate ? 1U : 0U);
  return written > 0 && std::size_t(written) < size ? std::size_t(written) : 0;
}
} // namespace config
