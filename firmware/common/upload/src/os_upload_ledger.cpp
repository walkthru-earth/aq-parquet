#include "os_upload_ledger.h"

#include "os_upload_policy.h"
#include "os_upload_text.h"

#include <sync_codec.h>

#include <algorithm>

namespace opensensor::upload {
namespace {

std::uint32_t crc_of(std::string_view s) {
  return sync::crc32_update(0xffffffffU,
                            reinterpret_cast<const std::uint8_t *>(s.data()),
                            s.size()) ^
         0xffffffffU;
}

bool etag_char(char c) { return text::is_alnum(c) || c == '-'; }

// Splits the next space separated field off `rest`.
std::string_view next_field(std::string_view &rest) {
  const std::size_t at = rest.find(' ');
  const std::string_view field = rest.substr(0, at);
  rest =
      at == std::string_view::npos ? std::string_view{} : rest.substr(at + 1);
  return field;
}

} // namespace

std::string_view sanitize_etag(std::string_view raw) {
  if (raw.starts_with("W/"))
    raw.remove_prefix(2);
  if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
    raw = raw.substr(1, raw.size() - 2);
  if (raw.empty() || raw.size() > kMaxEtag ||
      !std::ranges::all_of(raw, etag_char))
    return "-";
  return raw;
}

std::expected<std::size_t, LedgerError>
format_ledger_line(const LedgerEntry &entry, std::span<char> out) {
  if (!safe_archive_name(entry.name))
    return std::unexpected(LedgerError::BadName);
  if (entry.etag != "-" && sanitize_etag(entry.etag) != entry.etag)
    return std::unexpected(LedgerError::BadEtag);
  text::Writer w(out);
  w.put(entry.name);
  w.put(' ');
  w.put_uint(entry.bytes);
  w.put(' ');
  w.put_hex32(entry.crc32);
  w.put(' ');
  w.put(entry.etag);
  if (!w.ok())
    return std::unexpected(LedgerError::Overflow);
  const std::uint32_t line_crc = crc_of(w.view());
  w.put(' ');
  w.put_hex32(line_crc);
  w.put('\n');
  if (!w.ok())
    return std::unexpected(LedgerError::Overflow);
  return w.size();
}

std::expected<LedgerEntry, LedgerError>
parse_ledger_line(std::string_view line) {
  if (line.size() < 10 || line.size() + 1 > kLedgerLineMax)
    return std::unexpected(LedgerError::BadLine);
  const std::size_t last = line.rfind(' ');
  if (last == std::string_view::npos || line.size() - last != 9)
    return std::unexpected(LedgerError::BadLine);
  std::uint32_t line_crc = 0;
  if (!text::parse_hex32(line.substr(last + 1), line_crc))
    return std::unexpected(LedgerError::BadLine);
  const std::string_view body = line.substr(0, last);
  if (crc_of(body) != line_crc)
    return std::unexpected(LedgerError::BadChecksum);
  std::string_view rest = body;
  LedgerEntry entry;
  entry.name = next_field(rest);
  const std::string_view bytes = next_field(rest);
  const std::string_view crc = next_field(rest);
  entry.etag = next_field(rest);
  if (!rest.empty() || !safe_archive_name(entry.name))
    return std::unexpected(LedgerError::BadLine);
  const std::int64_t size = text::parse_digits(bytes);
  if (size < 0 || !text::parse_hex32(crc, entry.crc32))
    return std::unexpected(LedgerError::BadLine);
  entry.bytes = static_cast<std::uint64_t>(size);
  if (entry.etag != "-" && sanitize_etag(entry.etag) != entry.etag)
    return std::unexpected(LedgerError::BadEtag);
  return entry;
}

std::expected<std::size_t, LedgerError> ledger_day_path(std::string_view name,
                                                        std::span<char> out) {
  if (!finalized_dated(name))
    return std::unexpected(LedgerError::BadName);
  // "thing=<id>/year=YYYY/month=MM/day=DD" is everything before the last '/'.
  const std::string_view directory = name.substr(0, name.rfind('/'));
  text::Writer w(out);
  w.put(kLedgerRoot);
  w.put(directory);
  w.put(".log");
  if (!w.terminate())
    return std::unexpected(LedgerError::Overflow);
  return w.size();
}

void LedgerScan::feed(std::span<const char> chunk) {
  for (const char c : chunk) {
    if (c == '\n') {
      line_done();
      continue;
    }
    if (size_ < line_.size())
      line_[size_++] = c;
    else
      overlong_ = true;
  }
}

void LedgerScan::finish() {
  // A final line with no newline was torn by a power cut. Ignore it.
  torn_ = size_ > 0 || overlong_;
  if (torn_)
    ++bad_;
  size_ = 0;
  overlong_ = false;
}

void LedgerScan::line_done() {
  const std::string_view line(line_.data(), size_);
  const bool overlong = overlong_;
  size_ = 0;
  overlong_ = false;
  if (line.empty() && !overlong)
    return;
  const auto entry = overlong ? std::expected<LedgerEntry, LedgerError>(
                                    std::unexpected(LedgerError::BadLine))
                              : parse_ledger_line(line);
  if (!entry) {
    ++bad_;
    return;
  }
  ++good_;
  if (entry->name == wanted_)
    found_ = true;
}

} // namespace opensensor::upload
