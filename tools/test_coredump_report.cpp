#include "../firmware/common/runtime/src/coredump_report.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace {
namespace cd = opensensor::coredump;

struct FakeFlash {
  std::expected<cd::Identity, cd::SourceError> identity =
      std::unexpected(cd::SourceError::Absent);
  std::expected<void, cd::SourceError> verify{};
  std::expected<cd::Summary, cd::SourceError> summary{};
  int identity_calls = 0;
  int summary_calls = 0;
};

struct FakeNvs {
  std::optional<cd::Record> record;
  bool read_fails = false;
  bool write_fails = false;
  int saves = 0;
};

cd::Source source_for(FakeFlash &flash) {
  return {.context = &flash,
          .identity =
              [](void *context) {
                auto &f = *static_cast<FakeFlash *>(context);
                ++f.identity_calls;
                return f.identity;
              },
          .verify =
              [](void *context) {
                return static_cast<FakeFlash *>(context)->verify;
              },
          .summary =
              [](void *context) {
                auto &f = *static_cast<FakeFlash *>(context);
                ++f.summary_calls;
                return f.summary;
              }};
}

cd::Store store_for(FakeNvs &nvs) {
  return {
      .context = &nvs,
      .load = [](void *context) -> std::expected<cd::Record, cd::StoreError> {
        auto &n = *static_cast<FakeNvs *>(context);
        if (n.read_fails)
          return std::unexpected(cd::StoreError::ReadFailed);
        if (!n.record)
          return std::unexpected(cd::StoreError::Missing);
        return *n.record;
      },
      .save = [](void *context,
                 std::span<const std::uint8_t, cd::kRecordBytes> record)
          -> std::expected<void, cd::StoreError> {
        auto &n = *static_cast<FakeNvs *>(context);
        ++n.saves;
        if (n.write_fails)
          return std::unexpected(cd::StoreError::WriteFailed);
        cd::Record copy{};
        std::memcpy(copy.data(), record.data(), record.size());
        n.record = copy;
        return {};
      }};
}

cd::Identity dump(std::uint32_t bytes, std::uint8_t seed) {
  cd::Identity identity{.bytes = bytes};
  for (std::size_t i = 0; i < identity.checksum.size(); ++i)
    identity.checksum[i] = static_cast<std::uint8_t>(seed + i);
  return identity;
}

template <std::size_t N> std::array<char, N> text(std::string_view value) {
  std::array<char, N> out{};
  std::memcpy(out.data(), value.data(), value.size() < N ? value.size() : N);
  return out;
}

cd::Summary worker_stall() {
  return {.task = text<cd::kTaskNameBytes>("parquet-sd"),
          .pc = 0x4037a1b2,
          .exc_cause = 1,
          .exc_vaddr = 0,
          .elf = text<cd::kElfShaChars>("3f2a1b0c")};
}

struct Boot {
  cd::Report report;
  std::string line;
};

Boot boot(FakeFlash &flash, FakeNvs &nvs) {
  std::array<char, cd::kLineBytes> line{};
  line.fill('#');
  const cd::Report report = cd::report(source_for(flash), store_for(nvs), line);
  const std::string out(line.data());
  assert(out.size() == report.length);
  if (report.length > 0) {
    assert(out.back() == '\n');
    assert(out.find('\n') == out.size() - 1);
    assert(out.rfind("COREDUMP ", 0) == 0);
  }
  return {.report = report, .line = out};
}

bool has(const std::string &line, std::string_view field) {
  return line.find(std::string(" ") + std::string(field) + " ") !=
             std::string::npos ||
         line.find(std::string(" ") + std::string(field) + "\n") !=
             std::string::npos;
}

void record_round_trip() {
  const cd::Identity identity = dump(0x01020304, 0xa0);
  const cd::Record record = cd::encode(identity);
  assert(record.size() == 37);
  assert(record[0] == cd::kRecordVersion);
  // Explicit little endian, never host order.
  assert(record[1] == 0x04 && record[2] == 0x03 && record[3] == 0x02 &&
         record[4] == 0x01);
  assert(record[5] == 0xa0 && record[36] == 0xa0 + 31);
  const auto back = cd::decode(record);
  assert(back.has_value() && *back == identity);
  cd::Record wrong = record;
  wrong[0] = 0;
  const auto rejected = cd::decode(wrong);
  assert(!rejected.has_value() &&
         rejected.error() == cd::RecordError::WrongVersion);
}

void no_dump_is_silent() {
  FakeFlash flash;
  FakeNvs nvs;
  const Boot first = boot(flash, nvs);
  assert(first.report.decision == cd::Decision::Absent);
  assert(first.line.empty() && nvs.saves == 0 && flash.summary_calls == 0);
}

void reports_once_per_dump() {
  FakeFlash flash{.identity = dump(24992, 7), .summary = worker_stall()};
  FakeNvs nvs;
  const Boot first = boot(flash, nvs);
  assert(first.report.decision == cd::Decision::Reported);
  assert(first.line ==
         "COREDUMP present=1 valid=1 bytes=24992 sha=0708090a0b0c0d0e "
         "task=parquet-sd pc=0x4037a1b2 exc_cause=1 exc_vaddr=0x00000000 "
         "elf=3f2a1b0c prior=none saved=1\n");
  assert(nvs.saves == 1 && nvs.record == cd::encode(dump(24992, 7)));

  for (int i = 0; i < 3; ++i) {
    const Boot later = boot(flash, nvs);
    assert(later.report.decision == cd::Decision::AlreadyReported);
    assert(later.line.empty());
  }
  assert(nvs.saves == 1 && flash.summary_calls == 1);

  // A new panic of the same size still differs in its checksum.
  flash.identity = dump(24992, 8);
  const Boot next = boot(flash, nvs);
  assert(next.report.decision == cd::Decision::Reported);
  assert(has(next.line, "prior=other") && has(next.line, "saved=1"));
  assert(boot(flash, nvs).report.decision == cd::Decision::AlreadyReported);

  // Same checksum bytes but another size is another dump.
  flash.identity = dump(30000, 8);
  assert(boot(flash, nvs).report.decision == cd::Decision::Reported);
}

void unreadable_store_reports_again() {
  FakeFlash flash{.identity = dump(24992, 7), .summary = worker_stall()};
  FakeNvs nvs{.record = cd::encode(dump(24992, 7)), .read_fails = true};
  for (int i = 0; i < 2; ++i) {
    const Boot b = boot(flash, nvs);
    assert(b.report.decision == cd::Decision::Reported);
    assert(has(b.line, "prior=unknown") && has(b.line, "saved=1"));
  }
  assert(nvs.saves == 2);
  nvs.read_fails = false;
  assert(boot(flash, nvs).report.decision == cd::Decision::AlreadyReported);
}

void failed_save_reports_again() {
  FakeFlash flash{.identity = dump(24992, 7), .summary = worker_stall()};
  FakeNvs nvs{.record = std::nullopt, .read_fails = false, .write_fails = true};
  for (int i = 0; i < 2; ++i) {
    const Boot b = boot(flash, nvs);
    assert(b.report.decision == cd::Decision::Reported);
    assert(has(b.line, "prior=none") && has(b.line, "saved=0"));
  }
  nvs.write_fails = false;
  assert(has(boot(flash, nvs).line, "saved=1"));
  assert(boot(flash, nvs).report.decision == cd::Decision::AlreadyReported);
}

void corrupt_record_is_not_a_match() {
  FakeFlash flash{.identity = dump(24992, 7), .summary = worker_stall()};
  cd::Record stale = cd::encode(dump(24992, 7));
  stale[0] = 0;
  FakeNvs nvs{.record = stale};
  const Boot b = boot(flash, nvs);
  assert(b.report.decision == cd::Decision::Reported);
  assert(has(b.line, "prior=corrupt") && has(b.line, "saved=1"));
  assert(boot(flash, nvs).report.decision == cd::Decision::AlreadyReported);
}

void corrupt_dump_reported_once_without_summary() {
  FakeFlash flash{.identity = dump(4096, 1),
                  .verify = std::unexpected(cd::SourceError::Corrupt),
                  .summary = worker_stall()};
  FakeNvs nvs;
  const Boot b = boot(flash, nvs);
  assert(b.report.decision == cd::Decision::Reported);
  assert(b.line == "COREDUMP present=1 valid=0 bytes=4096 sha=0102030405060708 "
                   "task=- pc=- exc_cause=- exc_vaddr=- elf=- prior=none "
                   "saved=1\n");
  assert(flash.summary_calls == 0);
  assert(boot(flash, nvs).report.decision == cd::Decision::AlreadyReported);
}

void verify_read_error_does_not_settle() {
  FakeFlash flash{.identity = dump(24992, 7),
                  .verify = std::unexpected(cd::SourceError::ReadFailed),
                  .summary = worker_stall()};
  FakeNvs nvs;
  const Boot b = boot(flash, nvs);
  assert(b.report.decision == cd::Decision::Reported);
  assert(has(b.line, "valid=unknown") && has(b.line, "task=-") &&
         has(b.line, "saved=0"));
  assert(nvs.saves == 0);
  // Flash reads work again, so the full summary arrives once.
  flash.verify = {};
  const Boot later = boot(flash, nvs);
  assert(has(later.line, "valid=1") && has(later.line, "task=parquet-sd") &&
         has(later.line, "saved=1"));
  assert(boot(flash, nvs).report.decision == cd::Decision::AlreadyReported);
}

void missing_summary_still_reports() {
  FakeFlash flash{.identity = dump(24992, 7),
                  .summary = std::unexpected(cd::SourceError::ReadFailed)};
  FakeNvs nvs;
  const Boot b = boot(flash, nvs);
  assert(has(b.line, "valid=1") && has(b.line, "task=-") &&
         has(b.line, "pc=-") && has(b.line, "elf=-") && has(b.line, "saved=1"));
}

void unidentified_dump_lines() {
  FakeFlash flash{.identity = std::unexpected(cd::SourceError::ReadFailed)};
  FakeNvs nvs;
  Boot b = boot(flash, nvs);
  assert(b.report.decision == cd::Decision::Unreadable);
  assert(b.line == "COREDUMP present=unknown valid=0 error=read-failed\n");
  flash.identity = std::unexpected(cd::SourceError::InvalidSize);
  b = boot(flash, nvs);
  assert(b.report.decision == cd::Decision::Unreadable);
  assert(b.line == "COREDUMP present=1 valid=0 error=invalid-size\n");
  assert(nvs.saves == 0);
}

void names_stay_one_token() {
  cd::Summary odd = worker_stall();
  // Full 16 bytes with no NUL, a space, '=' and a newline.
  odd.task = text<cd::kTaskNameBytes>("ab cd=ef\ngh_ij.k");
  odd.elf = text<cd::kElfShaChars>("0123456789abcdef");
  FakeFlash flash{.identity = dump(24992, 7), .summary = odd};
  FakeNvs nvs;
  Boot b = boot(flash, nvs);
  assert(has(b.line, "task=ab_cd_ef_gh_ij.k"));
  assert(has(b.line, "elf=0123456789abcdef"));

  odd.task = {};
  odd.elf = {};
  flash = FakeFlash{.identity = dump(24992, 9), .summary = odd};
  b = boot(flash, nvs);
  assert(has(b.line, "task=-") && has(b.line, "elf=-"));
}

void longest_line_fits() {
  cd::Summary wide = worker_stall();
  wide.task = text<cd::kTaskNameBytes>("abcdefghijklmnop");
  wide.elf = text<cd::kElfShaChars>("abcdefghijklmnop");
  wide.pc = 0xffffffff;
  wide.exc_cause = 0xffffffff;
  wide.exc_vaddr = 0xffffffff;
  FakeFlash flash{.identity = dump(0xffffffff, 0xf0), .summary = wide};
  FakeNvs nvs{.record = std::nullopt, .read_fails = true};
  const Boot b = boot(flash, nvs);
  assert(has(b.line, "exc_cause=4294967295") &&
         has(b.line, "bytes=4294967295") && has(b.line, "prior=unknown"));
  assert(b.line.size() < cd::kLineBytes);
}

} // namespace

int main() {
  record_round_trip();
  no_dump_is_silent();
  reports_once_per_dump();
  unreadable_store_reports_again();
  failed_save_reports_again();
  corrupt_record_is_not_a_match();
  corrupt_dump_reported_once_without_summary();
  verify_read_error_does_not_settle();
  missing_summary_still_reports();
  unidentified_dump_lines();
  names_stay_one_token();
  longest_line_fits();
  return 0;
}
