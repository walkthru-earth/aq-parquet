#pragma once

// Renders the board LIVE key map through the shared emitter for the host
// contract tests. Every field is valid with integers 1 and floats 1, so the
// JSON names every key the board can send, in emit order, and each float shows
// its field's decimals. The Python side
// compares it with tools/fixtures/live_keys/<board>.json keyed by digest.
#include <aq_logger_live.h>
#include <cassert>
#include <cstdint>
#include <cstdio>

namespace livesample {
inline void print(const aqlogger::Field *table, std::size_t count) {
  aqlogger::Row row{};
  for (std::size_t i = 0; i < count; ++i) {
    if (table[i].type == telemetry::PhysicalType::Int64)
      row.set(i, std::int64_t{1});
    else if (table[i].type == telemetry::PhysicalType::Int32)
      row.set(i, std::int32_t{1});
    else
      row.set(i, 1.0f);
  }
  char json[ble::kMaxJson + 1];
  const auto used =
      aqlogger::format_live(table, count, row, json, sizeof(json));
  assert(used > 0);
  std::printf("%.*s\n", static_cast<int>(used), json);
}
} // namespace livesample
