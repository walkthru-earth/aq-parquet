#pragma once

// The board `opensensor` footer (#88) through the firmware's own
// oslogger::footer::build, with the Config fields the board passes to
// oslogger::begin. Include after the board telemetry_contract.h. Ids are
// placeholders, never a real station.

#include <aq_logger_footer.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

namespace footerfixture {
inline constexpr const char *kDevice = "0123456789ab";
inline constexpr const char *kStation = "00000000-0000-4000-8000-000000000000";
inline constexpr const char *kBoot = "0123456789abcdef0123456789abcdef";

// The fields of the board's oslogger::Config that the footer reads.
inline oslogger::Config board_config() {
  using namespace telemetry::contract;
  return {
      .schema_version = kSchemaVersion,
      .schema_name = kSchemaName,
      .firmware = kFirmware,
      .dictionary_version = kDictionaryVersion,
      .dictionary_uri = kDictionaryUri,
      .dictionary_sha256 = kDictionarySha256,
      .configuration_id = kConfigurationId,
      .configuration = kConfiguration,
      .time_semantics = kTimeSemantics,
      .board = kBoard,
      .descriptor = kTemplateJson,
      .descriptor_bytes = sizeof(kTemplateJson) - 1,
      .descriptor_sha256 = kDescriptorSha256,
      .template_href = kTemplateHref,
  };
}

// Builds into `out` or aborts, so a fixture never writes a file without it.
inline const char *build(std::span<char> out, std::uint32_t rotation_seconds,
                         std::size_t row_groups, bool compressed) {
  const auto length = oslogger::footer::build(
      out, board_config(),
      {.device = kDevice, .station = kStation, .boot = kBoot},
      {.rotation_seconds = rotation_seconds,
       .row_groups = row_groups,
       .compression = compressed ? "LZ4_RAW" : "UNCOMPRESSED",
       .benchmark = false});
  if (!length || *length != std::strlen(out.data())) {
    std::fprintf(stderr, "footer build failed\n");
    std::abort();
  }
  return out.data();
}

// `footer` mode, the JSON the board writes for a full one hour LZ4 file.
inline void print() {
  static char json[oslogger::footer::kMaxJsonBytes + 1];
  std::fputs(build(json, 3600, 4, true), stdout);
}
} // namespace footerfixture
