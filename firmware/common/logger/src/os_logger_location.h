#pragma once

#include <aq_location.h>
#include <device_config.h>
#include <parquet_writer.h>
#include <telemetry_logical.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace oslogger {
// Captured at acquisition, copied with the queue and RAM batch. Later settings
// changes cannot relabel buffered samples of an open file.
template <typename Row> struct LocationSample : Row {
  config::LocationSettings location{};
};

// Nullable fields, in order: cell ID, actual resolution, location_source and
// the GEOMETRY slot, a 2D point at the cell centre packed by
// telemetry::pack_point_e7. Invalid/unset snapshots produce four nulls. The
// point is always calculated from the persisted cell, never from an incoming
// GPS position.
template <typename Row>
bool apply_location(Row &row, const config::LocationSettings &snapshot,
                    const std::array<std::size_t, 4> &fields) {
  for (auto index : fields)
    row.valid[index] = 0;
  opensensor::location::Cell published = 0;
  std::int64_t lat = 0, lon = 0;
  if (!opensensor::location::coarsen(snapshot.cell, snapshot.resolution,
                                     published) ||
      published != snapshot.cell ||
      !opensensor::location::center_e7(published, lat, lon))
    return false;
  row.counter(fields[0], static_cast<std::int64_t>(published));
  row.integer(fields[1], opensensor::location::resolution(published));
  row.integer(fields[2],
              std::to_underlying(telemetry::LocationSource::Configured));
  // center_e7 keeps latitude within 90 and longitude within 180 degrees, so
  // both fit an int32.
  row.counter(fields[3],
              telemetry::pack_point_e7(static_cast<std::int32_t>(lon),
                                       static_cast<std::int32_t>(lat)));
  return true;
}
} // namespace oslogger
