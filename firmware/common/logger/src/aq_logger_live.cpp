#include "aq_logger_live.h"

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace aqlogger {
namespace {
bool key_chars(const char *key) {
  const auto length = std::strlen(key);
  if (length == 0 || length > kMaxLiveKey)
    return false;
  for (const char *p = key; *p; ++p)
    if (!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9') && *p != '_')
      return false;
  return true;
}
} // namespace

bool valid_live_keys(const Field *fields, std::size_t count) {
  if (count && !fields)
    return false;
  for (std::size_t i = 0; i < count; ++i) {
    const char *key = fields[i].live_key;
    if (!key)
      continue;
    if (!key_chars(key))
      return false;
    for (std::size_t j = 0; j < i; ++j)
      if (fields[j].live_key && std::strcmp(fields[j].live_key, key) == 0)
        return false;
  }
  return true;
}

std::size_t format_live(const Field *fields, std::size_t count, const Row &row,
                        char *out, std::size_t capacity) {
  if (!out || capacity == 0 || (count && !fields) ||
      count > telemetry::kMaxColumns)
    return 0;
  std::size_t used = 0;
  bool fits = true;
  // Appends literal text; a value that would not fit marks the whole document
  // as failed instead of publishing a truncated object.
  const auto append = [&](const char *text) {
    const auto length = std::strlen(text);
    if (!fits || length >= capacity - used) {
      fits = false;
      return;
    }
    std::memcpy(out + used, text, length + 1);
    used += length;
  };
  append("{");
  bool first = true;
  for (std::size_t i = 0; i < count && fits; ++i) {
    const auto &field = fields[i];
    if (!field.live_key || !row.valid[i])
      continue;
    char value[32];
    int written = -1;
    switch (field.type) {
    case telemetry::PhysicalType::Int64: {
      // UTC instants are stored in microseconds but LIVE keeps the wire unit
      // of nanoseconds. The stored value is exact, so the scale is lossless.
      const long long scale =
          field.unit && std::strcmp(field.unit, "us_since_unix_epoch") == 0
              ? 1000
              : 1;
      const long long stored = static_cast<long long>(row.data[i]);
      if (stored > LLONG_MAX / scale || stored < LLONG_MIN / scale)
        break; // out of range for the wire unit, omit like a null
      written = std::snprintf(value, sizeof(value), "%lld", stored * scale);
      break;
    }
    case telemetry::PhysicalType::Int32: {
      std::int32_t number;
      std::memcpy(&number, &row.data[i], sizeof(number));
      written =
          std::snprintf(value, sizeof(value), "%ld", static_cast<long>(number));
      break;
    }
    case telemetry::PhysicalType::Float: {
      float number;
      std::memcpy(&number, &row.data[i], sizeof(number));
      if (std::isfinite(number))
        written = std::snprintf(value, sizeof(value), "%.1f",
                                static_cast<double>(number));
      break;
    }
    }
    // Non-finite floats and values wider than the scratch buffer are omitted.
    if (written < 0 || std::size_t(written) >= sizeof(value))
      continue;
    append(first ? "\"" : ",\"");
    append(field.live_key);
    append("\":");
    append(value);
    first = false;
  }
  append("}");
  if (!fits) {
    out[0] = '\0';
    return 0;
  }
  return used;
}
} // namespace aqlogger
