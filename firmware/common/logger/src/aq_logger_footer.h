#pragma once

#include "aq_logger.h"
#include "aq_logger_cadence.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <parquet_writer.h>
#include <span>

// The fixed board neutral part of every telemetry footer, in file order.
// finalize_file appends location, anchor and board metadata after it. Pure,
// so a host test checks the exact keys and values a board writes.
namespace oslogger::footer {
inline constexpr std::size_t kFixedEntries = 22;

struct Ids {
  const char *device = nullptr;
  const char *station = nullptr;
  const char *boot = nullptr;
};
struct File {
  std::uint32_t rotation_seconds = 0;
  std::size_t row_groups = 0;
  const char *compression = nullptr;
  bool benchmark = false;
};
// Numbers formatted for the footer. Must outlive the written entries.
struct Text {
  std::array<char, 12> rotation{};
  std::array<char, 12> row_groups{};
  std::array<char, 12> rows_max{};
  std::array<char, 12> sample_interval{};
};

// Writes the fixed entries into `out`, which a caller sizes for the whole
// footer. Text must outlive the entries.
inline void fill(std::span<telemetry::KeyValue, kFixedEntries> out,
                 const Config &config, const Ids &ids, const File &file,
                 Text &text) {
  std::snprintf(text.rotation.data(), text.rotation.size(), "%lu",
                static_cast<unsigned long>(file.rotation_seconds));
  std::snprintf(text.row_groups.data(), text.row_groups.size(), "%u",
                unsigned(file.row_groups));
  std::snprintf(text.rows_max.data(), text.rows_max.size(), "%u",
                unsigned(cadence::kMaxBatchRows));
  std::snprintf(text.sample_interval.data(), text.sample_interval.size(), "%lu",
                static_cast<unsigned long>(config.sample_interval_ms));
  std::size_t next = 0;
  const auto put = [&](const char *key, const char *value) {
    out[next++] = {key, value};
  };
  put("schema_version", config.schema_name);
  put("device_id", ids.device);
  put("station_id", ids.station);
  put("boot_id", ids.boot);
  put("firmware", config.firmware);
  put("dictionary_version", config.dictionary_version);
  put("dictionary_uri", config.dictionary_uri);
  put("dictionary_sha256", config.dictionary_sha256);
  put("acquisition_config_id", config.configuration_id);
  put("acquisition_config", config.configuration);
  put("deployment_id", config.deployment_id);
  put("calibration_id", config.calibration_id);
  put("time_semantics", config.time_semantics);
  put("rotation_interval_s", text.rotation.data());
  put("row_groups", text.row_groups.data());
  put("row_group_rows_max", text.rows_max.data());
  put("compression", file.compression);
  put("purpose",
      file.benchmark ? "codec-comparison-duplicate-rows" : "telemetry");
  put("board", config.board);
  put("sample_interval_ms", text.sample_interval.data());
  put("clock", "0=unsynchronized/null,1=host estimate,2=restored from RTC "
               "(earlier external estimate, whole seconds),3=network SNTP; "
               "partitions UTC");
  put("durability",
      "RAM batch; unfinished rows lost on reset; each row group "
      "fsynced; footer at finalization; completed files retained");
}
} // namespace oslogger::footer
