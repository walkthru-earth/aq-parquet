#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace aq {
// Zero-initialize each acquired row. A validity byte of zero is a null; the
// trial owns column names, physical types, units and acquisition semantics.
template <std::size_t Columns> struct NumericSample {
  std::array<std::int64_t, Columns> data{};
  std::array<std::uint8_t, Columns> valid{};

  template <typename T> void set(std::size_t field, T value) {
    static_assert(sizeof(T) <= sizeof(std::int64_t), "numeric field too large");
    std::memcpy(&data[field], &value, sizeof(value));
    valid[field] = 1;
  }
  void integer(std::size_t field, std::int32_t value) { set(field, value); }
  void counter(std::size_t field, std::int64_t value) { set(field, value); }
  void number(std::size_t field, float value) {
    if (std::isfinite(value))
      set(field, value);
  }
};
} // namespace aq
