// Build through pixi (tools/test_parquet_reader.py); runs exactly the firmware
// footer reader on the host.
//
// Usage:
//   parquet_reader_fixture dump input.parquet
//     Decode through both entry points, require identical results, and print
//     the summary and Iceberg metrics as one JSON object. Exit 1 with
//     {"error": ...} when the reader refuses the file.
//   parquet_reader_fixture fuzz input.parquet iterations seed
//     Mutate and truncate the footer and feed every variant to both entry
//     points. Accepted variants must still satisfy the summary invariants.
//     Sanitizers catch memory and undefined-behaviour faults.
#include "../firmware/common/src/parquet_reader.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {

using telemetry::FileSummary;
using telemetry::IcebergDataFileMetrics;

struct Buffer {
  const uint8_t *data;
  size_t size;
  size_t reads;
};

bool buffer_read(void *context, uint64_t offset, uint8_t *dest, size_t size) {
  auto &buffer = *static_cast<Buffer *>(context);
  ++buffer.reads;
  if (offset > buffer.size || size > buffer.size - offset)
    return false;
  memcpy(dest, buffer.data + offset, size);
  return true;
}

bool load(const char *path, std::vector<uint8_t> &bytes) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return false;
  uint8_t chunk[4096];
  size_t n = 0;
  while ((n = fread(chunk, 1, sizeof(chunk), file)) > 0)
    bytes.insert(bytes.end(), chunk, chunk + n);
  return fclose(file) == 0;
}

const char *type_name(telemetry::PhysicalType type) {
  switch (type) {
  case telemetry::PhysicalType::Int32:
    return "INT32";
  case telemetry::PhysicalType::Int64:
    return "INT64";
  case telemetry::PhysicalType::Float:
    return "FLOAT";
  case telemetry::PhysicalType::ByteArray:
    return "BYTE_ARRAY";
  }
  return "?";
}

const char *logical_name(telemetry::LogicalType logical) {
  switch (logical) {
  case telemetry::LogicalType::None:
    return "none";
  case telemetry::LogicalType::TimestampMicrosUtc:
    return "timestamp_micros_utc";
  case telemetry::LogicalType::Uint8:
    return "uint8";
  case telemetry::LogicalType::Geometry:
    return "geometry";
  }
  return "?";
}

void hex(const uint8_t *bytes, size_t size) {
  putchar('"');
  for (size_t i = 0; i < size; ++i)
    printf("%02x", bytes[i]);
  putchar('"');
}

void print_u64_or_null(bool present, uint64_t value) {
  if (present)
    printf("%" PRIu64, value);
  else
    printf("null");
}

void dump(const FileSummary &file, const IcebergDataFileMetrics *metrics,
          const char *metrics_error) {
  printf("{\"version\":%d,\"num_rows\":%" PRIu64 ",\"file_size\":%" PRIu64
         ",\"footer_size\":%u,\"columns\":[",
         file.version, file.num_rows, file.file_size, file.footer_size);
  for (size_t c = 0; c < file.column_count; ++c) {
    const auto &column = file.columns[c];
    printf("%s{\"name\":\"%s\",\"type\":\"%s\",\"nullable\":%s,"
           "\"timestamp_micros_utc\":%s,\"logical\":\"%s\",\"field_id\":%d}",
           c ? "," : "", column.name, type_name(column.type),
           column.nullable ? "true" : "false",
           column.logical == telemetry::LogicalType::TimestampMicrosUtc
               ? "true"
               : "false",
           logical_name(column.logical), column.field_id);
  }
  printf("],\"row_groups\":[");
  for (size_t g = 0; g < file.row_group_count; ++g) {
    const auto &group = file.row_groups[g];
    printf("%s{\"num_rows\":%" PRIu64 ",\"file_offset\":%" PRIu64
           ",\"total_byte_size\":%" PRIu64 ",\"columns\":[",
           g ? "," : "", group.num_rows, group.file_offset,
           group.total_byte_size);
    for (size_t c = 0; c < file.column_count; ++c) {
      const auto &chunk = group.columns[c];
      printf("%s{\"codec\":%d,\"num_values\":%" PRIu64
             ",\"total_uncompressed_size\":%" PRIu64
             ",\"total_compressed_size\":%" PRIu64
             ",\"data_page_offset\":%" PRIu64 ",\"start_offset\":%" PRIu64
             ",\"null_count\":",
             c ? "," : "", static_cast<int>(chunk.codec), chunk.num_values,
             chunk.total_uncompressed_size, chunk.total_compressed_size,
             chunk.data_page_offset, chunk.start_offset);
      print_u64_or_null(chunk.has_null_count, chunk.null_count);
      printf(",\"nan_count\":");
      print_u64_or_null(chunk.has_nan_count, chunk.nan_count);
      printf(",\"min\":");
      print_u64_or_null(chunk.has_bounds, chunk.min);
      printf(",\"max\":");
      print_u64_or_null(chunk.has_bounds, chunk.max);
      putchar('}');
    }
    printf("]}");
  }
  printf("]");
  if (!metrics) {
    printf(",\"metrics_error\":\"%s\"}\n", metrics_error);
    return;
  }
  printf(",\"iceberg\":{\"record_count\":%" PRIu64
         ",\"file_size_in_bytes\":%" PRIu64 ",\"split_offsets\":[",
         metrics->record_count, metrics->file_size_in_bytes);
  for (size_t g = 0; g < metrics->split_offset_count; ++g)
    printf("%s%" PRIu64, g ? "," : "", metrics->split_offsets[g]);
  printf("],\"columns\":[");
  for (size_t c = 0; c < metrics->column_count; ++c) {
    const auto &column = metrics->columns[c];
    printf("%s{\"field_id\":%d,\"column_size\":%" PRIu64
           ",\"value_count\":%" PRIu64 ",\"null_value_count\":",
           c ? "," : "", column.field_id, column.column_size,
           column.value_count);
    print_u64_or_null(column.has_null_value_count, column.null_value_count);
    printf(",\"nan_value_count\":");
    print_u64_or_null(column.has_nan_value_count, column.nan_value_count);
    printf(",\"lower_bound\":");
    if (column.has_bounds)
      hex(column.lower_bound, column.bound_size);
    else
      printf("null");
    printf(",\"upper_bound\":");
    if (column.has_bounds)
      hex(column.upper_bound, column.bound_size);
    else
      printf("null");
    putchar('}');
  }
  printf("]}}\n");
}

// Invariants every accepted footer must satisfy, whatever the input bytes.
bool consistent(const FileSummary &file, size_t file_size) {
  if (file.file_size != file_size || !file.column_count ||
      file.column_count > telemetry::kMaxColumns ||
      file.row_group_count > telemetry::kMaxRowGroups ||
      file.footer_size > file_size - 12)
    return false;
  const uint64_t data_end = file_size - 8 - file.footer_size;
  uint64_t rows = 0;
  for (size_t c = 0; c < file.column_count; ++c) {
    const size_t length =
        strnlen(file.columns[c].name, telemetry::kMaxNameLength);
    if (!length || length >= telemetry::kMaxNameLength)
      return false;
  }
  for (size_t g = 0; g < file.row_group_count; ++g) {
    const auto &group = file.row_groups[g];
    if (group.num_rows > file.num_rows - rows)
      return false;
    rows += group.num_rows;
    for (size_t c = 0; c < file.column_count; ++c) {
      const auto &chunk = group.columns[c];
      if (chunk.num_values != group.num_rows ||
          chunk.start_offset + chunk.total_compressed_size > data_end ||
          (chunk.has_null_count && chunk.null_count > chunk.num_values))
        return false;
    }
  }
  return rows == file.num_rows;
}

bool same(const FileSummary &a, const FileSummary &b) {
  // Every decoded field is written explicitly, so padding aside the two
  // summaries must agree; compare the parts the reader filled.
  if (a.version != b.version || a.num_rows != b.num_rows ||
      a.file_size != b.file_size || a.footer_size != b.footer_size ||
      a.column_count != b.column_count ||
      a.row_group_count != b.row_group_count)
    return false;
  for (size_t c = 0; c < a.column_count; ++c) {
    const auto &x = a.columns[c], &y = b.columns[c];
    if (strcmp(x.name, y.name) != 0 || x.type != y.type ||
        x.nullable != y.nullable || x.logical != y.logical ||
        x.field_id != y.field_id)
      return false;
  }
  for (size_t g = 0; g < a.row_group_count; ++g) {
    const auto &x = a.row_groups[g], &y = b.row_groups[g];
    if (x.num_rows != y.num_rows || x.file_offset != y.file_offset ||
        x.total_byte_size != y.total_byte_size)
      return false;
    for (size_t c = 0; c < a.column_count; ++c) {
      const auto &p = x.columns[c], &q = y.columns[c];
      if (p.codec != q.codec || p.num_values != q.num_values ||
          p.total_compressed_size != q.total_compressed_size ||
          p.data_page_offset != q.data_page_offset ||
          p.start_offset != q.start_offset ||
          p.has_null_count != q.has_null_count ||
          p.null_count != q.null_count || p.has_nan_count != q.has_nan_count ||
          p.nan_count != q.nan_count || p.has_bounds != q.has_bounds ||
          p.min != q.min || p.max != q.max)
        return false;
    }
  }
  return true;
}

struct State {
  FileSummary memory;
  FileSummary streamed;
  IcebergDataFileMetrics metrics;
  uint8_t scratch[1 << 16];
};

int run_dump(const char *path, State &state) {
  std::vector<uint8_t> bytes;
  if (!load(path, bytes))
    return 3;
  const auto in_memory =
      telemetry::read_parquet_footer(bytes.data(), bytes.size(), state.memory);
  Buffer buffer{bytes.data(), bytes.size(), 0};
  const auto streamed = telemetry::read_parquet_footer(
      buffer_read, &buffer, bytes.size(), state.scratch, sizeof(state.scratch),
      state.streamed);
  if (in_memory.ok != streamed.ok)
    return 4;
  if (!in_memory.ok) {
    printf("{\"error\":\"%s\"}\n", in_memory.error);
    return 1;
  }
  // Header magic, tail, then the footer: three reads, never the pages.
  if (!same(state.memory, state.streamed) || buffer.reads != 3 ||
      !consistent(state.memory, bytes.size()))
    return 5;
  const auto metrics =
      telemetry::iceberg_data_file_metrics(state.memory, state.metrics);
  dump(state.memory, metrics.ok ? &state.metrics : nullptr, metrics.error);
  return 0;
}

uint32_t next(uint64_t &seed) {
  seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
  return static_cast<uint32_t>(seed >> 33);
}

int run_fuzz(const char *path, unsigned long iterations, uint64_t seed,
             State &state) {
  std::vector<uint8_t> original;
  if (!load(path, original) || original.size() < 12)
    return 3;
  uint32_t footer = 0;
  memcpy(&footer, original.data() + original.size() - 8, 4);
  if (footer > original.size() - 12)
    return 3;
  const size_t footer_start = original.size() - 8 - footer;
  unsigned long accepted = 0;
  std::vector<uint8_t> bytes;
  for (unsigned long i = 0; i < iterations; ++i) {
    bytes = original;
    const uint32_t mode = next(seed) % 8;
    const size_t edits = 1 + next(seed) % 4;
    for (size_t e = 0; e < edits; ++e) {
      const size_t at = footer_start + next(seed) % footer;
      switch (mode) {
      case 0: // flip one bit
        bytes[at] ^= static_cast<uint8_t>(1U << (next(seed) % 8));
        break;
      case 1: // random byte
        bytes[at] = static_cast<uint8_t>(next(seed));
        break;
      case 2: // varint continuation everywhere
        bytes[at] |= 0x80U;
        break;
      case 3: // long-form list or field headers
        bytes[at] = static_cast<uint8_t>(0xF0U | (next(seed) % 13));
        break;
      case 4: { // truncate the footer, keep a valid-looking tail
        const uint32_t cut = 1 + next(seed) % footer;
        bytes.erase(bytes.begin() + static_cast<long>(footer_start),
                    bytes.begin() + static_cast<long>(footer_start + cut));
        e = edits;
        break;
      }
      case 5: { // wrong footer length
        uint32_t length = next(seed);
        if (next(seed) % 2)
          length = footer + (next(seed) % 64) - 32;
        memcpy(bytes.data() + bytes.size() - 8, &length, 4);
        e = edits;
        break;
      }
      case 6: // zero a run
        for (size_t z = at; z < bytes.size() - 8 && z < at + 8; ++z)
          bytes[z] = 0;
        break;
      default: // drop the whole file to a random prefix plus the tail
        bytes.erase(bytes.begin() +
                        static_cast<long>(next(seed) % (bytes.size() - 8)),
                    bytes.end() - 8);
        e = edits;
        break;
      }
    }
    const auto in_memory = telemetry::read_parquet_footer(
        bytes.data(), bytes.size(), state.memory);
    Buffer buffer{bytes.data(), bytes.size(), 0};
    const auto streamed = telemetry::read_parquet_footer(
        buffer_read, &buffer, bytes.size(), state.scratch,
        sizeof(state.scratch), state.streamed);
    if (in_memory.ok != streamed.ok) {
      fprintf(stderr, "entry points disagree at iteration %lu\n", i);
      return 6;
    }
    if (!in_memory.ok)
      continue;
    ++accepted;
    if (!same(state.memory, state.streamed) ||
        !consistent(state.memory, bytes.size())) {
      fprintf(stderr, "inconsistent summary at iteration %lu\n", i);
      return 7;
    }
    telemetry::iceberg_data_file_metrics(state.memory, state.metrics);
  }
  printf("{\"iterations\":%lu,\"accepted\":%lu}\n", iterations, accepted);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  // FileSummary and metrics are large; keep them off the stack as the
  // firmware must.
  auto state = std::make_unique<State>();
  if (argc == 3 && strcmp(argv[1], "dump") == 0)
    return run_dump(argv[2], *state);
  if (argc == 5 && strcmp(argv[1], "fuzz") == 0)
    return run_fuzz(argv[2], strtoul(argv[3], nullptr, 10),
                    strtoull(argv[4], nullptr, 10), *state);
  return 2;
}
