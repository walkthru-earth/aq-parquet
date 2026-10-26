// The shared runtime is board neutral. These fixtures borrow the Waveshare
// PMS5003T rules as a realistic board policy.
#include "../firmware/esp-idf-waveshare-sim7670g/logger/pms5003t_identity.h"
#include <aq_location.h>
#include <debug_log.h>
#include <device_config.h>
#include <nvs.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr config::SensorPolicy kPms5003t{pms5003t::kVendor, pms5003t::kModel,
                                         pms5003t::valid_serial,
                                         pms5003t::batch_eligible};

// Pinned `cfg` blob contract. A layout change must bump the version.
constexpr std::size_t kHeaderBytes = 12;
constexpr std::size_t kPayloadBytes = 817;
constexpr std::size_t kPairAt = 0, kPinAt = 1, kDisplayAt = 5, kWifiAt = 6,
                      kLanAt = 7, kSsidAt = 8, kPskAt = 41, kTokenAt = 105,
                      kSensorAt = 137, kLocationAt = 217, kNtpAt = 257;

std::uint32_t crc32(const std::uint8_t *data, std::size_t size) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}
std::uint32_t le32(const std::uint8_t *p) {
  return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 |
         std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
void put_le32(std::uint8_t *p, std::uint32_t value) {
  for (int i = 0; i < 4; ++i)
    p[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
std::vector<std::uint8_t> &cfg() { return fake_platform::nvs.blobs.at("cfg"); }
std::uint8_t *payload() { return cfg().data() + kHeaderBytes; }
char *field(std::size_t offset) {
  return reinterpret_cast<char *>(payload() + offset);
}
void set_field(std::size_t offset, std::size_t size, const char *text) {
  std::memset(field(offset), 0, size);
  std::memcpy(field(offset), text, std::strlen(text));
}
void reseal() {
  put_le32(cfg().data() + 8, crc32(payload(), cfg().size() - kHeaderBytes));
}
bool has_legacy_key() {
  for (const auto *key : {"pair", "pin", "disp", "wifion", "ssid", "psk",
                          "lanon", "token", "sensor", "location", "ntp"})
    if (fake_platform::nvs.types.count(key))
      return true;
  return false;
}
void check_blob_header() {
  const auto &blob = cfg();
  assert(fake_platform::nvs.types.at("cfg") == 9);
  assert(blob.size() == kHeaderBytes + kPayloadBytes);
  assert(std::memcmp(blob.data(), "AQCF", 4) == 0);
  assert(blob[4] == 1 && blob[5] == 0);
  assert(blob[6] == (kPayloadBytes & 0xff) && blob[7] == (kPayloadBytes >> 8));
  assert(le32(blob.data() + 8) ==
         crc32(blob.data() + kHeaderBytes, kPayloadBytes));
}
std::string log_text() {
  static char logs[DebugLog::kFallbackRingBytes + 1];
  std::uint32_t total = 0;
  const auto count = oslog.tail(logs, sizeof(logs) - 1, total);
  logs[count] = '\0';
  return logs;
}

// A pre-blob image: every setting in its own typed key, as older firmware
// committed them.
void seed_legacy(bool with_policy_records) {
  auto &nvs = fake_platform::nvs;
  nvs = {};
  const auto number = [&](const char *key, std::uint32_t value, unsigned type) {
    nvs.numbers[key] = value;
    nvs.types[key] = type;
  };
  const auto string = [&](const char *key, const char *value) {
    nvs.strings[key] = value;
    nvs.types[key] = 8;
  };
  number("pair", 1, 1);
  number("pin", 654321, 4);
  number("disp", 0, 1);
  number("wifion", 1, 1);
  string("ssid", "legacy-net");
  string("psk", "legacy-password");
  number("lanon", 0, 1);
  nvs.blobs["token"] = std::vector<std::uint8_t>(config::kTokenBytes, 0x5a);
  nvs.types["token"] = 9;
  string("sensor", "Plantower|PMS5003T|PMS5003T-202604081332|1");
  if (with_policy_records) {
    string("location", "|5|EG");
    string("ntp", "1|0|0|7200|ntp.local|");
  }
}
void check_legacy_values(const config::Settings &settings) {
  assert(settings.pair == config::PairMode::Fixed && settings.pin == 654321);
  assert(!settings.display && settings.wifi_on && !settings.lan_on);
  assert(std::strcmp(settings.ssid, "legacy-net") == 0);
  assert(std::strcmp(settings.psk, "legacy-password") == 0);
  for (const auto byte : settings.token)
    assert(byte == 0x5a);
  assert(std::strcmp(settings.sensor_serial, "PMS5003T-202604081332") == 0);
  assert(settings.sensor_batch_candidate);
}
bool same(const config::Settings &a, const config::Settings &b) {
  return a.pair == b.pair && a.pin == b.pin && a.display == b.display &&
         a.wifi_on == b.wifi_on && std::strcmp(a.ssid, b.ssid) == 0 &&
         std::strcmp(a.psk, b.psk) == 0 && a.lan_on == b.lan_on &&
         std::strcmp(a.sensor_vendor, b.sensor_vendor) == 0 &&
         std::strcmp(a.sensor_model, b.sensor_model) == 0 &&
         std::strcmp(a.sensor_serial, b.sensor_serial) == 0 &&
         a.sensor_batch_candidate == b.sensor_batch_candidate &&
         config::same_location(a.location, b.location) &&
         a.ntp.enabled == b.ntp.enabled && a.ntp.dhcp == b.ntp.dhcp &&
         a.ntp.public_fallback == b.ntp.public_fallback &&
         a.ntp.interval_s == b.ntp.interval_s &&
         std::strcmp(a.ntp.servers[0], b.ntp.servers[0]) == 0 &&
         std::strcmp(a.ntp.servers[1], b.ntp.servers[1]) == 0 &&
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
  assert(!config::reboot_required());
  // A fresh device seeds one CRC-protected record with one write and commit.
  check_blob_header();
  assert(le32(payload() + kPinAt) == first.pin);
  assert(std::memcmp(payload() + kTokenAt, first.token, config::kTokenBytes) ==
         0);
  assert(!has_legacy_key());
  assert(fake_platform::nvs.writes == 1 && fake_platform::nvs.commits == 1);
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
  put_le32(payload() + kPinAt, config::kLegacyDefaultPin);
  reseal();
  assert(config::load(false));
  const auto migrated = config::get();
  assert(migrated.pin != config::kLegacyDefaultPin && migrated.pin < 1000000);
  assert(migrated.pair == config::PairMode::Fixed);
  assert(std::memcmp(migrated.token, original.token, config::kTokenBytes) == 0);
  check_blob_header();
  assert(le32(payload() + kPinAt) == migrated.pin);
  assert(config::load(false));
  assert(same(migrated, config::get()));
  // Legacy per-key universal PIN still rotates, while the old key is retained.
  seed_legacy(true);
  fake_platform::nvs.numbers["pin"] = config::kLegacyDefaultPin;
  assert(config::load(false));
  const auto rotated = config::get();
  assert(rotated.pin != config::kLegacyDefaultPin && rotated.pin < 1000000);
  check_blob_header();
  assert(le32(payload() + kPinAt) == rotated.pin);
  assert(fake_platform::nvs.numbers.at("pin") == config::kLegacyDefaultPin);
  assert(config::load(false));
  assert(same(rotated, config::get()));
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
  reject(std::string(config::kNtpServerMax + 32, 'x'), "line-too-long");
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
  char logs[DebugLog::kFallbackRingBytes + 1];
  std::uint32_t total = 0;
  const auto count = oslog.tail(logs, sizeof(logs) - 1, total);
  logs[count] = '\0';
  assert(!std::strstr(logs, settings.psk) && !std::strstr(logs, "654321"));
  std::puts(json); // runner parses this synthetic, secret-free CONFIG document
}

bool any_serial(const char *) { return true; }
bool no_batch(const char *, const char *, const char *) { return false; }

// The runtime holds no sensor rule of its own. It defers to the board hook.
void board_policy_hooks() {
  accept("sensor.vendor=\nsensor.model=\nsensor.serial=");
  config::set_sensor_policy({"Vendor", "Model", nullptr, nullptr});
  accept("sensor.vendor=Vendor\nsensor.model=Model");
  reject("sensor.serial=ANY-1", "sensor.serial");
  reject("sensor.batch_candidate=1", "sensor.batch_candidate");
  config::set_sensor_policy({"Vendor", "Model", any_serial, no_batch});
  accept("sensor.serial=ANY-1");
  reject("sensor.batch_candidate=1", "sensor.batch_candidate");
  accept("sensor.vendor=\nsensor.model=\nsensor.serial=");
  config::set_sensor_policy({});
  reject("sensor.vendor=Plantower\nsensor.model=PMS5003T", "sensor.vendor");
  config::set_sensor_policy(kPms5003t);
}

void sensor_identity() {
  config::set_sensor_policy(kPms5003t);
  assert(config::load(false));
  reject("sensor.vendor=Other\nsensor.model=PMS5003T", "sensor.vendor");
  reject("sensor.vendor=Plantower\nsensor.model=PMSA003", "sensor.model");
  reject("sensor.serial=bad barcode", "sensor.serial");
  reject("sensor.vendor=Plantower\nsensor.model=PMS5003T\n"
         "sensor.serial=PMS5003T-202602291332",
         "sensor.serial");
  reject("sensor.vendor=Plantower\nsensor.model=PMS5003T\n"
         "sensor.serial=PMS5003T-202613011332",
         "sensor.serial");
  reject("sensor.vendor=Plantower\nsensor.model=PMS5003T\n"
         "sensor.serial=PMS5003T-20260408",
         "sensor.serial");
  reject("sensor.vendor=Plantower\nsensor.model=PMS5003T\n"
         "sensor.serial=PMS5003T-202605011332\nsensor.batch_candidate=1",
         "sensor.batch_candidate");
  accept("sensor.vendor=Plantower\nsensor.model=PMS5003T\n"
         "sensor.serial=PMS5003T-202604081332\nsensor.batch_candidate=1");
  const auto selected = config::get();
  assert(selected.sensor_batch_candidate &&
         std::strcmp(selected.sensor_serial, "PMS5003T-202604081332") == 0);
  assert(config::load(false) && same(selected, config::get()));
  config::WifiView wifi{"connected", "192.0.2.1",  -40, "00:00:00:00:00:01",
                        1,           "aq-fixture", 2};
  char json[config::kSensorSerialMax + 480]{};
  const auto length = config::build_json(json, 480, wifi);
  assert(length && length < 480);
  assert(std::strstr(json, "\"serial\":\"PMS5003T-202604081332\""));
  assert(std::strstr(json, "\"batch_candidate\":1"));
  accept("sensor.serial=PMS5003T-20260408123456789\nwifi.ssid=" +
         std::string(32, '\\'));
  config::WifiView widest{
      "connecting", "255.255.255.255",       -2147483647, "ff:ff:ff:ff:ff:ff",
      4294967295U,  "aq-123456789012345678", 4294967295U};
  assert(config::build_json(json, 480, widest) != 0);
  accept("sensor.serial=PMS5003T-202605011332");
  assert(!config::get().sensor_batch_candidate);
  reject("sensor.batch_candidate=1", "sensor.batch_candidate");
  accept("sensor.serial=PMS5003T-2026040813327\nsensor.batch_candidate=1");
  assert(config::get().sensor_batch_candidate);
  accept("sensor.batch_candidate=0");
  accept("sensor.serial=PMS5003T-202402291");
  accept("sensor.serial=");
  assert(!config::get().sensor_serial[0]);
  fake_platform::nvs.fail_write_key = "cfg";
  const auto before = config::get();
  config::Actions actions;
  char bad_key[64];
  assert(!apply("sensor.serial=PMS5003T-202604081332", actions, bad_key));
  assert(std::strcmp(bad_key, "nvs") == 0 && same(before, config::get()));
  fake_platform::nvs.fail_write_key.clear();
  board_policy_hooks();
}

void persistence_failure() {
  assert(config::load(false));
  accept("wifi.ssid=fixture-network\nwifi.psk=fixture-password");
  const auto before = config::get();
  const auto stored = cfg();
  // Every setting shares one record, so one failed blob write or commit
  // rejects the whole save and the previous record stays readable.
  for (const auto *change :
       {"wifi.ssid=", "wifi.psk=", "ble.pin=000001", "lan.rotate_token=1"}) {
    for (const bool commit_fails : {false, true}) {
      (commit_fails ? fake_platform::nvs.fail_commit_key
                    : fake_platform::nvs.fail_write_key) = "cfg";
      const auto writes = fake_platform::nvs.writes;
      config::Actions actions;
      char bad_key[64];
      assert(!apply(change, actions, bad_key));
      assert(std::strcmp(bad_key, "nvs") == 0);
      assert(same(before, config::get()));
      assert(fake_platform::nvs.writes == writes + 1);
      assert(cfg() == stored);
      fake_platform::nvs.fail_write_key.clear();
      fake_platform::nvs.fail_commit_key.clear();
    }
  }
  assert(config::load(false));
  assert(same(before, config::get()));
  fake_platform::nvs.fail_commit_key = "cfg";
  assert(!config::rotate_token());
  assert(same(before, config::get()));
  fake_platform::nvs.fail_commit_key.clear();
  fake_platform::nvs.fail_write_key = "cfg";
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

void time_and_location() {
  assert(config::load(false));
  assert(config::get().ntp.enabled && config::get().ntp.dhcp &&
         config::get().ntp.public_fallback);
  assert(config::get().location.cell == 0 &&
         config::get().location.resolution == 5);
  char json[480];
  assert(config::build_page(json, sizeof(json), 1));
  assert(std::strstr(json, "\"lat\":null"));
  reject("location.cell=fffffffffffffff", "location.cell");
  reject("location.resolution=16", "location.resolution");
  reject("location.country=ZZ", "location.country");
  reject("location.country=eg", "location.country");
  reject("ntp.interval_s=59", "ntp.interval_s");
  reject("ntp.interval_s=42949672960", "ntp.interval_s");
  reject("ntp.server1=https://time.cloudflare.com", "ntp.server1");
  reject("ntp.server1=host|injection", "ntp.server1");
  reject("ntp.server1=host:123", "ntp.server1");
  reject("ntp.server1=0.0.0.0", "ntp.server1");
  reject("ntp.server1=" + std::string(64, 'a'), "ntp.server1");
  accept("ntp.server2=2001:db8::1");
  reject("ntp.server1=" + std::string(254, 'a'), "ntp.server1");
  config::Actions actions;
  char bad_key[64];
  assert(apply("location.cell=8928308280fffff\nlocation.resolution=5\nlocation."
               "country=EG",
               actions, bad_key));
  assert(actions.location_changed && !actions.ntp_changed &&
         !actions.wifi_changed);
  const auto coarse = config::get().location.cell;
  auto country_change = config::get().location;
  country_change.country[0] = 'U';
  country_change.country[1] = 'S';
  assert(!config::same_location(country_change, config::get().location));
  auto cap_change = config::get().location;
  cap_change.resolution = 6;
  assert(!config::same_location(cap_change, config::get().location));
  assert(opensensor::location::resolution(coarse) == 5 &&
         coarse != 0x8928308280fffffULL);
  assert(std::string(field(kLocationAt)).find("8928308280fffff") ==
         std::string::npos);
  assert(config::build_page(json, sizeof(json), 1));
  assert(std::strstr(json, "\"country\":\"EG\""));
  assert(config::load(false));
  assert(config::get().location.cell == coarse);
  accept("location.resolution=9");
  assert(config::get().location.cell == coarse); // finer cannot reconstruct GPS
  accept("location.resolution=5\nlocation.cell=8928308280fffff");
  assert(config::get().location.cell ==
         coarse); // patch order does not leak finer cell
  assert(apply("ntp.dhcp=0\nntp.public_fallback=0\nntp.server1=ntp.local\nntp."
               "server2=192.168.1.1\nntp.interval_s=7200",
               actions, bad_key));
  assert(actions.ntp_changed && !actions.wifi_changed);
  assert(config::load(false));
  const auto ntp = config::get().ntp;
  assert(!ntp.dhcp && !ntp.public_fallback && ntp.interval_s == 7200);
  assert(std::strcmp(ntp.servers[0], "ntp.local") == 0);
  assert(config::build_page(json, sizeof(json), 2));
  assert(config::build_page(json, sizeof(json), 3));
  assert(std::strstr(json, "ntp.local"));
  // Maximum supported DNS name remains readable within the wire JSON limit.
  accept("ntp.server1=" + std::string(63, 'a') + "." + std::string(63, 'b') +
         "." + std::string(63, 'c') + "." + std::string(61, 'd'));
  assert(config::build_page(json, sizeof(json), 3));
  assert(config::load(false));
  assert(std::strlen(config::get().ntp.servers[0]) == 253);
  assert(!config::build_page(json, 8, 1) && json[0] == '\0');
  assert(!config::build_page(json, sizeof(json), 5));
  const auto before_failure = config::get();
  fake_platform::nvs.fail_write_key = "cfg";
  assert(!apply("location.country=US", actions, bad_key));
  assert(std::strcmp(bad_key, "nvs") == 0);
  assert(same(before_failure, config::get()));
  fake_platform::nvs.fail_write_key.clear();
  fake_platform::nvs.fail_commit_key = "cfg";
  assert(!apply("ntp.public_fallback=1", actions, bad_key));
  assert(std::strcmp(bad_key, "nvs") == 0);
  assert(same(before_failure, config::get()));
  fake_platform::nvs.fail_commit_key.clear();
  assert(config::load(false));
  assert(same(before_failure, config::get()));
  // A sealed record still passes the same validation as per-key values.
  set_field(kLocationAt, 40, "broken|15|EG");
  set_field(kNtpAt, 560, "broken");
  reseal();
  assert(config::load(false));
  assert(config::get().location.cell == 0);
  assert(!config::get().ntp.enabled && !config::get().ntp.public_fallback);
  accept("location.cell=\nlocation.country=");
  assert(config::get().location.cell == 0);
}

void blob_round_trip() {
  config::set_sensor_policy(kPms5003t);
  assert(config::load(false));
  const auto writes = fake_platform::nvs.writes;
  const auto commits = fake_platform::nvs.commits;
  accept("wifi.ssid=round-trip\nwifi.psk=round-trip-password\nwifi.on=1\n"
         "ble.pair=none\nble.pin=024680\nlan.on=0\n"
         "sensor.vendor=Plantower\nsensor.model=PMS5003T\n"
         "sensor.serial=PMS5003T-202604081332\nsensor.batch_candidate=1\n"
         "location.cell=8928308280fffff\nlocation.resolution=7\n"
         "location.country=EG\nntp.dhcp=0\nntp.server1=ntp.local\n"
         "ntp.server2=192.168.1.1\nntp.interval_s=900");
  // One complete SET_CONFIG is one blob write and one commit.
  assert(fake_platform::nvs.writes == writes + 1);
  assert(fake_platform::nvs.commits == commits + 1);
  check_blob_header();
  assert(!has_legacy_key());
  const auto saved = config::get();
  assert(std::strcmp(field(kPskAt), "round-trip-password") == 0);
  assert(payload()[kPairAt] == 2 && payload()[kWifiAt] == 1 &&
         payload()[kLanAt] == 0 && payload()[kDisplayAt] == 0);
  assert(config::load(false));
  assert(same(saved, config::get()));
  assert(config::get().pair == config::PairMode::None &&
         config::get().pin == 24680);
  assert(config::get().location.cell && config::get().location.resolution == 7);
  assert(config::get().ntp.interval_s == 900 && !config::get().ntp.dhcp);
  const auto logs = log_text();
  assert(logs.find("round-trip-password") == std::string::npos);
  assert(logs.find("024680") == std::string::npos);
  assert(logs.find("CONFIG BLOB") == std::string::npos);
}

void legacy_migration() {
  config::set_sensor_policy(kPms5003t);
  seed_legacy(true);
  const fake_platform::Values legacy = fake_platform::nvs;
  assert(config::load(true));
  const auto loaded = config::get();
  check_legacy_values(loaded);
  assert(std::strcmp(loaded.location.country, "EG") == 0);
  assert(loaded.ntp.enabled && !loaded.ntp.dhcp &&
         !loaded.ntp.public_fallback && loaded.ntp.interval_s == 7200 &&
         std::strcmp(loaded.ntp.servers[0], "ntp.local") == 0);
  // Reading the legacy layout alone writes nothing.
  assert(fake_platform::nvs.writes == 0 &&
         !fake_platform::nvs.types.count("cfg"));
  accept("wifi.on=0");
  check_blob_header();
  // Legacy keys stay exactly as older firmware left them.
  auto &nvs = fake_platform::nvs;
  assert(nvs.numbers == legacy.numbers && nvs.strings == legacy.strings);
  for (const auto &entry : legacy.blobs)
    assert(nvs.blobs.at(entry.first) == entry.second);
  for (const auto &entry : legacy.types)
    assert(nvs.types.at(entry.first) == entry.second);
  assert(nvs.blobs.size() == legacy.blobs.size() + 1);
  const auto migrated = config::get();
  assert(!migrated.wifi_on);
  assert(config::load(true));
  assert(same(migrated, config::get()));
  // Missing policy records on a legacy image receive defaults.
  seed_legacy(false);
  assert(config::load(false));
  check_legacy_values(config::get());
  assert(config::get().ntp.enabled && config::get().ntp.dhcp &&
         config::get().ntp.public_fallback);
  assert(config::get().location.cell == 0 &&
         !config::get().location.country[0]);
  // Malformed legacy records become unset location and disabled NTP.
  seed_legacy(true);
  fake_platform::nvs.strings["location"] = "broken|15|EG";
  fake_platform::nvs.strings["ntp"] = "broken";
  assert(config::load(false));
  assert(config::get().location.cell == 0 &&
         !config::get().location.country[0]);
  assert(!config::get().ntp.enabled && !config::get().ntp.public_fallback);
}

void torn_blob() {
  config::set_sensor_policy(kPms5003t);
  seed_legacy(true);
  assert(config::load(false));
  accept("wifi.ssid=blob-net\nwifi.psk=blob-password");
  const auto good = cfg();
  const auto blob_settings = config::get();
  using Damage = void (*)(std::vector<std::uint8_t> &);
  const Damage damages[] = {
      [](std::vector<std::uint8_t> &b) { b[kHeaderBytes + kSsidAt] ^= 1; },
      [](std::vector<std::uint8_t> &b) { b[8] ^= 0x80; },
      [](std::vector<std::uint8_t> &b) { b.pop_back(); },
      [](std::vector<std::uint8_t> &b) { b.resize(kHeaderBytes); },
      [](std::vector<std::uint8_t> &b) { b.resize(3); },
      [](std::vector<std::uint8_t> &b) { b.push_back(0); },
      [](std::vector<std::uint8_t> &b) { b[0] = 'X'; },
      [](std::vector<std::uint8_t> &b) { b[4] = 2; },
      [](std::vector<std::uint8_t> &b) { b[6] = 0x30; },
      [](std::vector<std::uint8_t> &b) {
        // A sealed record whose fixed text field lost its terminator.
        std::memset(b.data() + kHeaderBytes + kSsidAt, 's',
                    config::kSsidMax + 1);
      },
  };
  unsigned index = 0;
  for (const auto damage : damages) {
    cfg() = good;
    damage(cfg());
    if (index == 9) // reseal so the terminator check is reached on its own
      reseal();
    const auto writes = fake_platform::nvs.writes;
    assert(config::load(false));
    check_legacy_values(config::get());
    assert(fake_platform::nvs.writes == writes); // no silent rewrite
    assert(log_text().find("CONFIG BLOB invalid=true fallback=legacy") !=
           std::string::npos);
    cfg() = good;
    assert(config::load(false));
    assert(same(blob_settings, config::get()));
    ++index;
  }
  // A type mismatch on `cfg` is also a torn record.
  fake_platform::nvs.blobs.erase("cfg");
  fake_platform::nvs.strings["cfg"] = "not-a-blob";
  fake_platform::nvs.types["cfg"] = 8;
  assert(config::load(false));
  check_legacy_values(config::get());
  // A damaged record with no legacy keys seeds a first-boot profile again.
  fake_platform::nvs = {};
  assert(config::load(false));
  cfg()[kHeaderBytes] ^= 1;
  assert(config::load(false));
  assert(config::get().pair == config::PairMode::Fixed);
  check_blob_header();
  assert(!has_legacy_key());
  // Invalid sensor text inside a sealed record clears only the sensor profile.
  cfg() = good;
  set_field(kSensorAt, 80, "Other|PMS5003T|PMS5003T-202604081332|1");
  reseal();
  assert(config::load(false));
  assert(!config::get().sensor_vendor[0] &&
         !config::get().sensor_batch_candidate);
  assert(std::strcmp(config::get().ssid, "blob-net") == 0);
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
  else if (test == "sensor")
    sensor_identity();
  else if (test == "nvs")
    persistence_failure();
  else if (test == "time-location")
    time_and_location();
  else if (test == "blob")
    blob_round_trip();
  else if (test == "legacy")
    legacy_migration();
  else if (test == "torn")
    torn_blob();
  else if (test == "unavailable")
    unavailable();
  else
    assert(false);
}
