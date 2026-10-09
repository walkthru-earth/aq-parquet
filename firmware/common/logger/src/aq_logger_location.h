#pragma once

#include <aq_location.h>
#include <device_config.h>
#include <parquet_writer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace aqlogger {
// Captured at acquisition, copied with the queue and RAM batch. Later settings
// changes cannot relabel buffered samples or the footer of an open file.
template <typename Row> struct LocationSample : Row {
  config::LocationSettings location{};
};

// Nullable fields, in order: cell ID, actual resolution, center lat/lon e7.
// Invalid/unset snapshots produce four nulls. The public center is always
// calculated from the persisted cell, never from the incoming GPS position.
template <typename Row>
bool apply_location(Row &row, const config::LocationSettings &snapshot,
                    const std::array<std::size_t, 4> &fields) {
  for (auto index : fields)
    row.valid[index] = 0;
  aq::location::Cell published = 0;
  std::int64_t lat = 0, lon = 0;
  if (!aq::location::coarsen(snapshot.cell, snapshot.resolution, published) ||
      published != snapshot.cell ||
      !aq::location::center_e7(published, lat, lon))
    return false;
  row.counter(fields[0], static_cast<std::int64_t>(published));
  row.integer(fields[1], aq::location::resolution(published));
  row.counter(fields[2], lat);
  row.counter(fields[3], lon);
  return true;
}
struct LocationMetadata {
  char cell[aq::location::kCellTextBytes] = "unknown";
  char resolution[12] = "unknown";
  char maximum[12]{};
  std::array<telemetry::KeyValue, 7> entries{};

  explicit LocationMetadata(const config::LocationSettings &snapshot) {
    aq::location::Cell published = 0;
    if (aq::location::coarsen(snapshot.cell, snapshot.resolution, published) &&
        published == snapshot.cell &&
        aq::location::format_cell(published, cell, sizeof(cell)))
      std::snprintf(resolution, sizeof(resolution), "%d",
                    aq::location::resolution(published));
    std::snprintf(maximum, sizeof(maximum), "%u",
                  unsigned(snapshot.resolution));
    entries = {
        {{"country_iso3166_1_alpha2",
          aq::location::valid_country(snapshot.country) ? snapshot.country
                                                        : "unknown"},
         {"country_source", "owner-declared"},
         {"location_source", "owner-provisioned-h3-grid-center"},
         {"h3_cell_id", cell},
         {"h3_resolution", resolution},
         {"h3_max_resolution", maximum},
         {"h3_center_units", "1e-7 degrees"}}};
  }
};
} // namespace aqlogger
