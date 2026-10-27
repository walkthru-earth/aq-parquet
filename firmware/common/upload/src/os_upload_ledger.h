#pragma once

// The append only upload ledger (#81). One text line per uploaded archive
// file, kept on SD under upload/ledger/, outside output/, so sync never
// lists it. The archive itself is never deleted, renamed or relabelled
// because of an upload. A re-run reads the ledger and uploads nothing new.
//
// Line format, one space between fields
//   <name> <bytes> <crc32 8 hex> <etag or -> <line crc32 8 hex>\n
// The line crc covers everything before its own leading space, so a torn or
// bit flipped line is skipped instead of trusted.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace opensensor::upload {

inline constexpr std::size_t kMaxEtag = 64;
inline constexpr std::size_t kLedgerLineMax =
    384 + 1 + 20 + 1 + 8 + 1 + kMaxEtag + 1 + 8 + 1;
inline constexpr std::string_view kLedgerRoot = "upload/ledger/";

struct LedgerEntry {
  std::string_view name;
  std::uint64_t bytes = 0;
  std::uint32_t crc32 = 0;
  // "-" when the server sent none or one we do not accept.
  std::string_view etag;
};

enum class LedgerError : std::uint8_t {
  BadName,
  BadEtag,
  Overflow,
  BadLine,
  BadChecksum,
};

// Quotes stripped, [0-9A-Za-z-] only, at most 64 bytes. Anything else is "-".
[[nodiscard]] std::string_view sanitize_etag(std::string_view raw);

[[nodiscard]] std::expected<std::size_t, LedgerError>
format_ledger_line(const LedgerEntry &entry, std::span<char> out);

// `line` excludes the newline. Views in the result point into `line`.
[[nodiscard]] std::expected<LedgerEntry, LedgerError>
parse_ledger_line(std::string_view line);

// "upload/ledger/thing=<id>/year=YYYY/month=MM/day=DD.log" for a dated
// archive name, so a ledger file stays small and is found by name.
[[nodiscard]] std::expected<std::size_t, LedgerError>
ledger_day_path(std::string_view name, std::span<char> out);

// Streams a ledger file in chunks of any size and answers whether one name
// is recorded. Bad lines are counted and skipped. A final line without its
// newline is torn and ignored.
class LedgerScan {
public:
  explicit LedgerScan(std::string_view wanted) : wanted_(wanted) {}

  void feed(std::span<const char> chunk);
  // Call at end of file.
  void finish();

  [[nodiscard]] bool found() const { return found_; }
  [[nodiscard]] std::uint32_t bad_lines() const { return bad_; }
  [[nodiscard]] std::uint32_t good_lines() const { return good_; }
  // The file ended without a newline. The appender writes one first, so
  // the next entry never joins the torn bytes.
  [[nodiscard]] bool torn_tail() const { return torn_; }

private:
  void line_done();

  std::string_view wanted_;
  std::array<char, kLedgerLineMax> line_{};
  std::size_t size_ = 0;
  bool overlong_ = false;
  bool found_ = false;
  bool torn_ = false;
  std::uint32_t bad_ = 0;
  std::uint32_t good_ = 0;
};

} // namespace opensensor::upload
