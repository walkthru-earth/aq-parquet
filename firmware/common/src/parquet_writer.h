#pragma once

#include <stddef.h>
#include <stdint.h>

namespace telemetry {

constexpr size_t kMaxColumns = 96;
// Per row group. Several row groups may share one file (kMaxRowGroups).
constexpr size_t kMaxRows = 65536;
constexpr size_t kMaxRowGroups = 8;

enum class PhysicalType : uint8_t { Int32 = 1, Int64 = 2, Float = 4 };
enum class Codec : uint8_t { Uncompressed = 0, Lz4Raw = 7 };
// Parquet LogicalType annotation on the schema leaf. Physical bytes are
// unchanged; readers present TIMESTAMP(NANOS, isAdjustedToUTC=true) columns as
// instants. Only valid for Int64.
enum class LogicalType : uint8_t { None = 0, TimestampNanosUtc = 1 };

// Caller owns separate raw/encoded page buffers and the codec's state. The
// callback returns compressed byte count, or zero on failure; no silent codec
// fallback is permitted inside a column chunk.
struct Compression {
  Codec codec;
  uint8_t *raw;
  size_t raw_capacity;
  uint8_t *encoded;
  size_t encoded_capacity;
  size_t (*compress)(void *, const uint8_t *, size_t, uint8_t *, size_t);
  void *context;
};

// values points at the first field in an array of rows, or a contiguous column.
// A null valid pointer makes the field required. Otherwise each strided byte is
// zero for null, nonzero for present. Buffers must remain immutable while a
// row group is being written; between row groups the caller may refill them.
struct Column {
  const char *name = nullptr;
  PhysicalType type = PhysicalType::Int32;
  const void *values = nullptr;
  size_t stride = 0;
  const uint8_t *valid = nullptr;
  size_t valid_stride = 1;
  LogicalType logical = LogicalType::None;
};

struct KeyValue {
  const char *key;
  const char *value;
};

// Return true ONLY when every byte was accepted; false aborts without retries.
using Sink = bool (*)(void *, const uint8_t *, size_t);

// Footer facts for one column chunk. min/max hold the PLAIN little-endian bit
// pattern of the value (4 or 8 bytes used). Floats follow the parquet-format
// 2.13 TYPE_ORDER rules: NaN excluded and counted, -0.0 for a zero minimum,
// +0.0 for a zero maximum, no bounds when every present value is NaN.
struct ChunkRecord {
  uint64_t offset;
  uint64_t size;
  uint64_t uncompressed_size;
  uint64_t null_count;
  uint64_t nan_count;
  uint64_t min;
  uint64_t max;
  bool has_bounds;
};

struct GroupRecord {
  uint64_t rows;
  uint64_t offset;
  uint64_t compressed_size;
  uint64_t uncompressed_size;
  int sorted_by; // column index whose values ascend within the group, or -1
};

// Caller-owned (static, heap, or PSRAM): no allocation or page-sized stack use.
struct Workspace {
  ChunkRecord chunks[kMaxRowGroups][kMaxColumns];
  GroupRecord groups[kMaxRowGroups];
  uint8_t buffer[512];
};

struct Result {
  bool ok;
  // Bytes accepted by successful sink calls since begin(). A failing sink may
  // have accepted part of its final block, which this interface cannot count.
  uint64_t bytes_written;
  const char *error;
};

// Flat numeric schema, PLAIN DataPageV1 (one page per column chunk),
// UNCOMPRESSED or caller-supplied LZ4_RAW, RLE definition levels, Thrift
// Compact footer with per-chunk statistics (null_count, min_value/max_value,
// exact flags, nan_count for FLOAT) and TYPE_ORDER column orders. A file holds
// up to kMaxRowGroups row groups written one at a time:
//
//   Writer writer;
//   writer.begin(sink, context, workspace, columns, count, compression);
//   writer.row_group(rows, sorted_by);   // repeat as batches complete
//   writer.finish(metadata, metadata_count, build);
//
// The column array must stay valid and unchanged (names/types/annotation)
// until finish(). The Compression struct is copied by begin(); only the
// buffers and codec state it points to must outlive the file. After any
// failure the writer is inert; the caller owns close/sync/rename and
// recovery. This finalizes file contents only.
class Writer {
public:
  Result begin(Sink sink, void *context, Workspace &workspace,
               const Column *columns, size_t column_count,
               const Compression *compression = nullptr);
  Result row_group(size_t row_count, int sorted_by = -1);
  // build is appended to created_by as "(build <build>)" when given. Omitting
  // created_by preserves the existing CoreS3 file identity.
  Result finish(const KeyValue *metadata = nullptr, size_t metadata_count = 0,
                const char *build = nullptr, const char *created_by = nullptr);
  bool active() const { return active_; }
  size_t row_groups() const { return groups_; }
  uint64_t rows() const { return rows_; }
  uint64_t bytes_written() const { return accepted_; }

private:
  Result fail(const char *error);
  bool active_ = false;
  Sink sink_ = nullptr;
  void *context_ = nullptr;
  Workspace *workspace_ = nullptr;
  const Column *columns_ = nullptr;
  size_t column_count_ = 0;
  bool compressed_ = false;
  Compression compression_{}; // copy: the caller's struct may be a temporary
  size_t groups_ = 0;
  size_t used_ = 0; // bytes staged in workspace_->buffer
  uint64_t rows_ = 0;
  uint64_t position_ = 0;
  uint64_t accepted_ = 0;
};

// One row group (or zero when row_count is 0) in one call.
Result write_parquet(Sink sink, void *context, const Column *columns,
                     size_t column_count, size_t row_count,
                     Workspace &workspace, const KeyValue *metadata = nullptr,
                     size_t metadata_count = 0,
                     const Compression *compression = nullptr,
                     int sorted_by = -1, const char *build = nullptr,
                     const char *created_by = nullptr);

} // namespace telemetry
