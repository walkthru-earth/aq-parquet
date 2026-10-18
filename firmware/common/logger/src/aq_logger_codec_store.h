#pragma once

#include "parquet_writer.h"
#include <nvs.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <utility>

// Persisted Parquet page codec. One u8 key, `codec`, in the existing
// `parquet` namespace next to `station`. The stored byte is the Parquet
// CompressionCodec value the footer already carries (0 UNCOMPRESSED,
// 7 LZ4_RAW), so a reader of the raw key and a reader of a file agree.
// Nothing is written until the owner selects a codec, and nothing here ever
// erases a key or the namespace. The storage worker is the only writer.
namespace aqlogger::codec_store {
inline constexpr char kNamespace[] = "parquet";
inline constexpr char kKey[] = "codec";
// Used when nothing is stored or the stored value cannot be read. LZ4_RAW
// halves a full file on the measured CoreS3 bench, finalizes faster and the
// phone reader already decodes it, see docs/shared/parquet-small-files.md.
inline constexpr telemetry::Codec kDefault = telemetry::Codec::Lz4Raw;

enum class [[nodiscard]] Error : std::uint8_t {
  Open,    // the namespace could not be opened
  Missing, // no codec key stored yet
  Read,    // NVS returned another error
  Corrupt, // wrong type or a byte that names no supported codec
  Write,   // nvs_set_u8 failed
  Commit,  // nvs_commit failed
};

[[nodiscard]] constexpr const char *error_name(Error error) {
  using enum Error;
  switch (error) {
  case Open:
    return "open";
  case Missing:
    return "missing";
  case Read:
    return "read";
  case Corrupt:
    return "corrupt";
  case Write:
    return "write";
  case Commit:
    return "commit";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::uint8_t encode(telemetry::Codec codec) {
  return std::to_underlying(codec);
}

[[nodiscard]] constexpr std::expected<telemetry::Codec, Error>
decode(std::uint8_t raw) {
  using enum telemetry::Codec;
  if (raw == std::to_underlying(Uncompressed))
    return Uncompressed;
  if (raw == std::to_underlying(Lz4Raw))
    return Lz4Raw;
  return std::unexpected(Error::Corrupt);
}

// Reads the stored codec. It never writes, so a corrupt value stays as
// evidence until the owner selects a codec again.
[[nodiscard]] inline std::expected<telemetry::Codec, Error> load() {
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK)
    return std::unexpected(Error::Open);
  std::uint8_t raw = 0;
  const esp_err_t found = nvs_get_u8(handle, kKey, &raw);
  nvs_close(handle);
  if (found == ESP_ERR_NVS_NOT_FOUND)
    return std::unexpected(Error::Missing);
  if (found == ESP_ERR_NVS_TYPE_MISMATCH)
    return std::unexpected(Error::Corrupt);
  if (found != ESP_OK)
    return std::unexpected(Error::Read);
  return decode(raw);
}

// What the logger boots with. fallback is empty when the stored codec is used
// and otherwise names why the default was taken, so the boot line can say so.
struct Boot {
  telemetry::Codec codec;
  std::optional<Error> fallback;
};

[[nodiscard]] constexpr Boot
resolve(const std::expected<telemetry::Codec, Error> &loaded) {
  if (loaded.has_value())
    return Boot{.codec = *loaded, .fallback = std::nullopt};
  return Boot{.codec = kDefault, .fallback = loaded.error()};
}

[[nodiscard]] inline std::expected<void, Error> store(telemetry::Codec codec) {
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK)
    return std::unexpected(Error::Open);
  if (nvs_set_u8(handle, kKey, encode(codec)) != ESP_OK) {
    nvs_close(handle);
    return std::unexpected(Error::Write);
  }
  const esp_err_t committed = nvs_commit(handle);
  nvs_close(handle);
  if (committed != ESP_OK)
    return std::unexpected(Error::Commit);
  return {};
}
} // namespace aqlogger::codec_store
