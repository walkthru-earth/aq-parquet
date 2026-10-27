#pragma once

// Small bounded text helpers shared by the upload modules (#81). Internal to
// the component, nothing here allocates.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace opensensor::upload::text {

inline constexpr std::array<char, 16> kLowerHex = {'0', '1', '2', '3', '4', '5',
                                                   '6', '7', '8', '9', 'a', 'b',
                                                   'c', 'd', 'e', 'f'};
inline constexpr std::array<char, 16> kUpperHex = {'0', '1', '2', '3', '4', '5',
                                                   '6', '7', '8', '9', 'A', 'B',
                                                   'C', 'D', 'E', 'F'};

constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }
constexpr bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
constexpr bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
constexpr bool is_alnum(char c) {
  return is_digit(c) || is_lower(c) || is_upper(c);
}
constexpr bool is_lower_hex(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f');
}
constexpr bool is_upper_hex(char c) {
  return is_digit(c) || (c >= 'A' && c <= 'F');
}
// Printable ASCII without the space.
constexpr bool is_graph(char c) { return c > 0x20 && c < 0x7f; }
// RFC 3986 unreserved characters.
constexpr bool unreserved(char c) {
  return is_alnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

// Zeroes secrets through a volatile pointer so the store is not dropped.
template <typename T, std::size_t N> void secure_zero(std::array<T, N> &data) {
  volatile T *p = data.data();
  for (std::size_t i = 0; i < N; ++i)
    p[i] = T{};
}

// Appends into a fixed buffer. Any overflow latches `ok() == false`.
class Writer {
public:
  explicit Writer(std::span<char> out) : out_(out) {}

  void put(char c) {
    if (size_ < out_.size())
      out_[size_++] = c;
    else
      overflow_ = true;
  }
  void put(std::string_view s) {
    for (const char c : s)
      put(c);
  }
  // Writes `value` in decimal with no padding.
  void put_uint(std::uint64_t value) {
    std::array<char, 20> digits{};
    std::size_t n = 0;
    do {
      digits[n++] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value != 0);
    while (n > 0)
      put(digits[--n]);
  }
  void put_hex32(std::uint32_t value) {
    for (int shift = 28; shift >= 0; shift -= 4)
      put(kLowerHex[(value >> shift) & 0x0fU]);
  }
  // Adds a NUL that is not counted in `size()`. Returns false on overflow.
  [[nodiscard]] bool terminate() {
    if (overflow_ || size_ >= out_.size()) {
      overflow_ = true;
      return false;
    }
    out_[size_] = '\0';
    return true;
  }
  [[nodiscard]] bool ok() const { return !overflow_; }
  [[nodiscard]] std::size_t size() const { return size_; }
  [[nodiscard]] std::string_view view() const { return {out_.data(), size_}; }

private:
  std::span<char> out_;
  std::size_t size_ = 0;
  bool overflow_ = false;
};

// Left padded decimal digits filling all of `out`.
constexpr void put_digits(std::span<char> out, int value) {
  for (std::size_t i = out.size(); i > 0; --i) {
    out[i - 1] = static_cast<char>('0' + value % 10);
    value /= 10;
  }
}

// Parses exactly `text.size()` decimal digits, at most 18. Returns -1 on a
// non digit or an empty or overlong field.
constexpr std::int64_t parse_digits(std::string_view text) {
  if (text.empty() || text.size() > 18)
    return -1;
  std::int64_t value = 0;
  for (const char c : text) {
    if (!is_digit(c))
      return -1;
    value = value * 10 + (c - '0');
  }
  return value;
}

// Parses 8 lowercase or uppercase hex digits.
constexpr bool parse_hex32(std::string_view text, std::uint32_t &out) {
  if (text.size() != 8)
    return false;
  std::uint32_t value = 0;
  for (const char c : text) {
    std::uint32_t nibble = 0;
    if (is_digit(c))
      nibble = static_cast<std::uint32_t>(c - '0');
    else if (c >= 'a' && c <= 'f')
      nibble = static_cast<std::uint32_t>(c - 'a' + 10);
    else
      return false;
    value = (value << 4) | nibble;
  }
  out = value;
  return true;
}

struct Civil {
  int year;
  int month;
  int day;
};

// Howard Hinnant's civil_from_days, the inverse of utc::days_from_civil.
constexpr Civil civil_from_days(std::int64_t days) {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto doe = static_cast<std::uint32_t>(days - era * 146097);
  const std::uint32_t yoe =
      (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::uint32_t mp = (5 * doy + 2) / 153;
  const std::uint32_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::uint32_t m = mp < 10 ? mp + 3 : mp - 9;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2);
  return Civil{.year = static_cast<int>(y),
               .month = static_cast<int>(m),
               .day = static_cast<int>(d)};
}

} // namespace opensensor::upload::text
