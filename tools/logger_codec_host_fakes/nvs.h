#pragma once

// Host fake for the few NVS calls the persisted codec makes. Entries are keyed
// by namespace and key and remember their NVS type, so a value written under
// another type reads back as ESP_ERR_NVS_TYPE_MISMATCH like the IDF does. Sets
// stage until nvs_commit and are dropped by nvs_close. Tests inject open, read,
// write and commit failures. This proves the logic and the error mapping, not
// flash wear, power loss or the real NVS page format.
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>

using esp_err_t = int;
using nvs_handle_t = unsigned;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
constexpr esp_err_t ESP_ERR_NVS_TYPE_MISMATCH = 0x1103;
constexpr unsigned NVS_READONLY = 0;
constexpr unsigned NVS_READWRITE = 1;

namespace fake_nvs {
enum class Type : std::uint8_t { U8, U32, Str };
struct Entry {
  Type type;
  std::uint32_t number;
};
using Key = std::pair<std::string, std::string>; // namespace, key
struct State {
  std::map<Key, Entry> committed;
  std::map<Key, Entry> pending;
  std::string open_namespace;
  bool opened = false;
  bool fail_open = false;
  esp_err_t fail_get = ESP_OK;
  bool fail_set = false;
  bool fail_commit = false;
  unsigned opens = 0, closes = 0, sets = 0, commits = 0;
};
inline State state;
inline void reset() { state = State{}; }
inline void put(const char *ns, const char *key, Type type,
                std::uint32_t value) {
  state.committed[{ns, key}] = Entry{.type = type, .number = value};
}
} // namespace fake_nvs

inline esp_err_t nvs_open(const char *name, unsigned, nvs_handle_t *handle) {
  auto &s = fake_nvs::state;
  ++s.opens;
  if (s.fail_open)
    return ESP_FAIL;
  s.opened = true;
  s.open_namespace = name;
  s.pending.clear();
  *handle = 1;
  return ESP_OK;
}

inline void nvs_close(nvs_handle_t) {
  auto &s = fake_nvs::state;
  ++s.closes;
  s.opened = false;
  s.pending.clear();
}

inline esp_err_t nvs_get_u8(nvs_handle_t, const char *key, std::uint8_t *out) {
  auto &s = fake_nvs::state;
  if (!s.opened)
    return ESP_FAIL;
  if (s.fail_get != ESP_OK)
    return s.fail_get;
  const fake_nvs::Key id{s.open_namespace, key};
  const fake_nvs::Entry *entry = nullptr;
  if (const auto pending = s.pending.find(id); pending != s.pending.end())
    entry = &pending->second;
  else if (const auto stored = s.committed.find(id);
           stored != s.committed.end())
    entry = &stored->second;
  if (!entry)
    return ESP_ERR_NVS_NOT_FOUND;
  if (entry->type != fake_nvs::Type::U8)
    return ESP_ERR_NVS_TYPE_MISMATCH;
  *out = static_cast<std::uint8_t>(entry->number);
  return ESP_OK;
}

inline esp_err_t nvs_set_u8(nvs_handle_t, const char *key, std::uint8_t value) {
  auto &s = fake_nvs::state;
  ++s.sets;
  if (!s.opened || s.fail_set)
    return ESP_FAIL;
  s.pending[{s.open_namespace, key}] =
      fake_nvs::Entry{.type = fake_nvs::Type::U8, .number = value};
  return ESP_OK;
}

inline esp_err_t nvs_commit(nvs_handle_t) {
  auto &s = fake_nvs::state;
  ++s.commits;
  if (!s.opened || s.fail_commit)
    return ESP_FAIL;
  for (const auto &[id, entry] : s.pending)
    s.committed[id] = entry;
  s.pending.clear();
  return ESP_OK;
}
