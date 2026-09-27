#include <Preferences.h>
#include <debug_log.h>
#include <device_config.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
bool same(const config::Settings &a, const config::Settings &b) {
  return a.pair == b.pair && a.pin == b.pin && a.display == b.display &&
         a.wifi_on == b.wifi_on && std::strcmp(a.ssid, b.ssid) == 0 &&
         std::strcmp(a.psk, b.psk) == 0 && a.lan_on == b.lan_on &&
         std::memcmp(a.token, b.token, config::kTokenBytes) == 0;
}

bool apply(const std::string &lines, config::Actions &actions, char *bad_key) {
  return config::apply_lines(lines.data(), lines.size(), bad_key, 64, actions);
}
void accept(const std::string &lines) {
  config::Actions actions;
  char bad_key[64];
  assert(apply(lines, actions, bad_key));
  assert(bad_key[0] == '\0');
}
void reject(const std::string &lines, const char *expected_key) {
  const auto before = config::get();
  const auto writes = fake_platform::nvs.writes;
  config::Actions actions;
  actions.clear_bonds = true; // output actions must remain unchanged on failure
  char bad_key[64];
  assert(!apply(lines, actions, bad_key));
  if (expected_key)
    assert(std::strcmp(bad_key, expected_key) == 0);
  assert(same(before, config::get()));
  assert(actions.clear_bonds);
  assert(fake_platform::nvs.writes == writes);
}

void first_boot(bool display) {
  assert(config::load(display));
  const auto first = config::get();
  assert(first.display == display);
  assert(first.pair ==
         (display ? config::PairMode::Random : config::PairMode::Fixed));
  assert(first.pin < 1000000 && first.pin != config::kLegacyDefaultPin);
  assert(!first.wifi_on && first.lan_on && !first.ssid[0] && !first.psk[0]);
  assert(fake_platform::nvs.numbers.at("pin") == first.pin);
  assert(fake_platform::nvs.blobs.at("token").size() == config::kTokenBytes);
  assert(!config::reboot_required());
  assert(
      config::load(!display)); // stored capability/pairing defaults are frozen
  assert(same(first, config::get()));
  fake_platform::nvs = {}; // a second board's clean NVS gets another PIN/token
  assert(config::load(display));
  const auto second = config::get();
  assert(second.pin != first.pin);
  assert(std::memcmp(second.token, first.token, config::kTokenBytes) != 0);
}

void migration() {
  assert(config::load(false));
  const auto original = config::get();
  fake_platform::nvs.numbers["pin"] = config::kLegacyDefaultPin;
  assert(config::load(false));
  const auto migrated = config::get();
  assert(migrated.pin != config::kLegacyDefaultPin && migrated.pin < 1000000);
  assert(migrated.pair == config::PairMode::Fixed);
  assert(std::memcmp(migrated.token, original.token, config::kTokenBytes) == 0);
  assert(fake_platform::nvs.numbers.at("pin") == migrated.pin);
  assert(config::load(false));
  assert(same(migrated, config::get()));
}

void validation() {
  assert(config::load(true));
  reject("wifi.on=1", "wifi.ssid");
  reject("wifi.ssid=accepted\nble.pin=wrong", "ble.pin");
  reject("lan.rotate_token=true\nunknown=1", "unknown");
  reject("ble.pair=fixed\nwifi.on=yes", "wifi.on");
  reject("ble.pin=01234", "ble.pin");
  reject("ble.pin=01234x", "ble.pin");
  reject("ble.pair=invalid", "ble.pair");
  reject("\r\n\n", "empty");
  reject(std::string(128, 'x'), "line-too-long");
  reject("missing-equals", "missing-equals");
  std::string nul = "wifi.ssid=visible";
  nul.push_back('\0');
  nul += "hidden";
  reject(nul, nullptr);
  config::Actions actions;
  char bad_key[64];
  assert(apply("ble.pair=fixed\r\nble.pin=012345\nble.clear_bonds=1\n"
               "lan.rotate_token=true\n",
               actions, bad_key));
  assert(actions.ble_changed && actions.clear_bonds && actions.rotate_token);
  assert(config::get().pin == 12345 && config::reboot_required());
  assert(config::load(true));
  assert(!config::reboot_required());
}

void credentials() {
  assert(config::load(false));
  reject("wifi.ssid=" + std::string(33, 's'), "wifi.ssid");
  reject("wifi.psk=" + std::string(7, 'p'), "wifi.psk");
  reject("wifi.psk=" + std::string(64, 'p'), "wifi.psk");
  reject("wifi.ssid=bad\tname", "wifi.ssid");
  reject(std::string("wifi.psk=bad") + char(127) + "password", "wifi.psk");
  accept("wifi.ssid=" + std::string(32, 's') +
         "\nwifi.psk=" + std::string(63, 'p'));
  accept("wifi.psk=\nwifi.on=true"); // an open network is explicit and valid
  accept("wifi.ssid=fixture\"\\name\nwifi.psk=fixture-only-password\nble.pin="
         "654321");
  const auto settings = config::get();
  config::WifiView wifi{"connected", "192.0.2.1",  -40, "00:00:00:00:00:01",
                        1,           "aq-fixture", 2};
  char json[512];
  const auto size = config::build_json(json, sizeof(json), wifi);
  assert(size > 0 && size == std::strlen(json));
  assert(!std::strstr(json, settings.psk));
  assert(!std::strstr(json, "654321"));
  assert(!std::strstr(json, "\"pin\":"));
  assert(!std::strstr(json, "\"psk\":"));
  assert(!std::strstr(json, "\"token\":"));
  char truncated[8];
  assert(config::build_json(truncated, sizeof(truncated), wifi) == 0);
  char logs[DebugLog::kRingBytes + 1];
  std::uint32_t total = 0;
  const auto count = aqlog.tail(logs, sizeof(logs) - 1, total);
  logs[count] = '\0';
  assert(!std::strstr(logs, settings.psk) && !std::strstr(logs, "654321"));
  std::puts(json); // runner parses this synthetic, secret-free CONFIG document
}

void persistence_failure() {
  assert(config::load(false));
  accept("wifi.ssid=fixture-network\nwifi.psk=fixture-password");
  const auto before = config::get();
  for (const auto *key : {"ssid", "psk", "pin", "token"}) {
    fake_platform::nvs.fail_write_key = key;
    config::Actions actions;
    char bad_key[64];
    const std::string change = std::strcmp(key, "ssid") == 0  ? "wifi.ssid="
                               : std::strcmp(key, "psk") == 0 ? "wifi.psk="
                               : std::strcmp(key, "pin") == 0
                                   ? "ble.pin=000001"
                                   : "lan.rotate_token=1";
    assert(!apply(change, actions, bad_key));
    assert(std::strcmp(bad_key, "nvs") == 0);
    assert(same(before, config::get()));
    fake_platform::nvs.fail_write_key.clear();
  }
  fake_platform::nvs.fail_write_key = "token";
  assert(!config::rotate_token());
  assert(same(before, config::get()));
  fake_platform::nvs.fail_write_key.clear();
  assert(config::rotate_token());
  assert(std::memcmp(before.token, config::get().token, config::kTokenBytes) !=
         0);
  fake_platform::nvs.fail_open = true;
  const auto rotated = config::get();
  config::Actions actions;
  char bad_key[64];
  assert(!apply("lan.on=false", actions, bad_key));
  assert(std::strcmp(bad_key, "nvs") == 0);
  assert(same(rotated, config::get()));
}

void unavailable() {
  fake_platform::nvs.fail_open = true;
  assert(!config::load(false));
  assert(config::get().pair == config::PairMode::Fixed);
  assert(!config::get().display);
  assert(fake_platform::nvs.writes == 0);
  accept("wifi.ssid=ram-only\nwifi.on=true");
  assert(config::get().wifi_on && fake_platform::nvs.writes == 0);
}
} // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  const std::string test = argv[1];
  if (test == "headless")
    first_boot(false);
  else if (test == "screen")
    first_boot(true);
  else if (test == "migration")
    migration();
  else if (test == "validation")
    validation();
  else if (test == "credentials")
    credentials();
  else if (test == "nvs")
    persistence_failure();
  else if (test == "unavailable")
    unavailable();
  else
    assert(false);
}
