#pragma once

#include "aq_logger.h"
#include "aq_logger_cadence.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

// The one Parquet key-value entry of every telemetry file, `opensensor`, whose
// value is compact UTF-8 JSON per stac-opensensor json-schema/footer.json
// v0.1.0 (a56dfb0), pinned in tools/stac_opensensor/v0.1.0/footer.json.
// Members follow the schema's property order, which matches its example
// footers/cores3-telemetry-v5.json. Numbers are JSON numbers. Pure and bounded,
// so a host test builds the exact bytes a board writes and validates them.
namespace oslogger::footer {
inline constexpr const char *kKey = "opensensor";
inline constexpr std::string_view kVersion = "0.1.0";
// Longest value telemetry::Writer::finish accepts, NUL not counted. A buffer
// of kMaxJsonBytes + 1 holds any footer the writer can store.
inline constexpr std::size_t kMaxJsonBytes = 4096;
// footer.json caps file.rotation_interval_s at one hour.
inline constexpr std::uint32_t kMaxRotationSeconds = 3600;

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
enum class Error : std::uint8_t {
  // A required string is null or empty.
  Missing,
  // A number is outside the footer schema range.
  Range,
  // The JSON and its NUL do not fit the buffer.
  Overflow,
  // dictionary.sha256 or template.sha256 is not 64 lowercase hex digits.
  Digest,
};
[[nodiscard]] constexpr const char *error_name(Error error) {
  switch (error) {
  case Error::Missing:
    return "footer-missing-field";
  case Error::Range:
    return "footer-out-of-range";
  case Error::Overflow:
    return "footer-overflow";
  case Error::Digest:
    return "footer-bad-sha256";
  }
  return "footer";
}

namespace detail {
// Appends to a fixed buffer and remembers the first overflow. No heap, no
// formatting library.
class Json {
public:
  explicit Json(std::span<char> out) : out_(out) {}

  void open(std::string_view key) {
    name(key);
    put('{');
    fresh_ = true;
  }
  void close() {
    put('}');
    fresh_ = false;
  }
  void text(std::string_view key, std::string_view value) {
    name(key);
    string(value);
  }
  void number(std::string_view key, std::uint64_t value) {
    name(key);
    char digits[20];
    const auto [end, error] =
        std::to_chars(digits, digits + sizeof(digits), value);
    if (error != std::errc{}) {
      ok_ = false;
      return;
    }
    append({digits, static_cast<std::size_t>(end - digits)});
  }
  void begin() {
    put('{');
    fresh_ = true;
  }
  [[nodiscard]] std::expected<std::size_t, Error> finish() {
    if (!ok_ || used_ >= out_.size())
      return std::unexpected(Error::Overflow);
    out_[used_] = '\0';
    return used_;
  }

private:
  std::span<char> out_;
  std::size_t used_ = 0;
  bool ok_ = true;
  bool fresh_ = true;

  void put(char c) {
    if (used_ >= out_.size()) {
      ok_ = false;
      return;
    }
    out_[used_++] = c;
  }
  void append(std::string_view bytes) {
    for (const char c : bytes)
      put(c);
  }
  void name(std::string_view key) {
    if (!fresh_)
      put(',');
    fresh_ = false;
    string(key);
    put(':');
  }
  // RFC 8259 escapes, the two character form where one exists. Bytes from
  // 0x80 pass through, so UTF-8 stays UTF-8.
  void string(std::string_view value) {
    static constexpr char kHex[] = "0123456789abcdef";
    put('"');
    for (const char c : value) {
      const auto byte = static_cast<unsigned char>(c);
      const char shorthand = c == '"'    ? '"'
                             : c == '\\' ? '\\'
                             : c == '\b' ? 'b'
                             : c == '\f' ? 'f'
                             : c == '\n' ? 'n'
                             : c == '\r' ? 'r'
                             : c == '\t' ? 't'
                                         : '\0';
      if (shorthand != '\0') {
        put('\\');
        put(shorthand);
      } else if (byte < 0x20) {
        append("\\u00");
        put(kHex[byte >> 4]);
        put(kHex[byte & 0x0f]);
      } else {
        put(c);
      }
    }
    put('"');
  }
};

[[nodiscard]] constexpr bool present(const char *value) {
  return value != nullptr && value[0] != '\0';
}
// The footer.json sha256 pattern, ^[0-9a-f]{64}$.
[[nodiscard]] constexpr bool sha256_hex(const char *value) {
  std::size_t length = 0;
  for (; value[length] != '\0'; ++length) {
    const char c = value[length];
    if (length == 64 || !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  }
  return length == 64;
}
} // namespace detail

// The board constants the footer needs, checked once by oslogger::begin() so a
// bad constant stops logging at boot rather than quarantining every file.
// Every string the schema requires, and every optional one this firmware
// writes, must be non empty except dictionary.version, acquisition.config_id,
// config and calibration_id, which the schema allows empty but must not be
// null. dictionary.uri and template.href must be non empty because format uri
// rejects "", and both digests must be 64 lowercase hex digits.
[[nodiscard]] constexpr std::expected<void, Error>
check_config(const Config &config) {
  using detail::present;
  if (!present(config.board) || !present(config.deployment_id) ||
      !present(config.firmware) || !present(config.schema_name) ||
      !present(config.dictionary_sha256) || !config.dictionary_version ||
      !present(config.dictionary_uri) || !present(config.template_href) ||
      !present(config.descriptor_sha256) || !present(config.time_semantics) ||
      !config.configuration_id || !config.configuration ||
      !config.calibration_id)
    return std::unexpected(Error::Missing);
  if (!detail::sha256_hex(config.dictionary_sha256) ||
      !detail::sha256_hex(config.descriptor_sha256))
    return std::unexpected(Error::Digest);
  if (config.sample_interval_ms == 0)
    return std::unexpected(Error::Range);
  return {};
}

// Writes the footer JSON and a NUL into `out` and returns its length without
// the NUL. Fails closed, never truncates. check_config() rules apply, and the
// ids and file.compression must be non empty.
[[nodiscard]] inline std::expected<std::size_t, Error>
build(std::span<char> out, const Config &config, const Ids &ids,
      const File &file) {
  using detail::present;
  if (const auto checked = check_config(config); !checked)
    return std::unexpected(checked.error());
  if (!present(ids.station) || !present(ids.device) || !present(ids.boot) ||
      !present(file.compression))
    return std::unexpected(Error::Missing);
  if (file.rotation_seconds == 0 ||
      file.rotation_seconds > kMaxRotationSeconds || file.row_groups == 0)
    return std::unexpected(Error::Range);
  detail::Json json(out);
  json.begin();
  json.text("version", kVersion);
  json.open("station");
  json.text("id", ids.station);
  json.text("device_id", ids.device);
  json.text("board", config.board);
  json.text("deployment_id", config.deployment_id);
  json.close();
  json.open("firmware");
  json.text("version", config.firmware);
  json.text("schema", config.schema_name);
  json.text("boot_id", ids.boot);
  json.close();
  json.open("dictionary");
  json.text("sha256", config.dictionary_sha256);
  json.text("version", config.dictionary_version);
  json.text("uri", config.dictionary_uri);
  json.close();
  json.open("template");
  json.text("href", config.template_href);
  json.text("sha256", config.descriptor_sha256);
  json.close();
  json.open("acquisition");
  json.number("sample_interval_ms", config.sample_interval_ms);
  json.text("time_semantics", config.time_semantics);
  json.text("config_id", config.configuration_id);
  json.text("config", config.configuration);
  json.text("calibration_id", config.calibration_id);
  json.close();
  json.open("file");
  json.number("rotation_interval_s", file.rotation_seconds);
  json.number("row_groups", file.row_groups);
  json.number("row_group_rows_max", cadence::kMaxBatchRows);
  json.text("compression", file.compression);
  json.text("purpose",
            file.benchmark ? "codec-comparison-duplicate-rows" : "telemetry");
  json.close();
  json.close();
  return json.finish();
}
} // namespace oslogger::footer
