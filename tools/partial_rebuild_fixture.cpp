// Host fixture for tools/test_rebuild_partial.py. Writes a board-shaped file
// with exactly the firmware writer and the board field table, then prints the
// byte offset where each row group ends, so the test can cut the file at the
// row group, footer and rename stages.
//
// Build with -DAQ_FIELDS_INC="\"<board>/telemetry_fields.inc\"".
// Usage: partial_rebuild_fixture output.parquet none|lz4 groups rows
//
// Columns follow prepare_columns() in the shared logger. Every leaf is
// OPTIONAL with a validity byte, field_id is the 1-based table position and
// an INT64 "us_since_unix_epoch" field carries TIMESTAMP(MICROS, UTC).
#include "../firmware/common/src/lz4_codec.h"
#include "../firmware/common/src/parquet_writer.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#ifndef AQ_FIELDS_INC
#error "define AQ_FIELDS_INC as the quoted path of a board telemetry_fields.inc"
#endif

namespace {
using telemetry::Column;
using telemetry::LogicalType;
using telemetry::PhysicalType;

struct FieldDef {
  const char *name;
  PhysicalType type;
  const char *unit;
};

constexpr FieldDef kFields[] = {
#define FIELD(name, type, procedure, unit, validity, live)                     \
  {#name, PhysicalType::type, unit},
#include AQ_FIELDS_INC
#undef FIELD
};
constexpr std::size_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);
static_assert(kFieldCount <= telemetry::kMaxColumns);

struct Row {
  std::array<std::uint64_t, kFieldCount> data{};
  std::array<std::uint8_t, kFieldCount> valid{};
};

bool file_sink(void *context, const std::uint8_t *bytes, std::size_t length) {
  return std::fwrite(bytes, 1, length, static_cast<FILE *>(context)) == length;
}

void put32(Row &row, std::size_t field, std::uint32_t bits) {
  row.data[field] = bits; // the writer reads the low four bytes (host LE)
}

void put_float(Row &row, std::size_t field, float value) {
  std::uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  put32(row, field, bits);
}

// Deterministic rows. Global row r spans every group. Identity and clock
// fields are always present and other fields have nulls. Float fields carry a
// NaN in the first group. In that group every third float field starts at a
// positive zero and only grows, so its minimum is +0.0 and the writer stores
// -0.0. The next field only falls and holds a negative zero, so its maximum is
// -0.0 and the writer stores +0.0. That pins both signed zero rules in the
// rebuilt statistics.
void fill(Row &row, std::size_t r) {
  for (std::size_t i = 0; i < kFieldCount; ++i) {
    const auto &field = kFields[i];
    const bool core = i < 9;
    row.valid[i] = core || (r * 3 + i) % 7 != 0;
    if (std::strcmp(field.name, "sequence") == 0) {
      row.data[i] = 1000 + r;
    } else if (std::strcmp(field.name, "monotonic_us") == 0) {
      row.data[i] = 10'000'000ULL * (r + 1);
    } else if (std::strcmp(field.name, "schema_version") == 0) {
      put32(row, i, 5);
    } else if (field.type == PhysicalType::Int64 &&
               std::strcmp(field.unit, "us_since_unix_epoch") == 0) {
      row.data[i] = static_cast<std::uint64_t>(
          1'788'890'000'000'000LL + static_cast<std::int64_t>(r) * 10'000'000);
      row.valid[i] = r % 11 != 0;
    } else if (field.type == PhysicalType::Int64) {
      const auto value =
          (static_cast<std::int64_t>(r) - 45) * 1'234'567'890'123LL +
          static_cast<std::int64_t>(i);
      row.data[i] = static_cast<std::uint64_t>(value);
    } else if (field.type == PhysicalType::Int32) {
      const auto value = static_cast<std::int32_t>(r * 31) +
                         static_cast<std::int32_t>(i * 7) - 100;
      put32(row, i, static_cast<std::uint32_t>(value));
    } else {
      float value =
          static_cast<float>(r) * 0.5F - 20.0F + static_cast<float>(i) * 0.01F;
      if (i % 3 == 0)
        value = static_cast<float>(r) * 0.5F;
      else if (i % 3 == 1)
        value = -static_cast<float>(r) * 0.5F - 0.25F;
      if (r == 1 && i % 2 == 0)
        value = std::numeric_limits<float>::quiet_NaN();
      if (r == 2 && i % 3 != 0)
        value = -0.0F;
      put_float(row, i, value);
    }
  }
}

int index_of(const char *name) {
  for (std::size_t i = 0; i < kFieldCount; ++i)
    if (std::strcmp(kFields[i].name, name) == 0)
      return static_cast<int>(i);
  return -1;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 5)
    return 2;
  const bool compressed = std::strcmp(argv[2], "lz4") == 0;
  const std::size_t groups = std::strtoul(argv[3], nullptr, 10);
  const std::size_t count = std::strtoul(argv[4], nullptr, 10);
  const int sequence = index_of("sequence");
  if (!groups || groups > telemetry::kMaxRowGroups || !count || count > 90 ||
      sequence < 0)
    return 2;
  std::vector<Row> rows(count);
  std::array<Column, kFieldCount> columns{};
  for (std::size_t i = 0; i < kFieldCount; ++i) {
    const auto &field = kFields[i];
    const auto logical =
        field.type == PhysicalType::Int64 &&
                std::strcmp(field.unit, "us_since_unix_epoch") == 0
            ? LogicalType::TimestampMicrosUtc
            : LogicalType::None;
    columns[i] = Column{field.name,        field.type,
                        &rows[0].data[i],  sizeof(Row),
                        &rows[0].valid[i], sizeof(Row),
                        logical,           static_cast<std::int32_t>(i + 1)};
  }
  telemetry::Workspace workspace{};
  telemetry::Lz4Workspace lz4{};
  const auto compression = lz4.configuration();
  FILE *file = std::fopen(argv[1], "wb");
  if (!file)
    return 3;
  telemetry::Writer writer;
  auto result = writer.begin(file_sink, file, workspace, columns.data(),
                             kFieldCount, compressed ? &compression : nullptr);
  std::vector<unsigned long long> ends;
  for (std::size_t g = 0; g < groups && result.ok; ++g) {
    for (std::size_t r = 0; r < count; ++r)
      fill(rows[r], g * count + r);
    result = writer.row_group(count, sequence);
    ends.push_back(writer.bytes_written());
  }
  const telemetry::KeyValue metadata[] = {
      {"schema_version", "fixture"}, {"purpose", "partial-rebuild-fixture"}};
  if (result.ok)
    result = writer.finish(metadata, 2, "fixture", "aq-parquet fixture");
  const bool closed = std::fclose(file) == 0;
  if (!result.ok || !closed) {
    std::fprintf(stderr, "write failed: %s\n",
                 result.error ? result.error : "close");
    return 4;
  }
  std::printf("{\"columns\":%zu,\"size\":%llu,\"group_ends\":[", kFieldCount,
              static_cast<unsigned long long>(result.bytes_written));
  for (std::size_t g = 0; g < ends.size(); ++g)
    std::printf("%s%llu", g ? "," : "", ends[g]);
  std::puts("]}");
  return 0;
}
