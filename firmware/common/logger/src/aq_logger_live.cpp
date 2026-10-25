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

bool usable_table(const Field *fields, std::size_t count) {
  return (fields || count == 0) && count <= telemetry::kMaxColumns;
}

struct Rendered {
  std::size_t used = 0;   // bytes written to `out`, without the NUL
  std::size_t needed = 0; // bytes the whole object needs, without the NUL
  bool fits = true;
};

// Writes the LIVE object into `out` while it fits and always counts the bytes
// the whole object needs, so an overflow can report its real size. A null
// `out` only counts.
Rendered render(const Field *fields, std::size_t count, const Row &row,
                char *out, std::size_t capacity) {
  Rendered result;
  result.fits = out && capacity > 0;
  // Appends literal text. A value that would not fit marks the whole document
  // as failed instead of publishing a truncated object.
  const auto append = [&](const char *text) {
    const auto length = std::strlen(text);
    result.needed += length;
    if (!result.fits || length >= capacity - result.used) {
      result.fits = false;
      return;
    }
    std::memcpy(out + result.used, text, length + 1);
    result.used += length;
  };
  append("{");
  bool first = true;
  for (std::size_t i = 0; i < count; ++i) {
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
  return result;
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
  if (!out || capacity == 0 || !usable_table(fields, count))
    return 0;
  const auto result = render(fields, count, row, out, capacity);
  if (!result.fits) {
    out[0] = '\0';
    return 0;
  }
  return result.used;
}

std::size_t live_size(const Field *fields, std::size_t count, const Row &row) {
  if (!usable_table(fields, count))
    return 0;
  return render(fields, count, row, nullptr, 0).needed;
}

std::size_t format_live_overflow(char *out, std::size_t capacity,
                                 std::size_t needed, std::size_t max) {
  if (!out || capacity == 0)
    return 0;
  const int written = std::snprintf(
      out, capacity, "PARQUET LIVE overflow bytes=%lu max=%lu\n",
      static_cast<unsigned long>(needed), static_cast<unsigned long>(max));
  if (written < 0 || std::size_t(written) >= capacity) {
    out[0] = '\0';
    return 0;
  }
  return std::size_t(written);
}

LivePublish LiveOverflow::publish(const Field *fields, std::size_t count,
                                  const Row &row, char *out,
                                  std::size_t capacity, std::int64_t now_us) {
  LivePublish result;
  if (!out || capacity == 0 || !usable_table(fields, count))
    return result;
  result.length = format_live(fields, count, row, out, capacity);
  if (result.length)
    return result;
  // The object does not fit. Publish `{}` so no reader keeps the previous
  // sample as current, and log at most once per kRepeatUs.
  result.needed = live_size(fields, count, row);
  if (capacity > 2) {
    std::memcpy(out, "{}", 3);
    result.length = 2;
  }
  if (!logged_ || now_us - last_log_us_ >= kRepeatUs) {
    logged_ = true;
    last_log_us_ = now_us;
    result.log = true;
  }
  return result;
}
} // namespace aqlogger
