#include "os_gnss.h"

#include <array>
#include <cstddef>

namespace opensensor::gnss {
namespace {
constexpr std::string_view kInfoPrefix = "+CGNSSINFO:";
constexpr std::string_view kPowerPrefix = "+CGNSSPWR:";
// The manual fix reply has 18 fields. Room for a firmware that adds a few.
constexpr std::size_t kMaxFields = 24;
constexpr std::size_t kMaxFractionDigits = 10;

constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

constexpr std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\r' ||
                           text.front() == '\n' || text.front() == '\t'))
    text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\r' ||
                           text.back() == '\n' || text.back() == '\t'))
    text.remove_suffix(1);
  return text;
}

constexpr bool all_digits(std::string_view text) {
  if (text.empty())
    return false;
  for (char c : text)
    if (!is_digit(c))
      return false;
  return true;
}

// The shape of ddmm.mmmm or dddmm.mmmm, before any range check.
constexpr bool coordinate_shaped(std::string_view text) {
  const auto dot = text.find('.');
  return dot != std::string_view::npos && all_digits(text.substr(0, dot)) &&
         all_digits(text.substr(dot + 1));
}

// Degrees and minutes as SIMCom prints them, degrees * 100 + minutes. The
// integer part has at most `integer_digits` digits (4 for latitude, 5 for
// longitude). Parsed by hand, no locale and no strtod.
std::expected<double, ParseError> degrees_minutes(std::string_view text,
                                                  std::size_t integer_digits,
                                                  int max_degrees) {
  const auto dot = text.find('.');
  if (dot == std::string_view::npos)
    return std::unexpected(ParseError::Malformed);
  const auto whole = text.substr(0, dot);
  const auto fraction = text.substr(dot + 1);
  if (!all_digits(whole) || whole.size() > integer_digits ||
      !all_digits(fraction) || fraction.size() > kMaxFractionDigits)
    return std::unexpected(ParseError::Malformed);
  std::int64_t integer = 0;
  for (char c : whole)
    integer = integer * 10 + (c - '0');
  std::int64_t numerator = 0, denominator = 1;
  for (char c : fraction) {
    numerator = numerator * 10 + (c - '0');
    denominator *= 10;
  }
  const auto degrees = integer / 100;
  const double minutes =
      static_cast<double>(integer % 100) +
      static_cast<double>(numerator) / static_cast<double>(denominator);
  if (minutes >= 60 || degrees > max_degrees ||
      (degrees == max_degrees && minutes > 0))
    return std::unexpected(ParseError::OutOfRange);
  return static_cast<double>(degrees) + minutes / 60;
}

constexpr int hemisphere(std::string_view text, char positive, char negative) {
  if (text.size() != 1)
    return 0;
  return text[0] == positive ? 1 : text[0] == negative ? -1 : 0;
}

// Satellites in use, the per system counts before the latitude. -1 unknown.
std::int16_t satellites(const std::array<std::string_view, kMaxFields> &fields,
                        std::size_t latitude_index) {
  if (latitude_index != 5)
    return -1;
  int total = 0;
  bool any = false;
  for (std::size_t i = 1; i < 5; ++i) {
    if (fields[i].empty())
      continue;
    if (!all_digits(fields[i]) || fields[i].size() > 3)
      return -1;
    any = true;
    int count = 0;
    for (char c : fields[i])
      count = count * 10 + (c - '0');
    total += count;
  }
  return any ? static_cast<std::int16_t>(total) : std::int16_t{-1};
}
} // namespace

std::expected<bool, ParseError>
SimcomDialect::parse_power(std::string_view line) const {
  line = trim(line);
  if (!line.starts_with(kPowerPrefix))
    return std::unexpected(ParseError::NotThisLine);
  const auto value = trim(line.substr(kPowerPrefix.size()));
  if (value == "1" || value == "READY!")
    return true;
  if (value == "0")
    return false;
  return std::unexpected(ParseError::Malformed);
}

std::expected<Reading, ParseError>
SimcomDialect::parse_fix(std::string_view line) const {
  line = trim(line);
  if (!line.starts_with(kInfoPrefix))
    return std::unexpected(ParseError::NotThisLine);
  const auto body = trim(line.substr(kInfoPrefix.size()));
  std::array<std::string_view, kMaxFields> fields{};
  std::size_t count = 0;
  bool any = false;
  for (std::size_t start = 0;;) {
    if (count == kMaxFields)
      return std::unexpected(ParseError::Malformed);
    const auto comma = body.find(',', start);
    const auto field = trim(body.substr(
        start, comma == std::string_view::npos ? comma : comma - start));
    any = any || !field.empty();
    fields[count++] = field;
    if (comma == std::string_view::npos)
      break;
    start = comma + 1;
  }
  // V1.9.05 answers ",,,,,,,," and the manual 18 empty fields.
  if (!any)
    return std::unexpected(ParseError::NoFix);
  std::size_t at = count;
  for (std::size_t i = 0; i + 3 < count; ++i)
    if (coordinate_shaped(fields[i]) && hemisphere(fields[i + 1], 'N', 'S') &&
        coordinate_shaped(fields[i + 2]) &&
        hemisphere(fields[i + 3], 'E', 'W')) {
      at = i;
      break;
    }
  if (at == count)
    return std::unexpected(ParseError::Malformed);
  Reading reading{};
  if (at > 0) {
    // A leading field, when present, is the fix mode, 2 for 2D or 3 for 3D.
    if (fields[0] != "2" && fields[0] != "3")
      return std::unexpected(ParseError::Malformed);
    reading.mode = static_cast<std::uint8_t>(fields[0][0] - '0');
  }
  const auto latitude = degrees_minutes(fields[at], 4, 90);
  if (!latitude)
    return std::unexpected(latitude.error());
  const auto longitude = degrees_minutes(fields[at + 2], 5, 180);
  if (!longitude)
    return std::unexpected(longitude.error());
  // Some receivers print zeros before the first fix. Null Island is no fix.
  if (*latitude == 0 && *longitude == 0)
    return std::unexpected(ParseError::NoFix);
  reading.latitude = *latitude * hemisphere(fields[at + 1], 'N', 'S');
  reading.longitude = *longitude * hemisphere(fields[at + 3], 'E', 'W');
  reading.satellites = satellites(fields, at);
  return reading;
}

const Dialect &simcom_dialect() {
  static const SimcomDialect dialect;
  return dialect;
}

std::expected<Fix, ParseError> to_fix(const Reading &reading, int resolution,
                                      std::int64_t fix_mono_us) {
  location::Cell cell = 0;
  if (!location::from_degrees(reading.latitude, reading.longitude, resolution,
                              cell))
    return std::unexpected(ParseError::OutOfRange);
  return Fix{.cell = cell,
             .resolution =
                 static_cast<std::uint8_t>(location::resolution(cell)),
             .fix_mono_us = fix_mono_us,
             .mode = reading.mode,
             .satellites = reading.satellites};
}

bool fresh(const Fix &fix, std::int64_t now_us) {
  if (fix.cell == 0 || fix.fix_mono_us <= 0)
    return false;
  const auto age = now_us - fix.fix_mono_us;
  return age <= kFixMaxAgeUs && age >= -kFixClockSkewUs;
}

Selection select_location(const config::LocationSettings &row,
                          const std::optional<Fix> &fix, std::int64_t now_us) {
  location::Cell published = 0;
  if (row.mobility == config::Mobility::Fixed) {
    if (location::coarsen(row.cell, row.resolution, published) &&
        published == row.cell)
      return {.cell = published,
              .source = telemetry::LocationSource::Configured};
    return {};
  }
  if (fix && fresh(*fix, now_us) &&
      location::coarsen(fix->cell, row.resolution, published))
    return {.cell = published, .source = telemetry::LocationSource::Gnss};
  return {};
}
} // namespace opensensor::gnss
