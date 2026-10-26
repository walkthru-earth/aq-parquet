#pragma once

// Bounded Parquet footer reader and Apache Iceberg DataFile metrics.
//
// Decodes the Thrift Compact FileMetaData of flat files with INT32, INT64,
// FLOAT and GEOMETRY BYTE_ARRAY leaves (the shapes parquet_writer.h emits)
// into a caller-owned FileSummary, then derives the per-column metrics an
// Iceberg v3 manifest entry needs. Manifests and catalog commits stay on the
// phone or host; this code only reads what the footer already says, never
// pages.
//
// Zero heap. The caller owns FileSummary (about 70 KB, so static, PSRAM or
// host memory, never a task stack) and the footer scratch for ReadAt. Each
// decode call uses well under 1 KB of stack (checked by the host test with
// -Wframe-larger-than), and every Thrift element consumes at least one byte,
// so work is bounded by the footer length.
//
// key_value_metadata and created_by are validated and skipped, so their length
// is not limited by the reader. Statistics that are absent stay absent: the
// has_* flags say what the footer actually carried.

#include "parquet_writer.h"

namespace telemetry {

constexpr size_t kMaxNameLength = 64;

// Random-access byte reader (SD file, flash, host descriptor). Returns true
// only when all `size` bytes at `offset` were copied to `dest`.
using ReadAt = bool (*)(void *context, uint64_t offset, uint8_t *dest,
                        size_t size);

struct SchemaColumn {
  char name[kMaxNameLength]{};
  PhysicalType type = PhysicalType::Int32;
  bool nullable = false;
  LogicalType logical = LogicalType::None;
  int32_t field_id = 0; // 0 when the footer carries none
};

struct ColumnChunkSummary {
  Codec codec = Codec::Uncompressed;
  uint64_t num_values = 0;
  uint64_t total_uncompressed_size = 0;
  uint64_t total_compressed_size = 0;
  uint64_t data_page_offset = 0;
  uint64_t start_offset = 0; // dictionary page when present, else data page
  uint64_t null_count = 0;
  uint64_t nan_count = 0;
  uint64_t min = 0; // little-endian PLAIN bits, 4 or 8 bytes wide
  uint64_t max = 0;
  bool has_null_count = false;
  bool has_nan_count = false;
  bool has_bounds = false;
};

struct RowGroupSummary {
  uint64_t num_rows = 0;
  uint64_t file_offset = 0;
  uint64_t total_byte_size = 0;
  ColumnChunkSummary columns[kMaxColumns]{};
};

struct FileSummary {
  int32_t version = 0;
  uint64_t num_rows = 0;
  uint64_t file_size = 0;
  uint32_t footer_size = 0;
  size_t column_count = 0;
  size_t row_group_count = 0;
  SchemaColumn columns[kMaxColumns]{};
  RowGroupSummary row_groups[kMaxRowGroups]{};
};

// One column of an Iceberg DataFile, keyed by Parquet field_id. Bounds use
// the Iceberg single-value binary form, little-endian, 4 bytes for int and
// float and 8 bytes for long and timestamptz (microseconds). A GEOMETRY
// column never has bounds here, its bbox is in GeospatialStatistics, which
// this reader skips.
struct IcebergColumnMetrics {
  int32_t field_id = 0;
  PhysicalType type = PhysicalType::Int32;
  LogicalType logical = LogicalType::None;
  uint64_t column_size = 0;
  uint64_t value_count = 0;
  uint64_t null_value_count = 0;
  uint64_t nan_value_count = 0;
  bool has_null_value_count = false; // every chunk carried null_count
  bool has_nan_value_count = false;  // FLOAT and every chunk carried nan_count
  bool has_bounds = false; // every chunk with a non-NaN value carried bounds
  uint8_t bound_size = 0;
  uint8_t lower_bound[8]{};
  uint8_t upper_bound[8]{};
};

struct IcebergDataFileMetrics {
  uint64_t record_count = 0;
  uint64_t file_size_in_bytes = 0;
  size_t split_offset_count = 0;
  uint64_t split_offsets[kMaxRowGroups]{};
  size_t column_count = 0;
  IcebergColumnMetrics columns[kMaxColumns]{};
};

// Check both PAR1 magics and decode the footer of an in-memory file.
Result read_parquet_footer(const uint8_t *file_bytes, size_t file_size,
                           FileSummary &summary);

// Same, through ReadAt. The footer is copied into caller scratch first, so
// scratch_capacity bounds the largest accepted footer.
Result read_parquet_footer(ReadAt read_at, void *context, uint64_t file_size,
                           uint8_t *footer_scratch, size_t scratch_capacity,
                           FileSummary &summary);

// Derive Iceberg DataFile metrics. Every column needs a positive, unique
// field_id; files without ids must be mapped by name on the host instead.
Result iceberg_data_file_metrics(const FileSummary &file,
                                 IcebergDataFileMetrics &metrics);

} // namespace telemetry
