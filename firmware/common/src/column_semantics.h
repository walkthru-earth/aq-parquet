#pragma once

// Column semantics that sit next to a board's FIELD table (issue #84). Each
// board keeps a telemetry_semantics.inc beside telemetry_fields.inc with one
// SEM row per FIELD row, in the same order. The rows feed the stac-opensensor
// v0.1.0 board template that the host generator builds (#85). They are not
// part of the dict digest, which hashes only the raw telemetry_fields.inc.
//
// Everything here is constexpr and is only read by static_assert, so a board
// image gains no code or data from it. The checks are constexpr functions, not
// consteval, so the host test can also run them on broken tables.

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string_view>

namespace aq::semantics {

// ACDD coverage_content_type values allowed by the stac-opensensor profile.
// Enumerator spelling equals the published value, so `#role` in a SEM row is
// the exact JSON string.
enum class Role : std::uint8_t {
  physicalMeasurement,
  auxiliaryInformation,
  qualityInformation,
  referenceInformation,
  coordinate,
  modelResult,
};

// CF cell_methods words used by opensensor:aggregate, same spelling rule.
enum class Aggregate : std::uint8_t {
  mean,
  minimum,
  maximum,
  sum,
  point,
  mode
};

struct Column {
  std::string_view name;
  std::string_view ucum;
  Role role = Role::referenceInformation;
  std::string_view cf_standard_name;
  Aggregate aggregate = Aggregate::point;
  // Decimal text as written in the .inc, empty when no range applies.
  std::string_view plausible_min;
  std::string_view plausible_max;
  std::uint32_t settle_s = 0;
  // "" no gate, "finite" the column itself must be finite, or
  // "<column> == <integer>" for an equality gate on another column.
  std::string_view valid_when;
  // "" means the board URN urn:walkthru-earth:<board>:property:<name>.
  std::string_view property;
  std::string_view sensor_vendor;
  std::string_view sensor_model;
};

// True when both tables have the same length and the same name at every
// position. Fields is any range of rows with a `name` C string member.
template <typename Fields>
constexpr bool aligned(const Fields &fields, std::span<const Column> rows) {
  if (std::size(fields) != rows.size())
    return false;
  std::size_t i = 0;
  for (const auto &field : fields) {
    if (field.name == nullptr || std::string_view{field.name} != rows[i].name)
      return false;
    ++i;
  }
  return true;
}

// Optional sign, digits, optional fraction. Empty is not a number.
constexpr bool is_decimal(std::string_view text) {
  std::size_t i = 0;
  if (i < text.size() && text[i] == '-')
    ++i;
  std::size_t digits = 0;
  for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i)
    ++digits;
  if (i < text.size() && text[i] == '.') {
    ++i;
    std::size_t fraction = 0;
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i)
      ++fraction;
    if (fraction == 0)
      return false;
  }
  return digits > 0 && i == text.size();
}

// Only for text that passed is_decimal. Exact enough to order two bounds.
constexpr double decimal_value(std::string_view text) {
  const bool negative = !text.empty() && text.front() == '-';
  double value = 0;
  double scale = 0;
  for (std::size_t i = negative ? 1 : 0; i < text.size(); ++i) {
    if (text[i] == '.') {
      scale = 1;
      continue;
    }
    value = value * 10 + (text[i] - '0');
    scale *= 10;
  }
  if (scale > 0)
    value /= scale;
  return negative ? -value : value;
}

template <typename Fields>
constexpr bool has_column(const Fields &fields, std::string_view name) {
  for (const auto &field : fields)
    if (field.name != nullptr && std::string_view{field.name} == name)
      return true;
  return false;
}

// "" or "finite", or "<column> == <integer>" naming another column of the
// same table.
template <typename Fields>
constexpr bool valid_gate(const Fields &fields, const Column &row) {
  const auto gate = row.valid_when;
  if (gate.empty() || gate == "finite")
    return true;
  constexpr std::string_view kEquals = " == ";
  const auto at = gate.find(kEquals);
  if (at == std::string_view::npos || at == 0)
    return false;
  const auto column = gate.substr(0, at);
  const auto value = gate.substr(at + kEquals.size());
  return column != row.name && has_column(fields, column) &&
         is_decimal(value) && value.find('.') == std::string_view::npos;
}

// Per row rules from the stac-opensensor v0.1.0 profile and issue #84. Every
// column has a UCUM code. A physical measurement also has a gate. A range is
// both bounds or none, with min below max.
template <typename Fields>
constexpr bool well_formed(const Fields &fields, std::span<const Column> rows) {
  for (const auto &row : rows) {
    if (row.name.empty() || row.ucum.empty() || !valid_gate(fields, row))
      return false;
    if (row.role == Role::physicalMeasurement && row.valid_when.empty())
      return false;
    if (row.plausible_min.empty() != row.plausible_max.empty())
      return false;
    if (!row.plausible_min.empty() &&
        (!is_decimal(row.plausible_min) || !is_decimal(row.plausible_max) ||
         !(decimal_value(row.plausible_min) <
           decimal_value(row.plausible_max))))
      return false;
    if (row.sensor_vendor.empty() != row.sensor_model.empty())
      return false;
  }
  return true;
}

} // namespace aq::semantics
