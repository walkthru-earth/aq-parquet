#pragma once

#include <aq_location.h>
#include <device_config.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace oslogger {
// Captured at acquisition, copied with the queue and RAM batch. Later settings
// changes cannot relabel buffered samples of an open file.
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
  opensensor::location::Cell published = 0;
  std::int64_t lat = 0, lon = 0;
  if (!opensensor::location::coarsen(snapshot.cell, snapshot.resolution,
                                     published) ||
      published != snapshot.cell ||
      !opensensor::location::center_e7(published, lat, lon))
    return false;
  row.counter(fields[0], static_cast<std::int64_t>(published));
  row.integer(fields[1], opensensor::location::resolution(published));
  row.counter(fields[2], lat);
  row.counter(fields[3], lon);
  return true;
}
} // namespace oslogger
