#pragma once

// Widest real LIVE row for the board contract tests (#91). Each board lists,
// for every LIVE key, the widest value its producer can write while the key
// is valid. Every declared key is always sent, as a number or as null, so a
// key whose widest value is shorter than `null` (a one digit code, or 100)
// is widest when it has no reading. The helper fills a row from the list,
// turns each such key to null, checks every key is printed and returns the
// object size, the largest LIVE object the board can send. A new
// LIVE key without an entry, or an entry for a key the board does not have,
// fails the test, so whoever adds a key must state its widest value.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <os_logger_live.h>
#include <span>

namespace livewidest {
struct Widest {
  const char *key;
  std::int64_t integer = 0; // INT32 and INT64 keys, stored units
  float number = 0.0F;      // FLOAT keys
};

inline std::size_t measure(const oslogger::Field *table, std::size_t count,
                           std::span<const Widest> widest) {
  assert(oslogger::valid_live_keys(table, count));
  oslogger::Row row{};
  std::size_t keys = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if (!table[i].live_key)
      continue;
    ++keys;
    const Widest *entry = nullptr;
    for (const auto &candidate : widest)
      if (std::strcmp(candidate.key, table[i].live_key) == 0)
        entry = &candidate;
    assert(entry && "every LIVE key needs a widest value");
    if (!entry)
      continue;
    switch (table[i].type) {
    case telemetry::PhysicalType::Int64:
      row.set(i, entry->integer);
      break;
    case telemetry::PhysicalType::Int32:
      assert(entry->integer >= INT32_MIN && entry->integer <= INT32_MAX);
      row.set(i, static_cast<std::int32_t>(entry->integer));
      break;
    case telemetry::PhysicalType::Float:
      row.set(i, entry->number);
      break;
    case telemetry::PhysicalType::ByteArray:
      // valid_live_keys above refuses a LIVE key on a byte array column.
      break;
    }
  }
  assert(keys == widest.size() && "a widest entry names no LIVE key");
  // Sizes add up per key, so choosing the wider of value and null for each
  // key on its own gives the widest object.
  for (std::size_t i = 0; i < count; ++i) {
    if (!table[i].live_key)
      continue;
    const auto as_value = oslogger::live_size(table, count, row);
    row.valid[i] = 0;
    if (oslogger::live_size(table, count, row) <= as_value)
      row.valid[i] = 1;
  }
  char json[ble::kMaxJson + 1];
  const auto used =
      oslogger::format_live(table, count, row, json, sizeof(json));
  const auto needed = oslogger::live_size(table, count, row);
  assert(used == 0 || used == needed);
  // Every key must be printed, so nothing hides as an omitted value.
  if (used)
    for (const auto &entry : widest) {
      char quoted[oslogger::kMaxLiveKey + 4];
      std::snprintf(quoted, sizeof(quoted), "\"%s\":", entry.key);
      assert(std::strstr(json, quoted));
    }
  return needed;
}

// Size of the object with every declared key null, which the logger sends
// when a row overflows. It must fit, or an overflow would publish nothing.
inline std::size_t all_null(const oslogger::Field *table, std::size_t count) {
  const oslogger::Row none{};
  char json[ble::kMaxJson + 1];
  const auto used =
      oslogger::format_live(table, count, none, json, sizeof(json));
  assert(used > 0 && used == oslogger::live_size(table, count, none));
  for (std::size_t i = 0; i < count; ++i)
    if (table[i].live_key) {
      char quoted[oslogger::kMaxLiveKey + 8];
      std::snprintf(quoted, sizeof(quoted), "\"%s\":null", table[i].live_key);
      assert(std::strstr(json, quoted));
    }
  return used;
}
} // namespace livewidest
