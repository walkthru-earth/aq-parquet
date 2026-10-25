#pragma once

// Widest real LIVE row for the board contract tests (#91). Each board lists,
// for every LIVE key, the widest value its producer can write while the key
// is valid. The helper fills a row from that list, checks every key is
// printed (none dropped as out of range) and returns the object size. A new
// LIVE key without an entry, or an entry for a key the board does not have,
// fails the test, so whoever adds a key must state its widest value.
#include <aq_logger_live.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

namespace livewidest {
struct Widest {
  const char *key;
  std::int64_t integer = 0; // INT32 and INT64 keys, stored units
  float number = 0.0F;      // FLOAT keys
};

inline std::size_t measure(const aqlogger::Field *table, std::size_t count,
                           std::span<const Widest> widest) {
  assert(aqlogger::valid_live_keys(table, count));
  aqlogger::Row row{};
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
    }
  }
  assert(keys == widest.size() && "a widest entry names no LIVE key");
  char json[ble::kMaxJson + 1];
  const auto used =
      aqlogger::format_live(table, count, row, json, sizeof(json));
  const auto needed = aqlogger::live_size(table, count, row);
  assert(used == 0 || used == needed);
  // Every key must be printed, so nothing hides as an omitted value.
  if (used)
    for (const auto &entry : widest) {
      char quoted[aqlogger::kMaxLiveKey + 4];
      std::snprintf(quoted, sizeof(quoted), "\"%s\":", entry.key);
      assert(std::strstr(json, quoted));
    }
  return needed;
}
} // namespace livewidest
