#include "coredump_report.h"

#include <cstdio>
#include <utility>

namespace aq::coredump {
namespace {

// Room for the longest field text plus its NUL.
using Field = std::array<char, 20>;

[[nodiscard]] bool safe_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

// Copies a borrowed, possibly unterminated name and keeps only safe bytes, so
// the line stays one space separated key=value record.
template <std::size_t N>
[[nodiscard]] Field clean(const std::array<char, N> &in) {
  static_assert(N < Field{}.size());
  Field out{};
  std::size_t used = 0;
  for (const char c : in) {
    if (c == '\0')
      break;
    out[used++] = safe_char(c) ? c : '_';
  }
  if (used == 0)
    out[used++] = '-';
  out[used] = '\0';
  return out;
}

[[nodiscard]] Field hex32(std::uint32_t value) {
  Field out{};
  std::snprintf(out.data(), out.size(), "0x%08lx",
                static_cast<unsigned long>(value));
  return out;
}

[[nodiscard]] Field decimal(std::uint32_t value) {
  Field out{};
  std::snprintf(out.data(), out.size(), "%lu",
                static_cast<unsigned long>(value));
  return out;
}

constexpr Field kNone{'-', '\0'};

// First eight checksum bytes as hex, enough to tell dumps apart in a log.
[[nodiscard]] Field checksum_prefix(const Identity &identity) {
  static constexpr std::array<char, 16> kHex{'0', '1', '2', '3', '4', '5',
                                             '6', '7', '8', '9', 'a', 'b',
                                             'c', 'd', 'e', 'f'};
  Field out{};
  std::size_t used = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    out[used++] = kHex[identity.checksum[i] >> 4];
    out[used++] = kHex[identity.checksum[i] & 0x0f];
  }
  out[used] = '\0';
  return out;
}

[[nodiscard]] std::size_t finish(int written, std::size_t capacity) {
  if (written <= 0)
    return 0;
  const auto length = static_cast<std::size_t>(written);
  return length < capacity ? length : capacity - 1;
}

} // namespace

Record encode(const Identity &identity) {
  Record record{};
  record[0] = kRecordVersion;
  for (std::size_t i = 0; i < 4; ++i)
    record[1 + i] = static_cast<std::uint8_t>(identity.bytes >> (8 * i));
  for (std::size_t i = 0; i < kChecksumBytes; ++i)
    record[5 + i] = identity.checksum[i];
  return record;
}

std::expected<Identity, RecordError>
decode(std::span<const std::uint8_t, kRecordBytes> record) {
  if (record[0] != kRecordVersion)
    return std::unexpected(RecordError::WrongVersion);
  Identity identity{};
  for (std::size_t i = 0; i < 4; ++i)
    identity.bytes |= static_cast<std::uint32_t>(record[1 + i]) << (8 * i);
  for (std::size_t i = 0; i < kChecksumBytes; ++i)
    identity.checksum[i] = record[5 + i];
  return identity;
}

Report report(const Source &source, const Store &store,
              std::span<char, kLineBytes> line) {
  line[0] = '\0';
  const auto identity = source.identity(source.context);
  if (!identity) {
    if (identity.error() == SourceError::Absent)
      return {.decision = Decision::Absent, .length = 0};
    const bool read_failed = identity.error() == SourceError::ReadFailed;
    const int written = std::snprintf(
        line.data(), line.size(), "COREDUMP present=%s valid=0 error=%s\n",
        read_failed ? "unknown" : "1",
        read_failed ? "read-failed" : "invalid-size");
    return {.decision = Decision::Unreadable,
            .length = finish(written, line.size())};
  }

  const char *prior = "none";
  const auto stored = store.load(store.context);
  if (stored) {
    const auto saved = decode(*stored);
    if (saved && *saved == *identity)
      return {.decision = Decision::AlreadyReported, .length = 0};
    prior = saved ? "other" : "corrupt";
  } else if (stored.error() != StoreError::Missing) {
    prior = "unknown";
  }

  const auto verified = source.verify(source.context);
  const char *valid = "1";
  // Save only after a definite answer, so a flash read error now does not
  // hide the summary of a good dump on the next boot.
  bool settled = true;
  if (!verified) {
    settled = verified.error() == SourceError::Corrupt;
    valid = settled ? "0" : "unknown";
  }

  Field task = kNone;
  Field pc = kNone;
  Field exc_cause = kNone;
  Field exc_vaddr = kNone;
  Field elf = kNone;
  if (verified) {
    const auto summary = source.summary(source.context);
    if (summary) {
      task = clean(summary->task);
      pc = hex32(summary->pc);
      exc_cause = decimal(summary->exc_cause);
      exc_vaddr = hex32(summary->exc_vaddr);
      elf = clean(summary->elf);
    }
  }

  bool saved = false;
  if (settled) {
    const Record record = encode(*identity);
    saved = store.save(store.context, record).has_value();
  }

  const int written = std::snprintf(
      line.data(), line.size(),
      "COREDUMP present=1 valid=%s bytes=%lu sha=%s task=%s pc=%s "
      "exc_cause=%s exc_vaddr=%s elf=%s prior=%s saved=%d\n",
      valid, static_cast<unsigned long>(identity->bytes),
      checksum_prefix(*identity).data(), task.data(), pc.data(),
      exc_cause.data(), exc_vaddr.data(), elf.data(), prior, saved ? 1 : 0);
  return {.decision = Decision::Reported,
          .length = finish(written, line.size())};
}

} // namespace aq::coredump
