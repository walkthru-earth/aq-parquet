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
  // Seconds after the sensor wakes, not after boot, so a duty cycled sensor
  // (#39) settles again on every wake.
  std::uint32_t settle_s = 0;
  // "" no gate, "finite" the column itself must be finite, or
  // "<column> == <integer>" for an equality gate on another column. A row
  // that passes its gate still counts as settling until settle_s has passed
  // since the sensor woke.
  std::string_view valid_when;
  // Board neutral property id, the single source of the FIELD property_uri.
  // See board_neutral_property for the allowed forms.
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

// [a-z][a-z0-9_]*, the quantity part of a walkthru-earth property URN.
constexpr bool is_snake_case(std::string_view text) {
  if (text.empty() || text.front() < 'a' || text.front() > 'z')
    return false;
  for (const char c : text)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
      return false;
  return true;
}

// Board, chip and vendor names that belong in opensensor:procedure, never in
// a property id. A substring deny list, so it is best effort. Short stems
// like bmi and ltr also catch spellings like bmi_270. Keep the list in
// tools/test_telemetry_contract.py and tools/test_waveshare_contract.py the
// same.
inline constexpr std::string_view kProcedureWords[] = {
    "cores3",    "waveshare", "sim76",     "simcom",    "m5",       "esp32",
    "espressif", "pms",       "plantower", "bmi",       "bmm",      "bosch",
    "ltr",       "lite_on",   "liteon",    "axp",       "x_powers", "xpowers",
    "bm8563",    "belling",   "ft6336",    "focaltech", "max1704",  "maxim",
    "sht",       "sensirion"};

// The stac-opensensor property rule. A NERC P07 term, else the NERC CF
// standard_name URI for an exact CF match, else
// urn:walkthru-earth:property:<quantity>. Never a board or sensor name, so
// the same quantity has the same id on every board.
constexpr bool board_neutral_property(std::string_view property) {
  constexpr std::string_view kP07 =
      "http://vocab.nerc.ac.uk/collection/P07/current/";
  constexpr std::string_view kStandardName =
      "http://vocab.nerc.ac.uk/standard_name/";
  constexpr std::string_view kUrn = "urn:walkthru-earth:property:";
  for (const auto word : kProcedureWords)
    if (property.find(word) != std::string_view::npos)
      return false;
  if (property.starts_with(kUrn))
    return is_snake_case(property.substr(kUrn.size()));
  if (!property.ends_with('/'))
    return false;
  if (property.starts_with(kStandardName))
    return is_snake_case(property.substr(
        kStandardName.size(), property.size() - kStandardName.size() - 1));
  if (!property.starts_with(kP07) || property.size() <= kP07.size() + 1)
    return false;
  for (const char c :
       property.substr(kP07.size(), property.size() - kP07.size() - 1))
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
      return false;
  return true;
}

// The property of the row called `name`, as a C string for the FIELD table,
// or nullptr when no row has that name. Every property is a string literal
// in the .inc, so data() is null terminated.
constexpr const char *property_of(std::span<const Column> rows,
                                  std::string_view name) {
  for (const auto &row : rows)
    if (row.name == name)
      return row.property.data();
  return nullptr;
}

// True when every FIELD row got a property from property_of, so a lookup miss
// fails the build instead of leaving a null property_uri.
template <typename Fields>
constexpr bool every_property_set(const Fields &fields) {
  for (const auto &field : fields)
    if (field.property_uri == nullptr || *field.property_uri == '\0')
      return false;
  return true;
}

// Per row rules from the stac-opensensor v0.1.0 profile and issue #84. Every
// column has a UCUM code and a board neutral property id, unique in the
// table. A physical measurement also has a gate. A range is both bounds or
// none, with min below max.
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
    if (!board_neutral_property(row.property))
      return false;
  }
  for (std::size_t i = 0; i < rows.size(); ++i)
    for (std::size_t j = i + 1; j < rows.size(); ++j)
      if (rows[i].property == rows[j].property)
        return false;
  return true;
}

} // namespace aq::semantics
