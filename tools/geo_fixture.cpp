// Host fixture for the native GEOMETRY column and the `geo` footer key (#83).
//
// Usage: geo-fixture <with|none|mixed> <out.parquet>
//
// Writes sequence (INT64), location_source (INT32 as INTEGER(8, unsigned))
// and geometry (BYTE_ARRAY as GEOMETRY, 2D WKB Point) through the exact
// firmware writer in two row groups, then the `geo` key from
// oslogger::footer::build_geo over Writer::point_bounds. `with` gives three
// of every four rows a location, `none` leaves every location null. `mixed`
// writes three row groups and leaves the middle one wholly null between two
// located ones, so its chunk holds no GeospatialStatistics.
// tools/test_geoparquet.py repeats the same row rule and checks every value.
// The fixture also proves the writer refuses bad annotations and values.
#include "os_logger_footer.h"
#include "parquet_writer.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <source_location>
#include <string_view>

namespace {
void check(bool condition,
           std::source_location where = std::source_location::current()) {
  if (condition)
    return;
  std::fprintf(stderr, "geo fixture check failed at line %u\n",
               unsigned(where.line()));
  std::exit(1);
}

struct Row {
  std::int64_t sequence = 0;
  std::int32_t source = 0;
  std::int64_t point = 0;
  std::uint8_t located = 0;
};

constexpr std::size_t kGroupRows = 30;

enum class Mode { With, None, Mixed };

// The same rule as row() in tools/test_geoparquet.py.
Row row(std::int64_t n, Mode mode) {
  const bool null_group =
      mode == Mode::Mixed && n / std::int64_t{kGroupRows} == 1;
  const bool located = mode != Mode::None && !null_group && n % 4 != 0;
  const auto step = static_cast<std::int32_t>(n % 3);
  return {.sequence = n,
          .source = located ? 1 : 0,
          .point = located ? telemetry::pack_point_e7(312345678 + step * 100000,
                                                      300444000 - step * 50000)
                           : 0,
          .located = static_cast<std::uint8_t>(located)};
}

bool file_sink(void *context, const std::uint8_t *bytes, std::size_t length) {
  return std::fwrite(bytes, 1, length, static_cast<FILE *>(context)) == length;
}
bool null_sink(void *, const std::uint8_t *, std::size_t) { return true; }

std::array<Row, kGroupRows> rows{};

std::array<telemetry::Column, 3> columns() {
  constexpr auto stride = sizeof(Row);
  return {{
      {.name = "sequence",
       .type = telemetry::PhysicalType::Int64,
       .values = &rows[0].sequence,
       .stride = stride,
       .field_id = 1},
      {.name = "location_source",
       .type = telemetry::PhysicalType::Int32,
       .values = &rows[0].source,
       .stride = stride,
       .valid = &rows[0].located,
       .valid_stride = stride,
       .logical = telemetry::LogicalType::Uint8,
       .field_id = 2},
      {.name = "geometry",
       .type = telemetry::PhysicalType::ByteArray,
       .values = &rows[0].point,
       .stride = stride,
       .valid = &rows[0].located,
       .valid_stride = stride,
       .logical = telemetry::LogicalType::Geometry,
       .field_id = 3},
  }};
}

// begin() refuses a logical type on the wrong physical type, and row_group()
// refuses values outside the logical range, so no file is ever mislabeled.
void refusals() {
  static telemetry::Workspace workspace{};
  auto bad = columns();
  bad[2].type = telemetry::PhysicalType::Int64;
  telemetry::Writer writer;
  check(
      !writer.begin(null_sink, nullptr, workspace, bad.data(), bad.size()).ok);
  bad = columns();
  bad[1].type = telemetry::PhysicalType::Int64;
  check(
      !writer.begin(null_sink, nullptr, workspace, bad.data(), bad.size()).ok);
  bad = columns();
  bad[1].logical = telemetry::LogicalType::None;
  bad[1].type = telemetry::PhysicalType::ByteArray;
  check(
      !writer.begin(null_sink, nullptr, workspace, bad.data(), bad.size()).ok);

  const auto refused = [&](const Row &value) {
    rows.fill(row(1, Mode::With));
    rows[0] = value;
    auto good = columns();
    telemetry::Writer one;
    check(
        one.begin(null_sink, nullptr, workspace, good.data(), good.size()).ok);
    return !one.row_group(1).ok;
  };
  check(!refused(row(1, Mode::With)));
  check(refused({.sequence = 1, .source = 256, .point = 0, .located = 1}));
  check(refused({.sequence = 1, .source = -1, .point = 0, .located = 1}));
  check(refused({.sequence = 1,
                 .source = 1,
                 .point = telemetry::pack_point_e7(1800000001, 0),
                 .located = 1}));
  check(refused({.sequence = 1,
                 .source = 1,
                 .point = telemetry::pack_point_e7(0, -900000001),
                 .located = 1}));
  // A null row carries no value, whatever its slot holds.
  check(!refused({.sequence = 1, .source = 999, .point = -1, .located = 0}));
}
} // namespace

int main(int argc, char **argv) {
  check(argc == 3);
  const std::string_view mode = argv[1];
  check(mode == "with" || mode == "none" || mode == "mixed");
  const Mode rule = mode == "with"   ? Mode::With
                    : mode == "none" ? Mode::None
                                     : Mode::Mixed;
  const std::size_t groups = rule == Mode::Mixed ? 3 : 2;
  refusals();

  static telemetry::Workspace workspace{};
  auto layout = columns();
  FILE *file = std::fopen(argv[2], "wb");
  check(file != nullptr);
  telemetry::Writer writer;
  check(writer.begin(file_sink, file, workspace, layout.data(), layout.size())
            .ok);
  check(!writer.point_bounds(0).has_value());
  for (std::size_t group = 0; group < groups; ++group) {
    for (std::size_t i = 0; i < kGroupRows; ++i)
      rows[i] = row(static_cast<std::int64_t>(group * kGroupRows + i), rule);
    check(writer.row_group(kGroupRows, 0).ok);
  }
  const auto bounds = writer.point_bounds(2);
  check(bounds.has_value() == (rule != Mode::None));
  static char geo[oslogger::footer::kMaxGeoJsonBytes];
  check(oslogger::footer::build_geo(geo, bounds).has_value());
  const telemetry::KeyValue metadata[] = {{oslogger::footer::kGeoKey, geo}};
  check(writer.finish(metadata, 1, "geo-fixture").ok);
  check(std::fclose(file) == 0);
  std::printf("%s\n", geo);
}
