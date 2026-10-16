#include "parquet_reader.h"

#include <cstring>

namespace telemetry {
namespace {

// Thrift Compact type ids.
constexpr uint8_t kStop = 0, kTrue = 1, kFalse = 2, kByte = 3, kI16 = 4,
                  kI32 = 5, kI64 = 6, kDouble = 7, kBinary = 8, kList = 9,
                  kSet = 10, kMap = 11, kStruct = 12;
// Parquet nests at most a few levels; anything deeper is hostile input.
constexpr size_t kMaxDepth = 16;
constexpr int32_t kTimestampMicros = 10; // legacy ConvertedType

Result fail(const char *error) { return {false, 0, error}; }

bool valid_type(uint8_t type) { return type != kStop && type <= kStruct; }

size_t type_width(PhysicalType type) {
  return type == PhysicalType::Int64 ? 8 : 4;
}

uint64_t load_le(const uint8_t *bytes, size_t width) {
  uint64_t value = 0;
  for (size_t i = 0; i < width; ++i)
    value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
  return value;
}

void store_le(uint8_t *dest, uint64_t value, size_t width) {
  for (size_t i = 0; i < width; ++i)
    dest[i] = static_cast<uint8_t>(value >> (8 * i));
}

float as_float(uint64_t bits) {
  const auto narrow = static_cast<uint32_t>(bits);
  float value = 0.0F;
  memcpy(&value, &narrow, sizeof(value));
  return value;
}

uint64_t float_bits(float value) {
  uint32_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

int64_t as_signed(PhysicalType type, uint64_t bits) {
  if (type == PhysicalType::Int64) {
    int64_t value = 0;
    memcpy(&value, &bits, sizeof(value));
    return value;
  }
  const auto narrow = static_cast<uint32_t>(bits);
  int32_t value = 0;
  memcpy(&value, &narrow, sizeof(value));
  return value;
}

// True when a < b in the column's sort order. NaN never reaches here.
bool less(PhysicalType type, uint64_t a, uint64_t b) {
  if (type == PhysicalType::Float)
    return as_float(a) < as_float(b);
  return as_signed(type, a) < as_signed(type, b);
}

// Each handled field may appear once per struct; a repeat could replace data
// already cross-checked against it (for example the schema after row groups).
bool once(uint32_t &seen, int16_t id) {
  if (id >= 32)
    return true;
  const uint32_t bit = 1U << id;
  if (seen & bit)
    return false;
  seen |= bit;
  return true;
}

class Decoder {
public:
  Decoder(const uint8_t *data, size_t size) : data_(data), size_(size) {}

  size_t pos() const { return pos_; }
  size_t remaining() const { return size_ - pos_; }

  bool byte(uint8_t &out) {
    if (pos_ >= size_)
      return false;
    out = data_[pos_++];
    return true;
  }

  bool varint(uint64_t &out) {
    out = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
      uint8_t b = 0;
      if (!byte(b) || (shift == 63 && b > 1))
        return false;
      out |= static_cast<uint64_t>(b & 0x7FU) << shift;
      if ((b & 0x80U) == 0)
        return true;
    }
    return false;
  }

  bool zigzag(int64_t &out) {
    uint64_t raw = 0;
    if (!varint(raw))
      return false;
    out = static_cast<int64_t>(raw >> 1) ^ -static_cast<int64_t>(raw & 1U);
    return true;
  }

  bool i32(int32_t &out) {
    int64_t value = 0;
    if (!zigzag(value) || value < INT32_MIN || value > INT32_MAX)
      return false;
    out = static_cast<int32_t>(value);
    return true;
  }

  // Non-negative INT64 (sizes, counts, offsets).
  bool count(uint64_t &out) {
    int64_t value = 0;
    if (!zigzag(value) || value < 0)
      return false;
    out = static_cast<uint64_t>(value);
    return true;
  }

  // Struct field header. Ids stay within 1..INT16_MAX; the short form adds a
  // 4-bit delta to the previous id, computed wide so it cannot wrap.
  bool field(int16_t &last, int16_t &id, uint8_t &type) {
    uint8_t head = 0;
    if (!byte(head))
      return false;
    if (head == kStop) {
      type = kStop;
      return true;
    }
    type = head & 0x0FU;
    if (!valid_type(type))
      return false;
    int64_t next = 0;
    if (const unsigned delta = head >> 4U)
      next = static_cast<int64_t>(last) + delta;
    else if (!zigzag(next))
      return false;
    if (next <= 0 || next > INT16_MAX)
      return false;
    id = static_cast<int16_t>(next);
    last = id;
    return true;
  }

  // Every list element takes at least one byte, so a count larger than the
  // bytes left is corrupt; this bounds all container loops by the input.
  bool list(uint8_t &element, size_t &length) {
    uint8_t head = 0;
    if (!byte(head))
      return false;
    element = head & 0x0FU;
    uint64_t n = head >> 4U;
    if (n == 15 && !varint(n))
      return false;
    if (!valid_type(element) || n > remaining())
      return false;
    length = static_cast<size_t>(n);
    return true;
  }

  bool binary(const uint8_t *&ptr, size_t &length) {
    uint64_t n = 0;
    if (!varint(n) || n > remaining())
      return false;
    ptr = data_ + pos_;
    length = static_cast<size_t>(n);
    pos_ += length;
    return true;
  }

  // Skip one value. Booleans are carried in a struct field header but take
  // one byte each inside a list, set or map.
  bool skip(uint8_t type, size_t depth, bool element = false) {
    if (depth > kMaxDepth)
      return false;
    switch (type) {
    case kTrue:
    case kFalse: {
      uint8_t unused = 0;
      return !element || byte(unused);
    }
    case kByte: {
      uint8_t unused = 0;
      return byte(unused);
    }
    case kI16:
    case kI32:
    case kI64: {
      uint64_t unused = 0;
      return varint(unused);
    }
    case kDouble:
      if (remaining() < 8)
        return false;
      pos_ += 8;
      return true;
    case kBinary: {
      const uint8_t *ptr = nullptr;
      size_t length = 0;
      return binary(ptr, length);
    }
    case kList:
    case kSet: {
      uint8_t element_type = 0;
      size_t length = 0;
      if (!list(element_type, length))
        return false;
      for (size_t i = 0; i < length; ++i)
        if (!skip(element_type, depth + 1, true))
          return false;
      return true;
    }
    case kMap: {
      uint64_t n = 0;
      if (!varint(n))
        return false;
      if (n == 0)
        return true;
      uint8_t types = 0;
      if (!byte(types) || n > remaining() / 2)
        return false;
      const uint8_t key = types >> 4U, value = types & 0x0FU;
      if (!valid_type(key) || !valid_type(value))
        return false;
      for (uint64_t i = 0; i < n; ++i)
        if (!skip(key, depth + 1, true) || !skip(value, depth + 1, true))
          return false;
      return true;
    }
    case kStruct: {
      int16_t last = 0, id = 0;
      for (uint8_t field_type = 0;;) {
        if (!field(last, id, field_type))
          return false;
        if (field_type == kStop)
          return true;
        if (!skip(field_type, depth + 1))
          return false;
      }
    }
    default:
      return false;
    }
  }

private:
  const uint8_t *data_;
  size_t size_;
  size_t pos_ = 0;
};

// LogicalType union. Only TIMESTAMP(MICROS, isAdjustedToUTC=true) maps to an
// Iceberg type this reader reports; any other annotation is refused rather
// than silently read as a plain number.
bool decode_logical(Decoder &in, LogicalType &logical) {
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  bool decoded = false;
  for (;;) {
    if (!in.field(last, id, type))
      return false;
    if (type == kStop)
      return decoded;
    if (decoded || id != 8 || type != kStruct)
      return false;
    bool utc = false, micros = false, has_utc = false, has_unit = false;
    int16_t ts_last = 0, ts_id = 0;
    for (uint8_t ts_type = 0;;) {
      if (!in.field(ts_last, ts_id, ts_type))
        return false;
      if (ts_type == kStop)
        break;
      if (ts_id == 1 && (ts_type == kTrue || ts_type == kFalse) && !has_utc) {
        utc = ts_type == kTrue;
        has_utc = true;
      } else if (ts_id == 2 && ts_type == kStruct && !has_unit) {
        int16_t unit_last = 0, unit_id = 0;
        uint8_t unit_type = 0;
        if (!in.field(unit_last, unit_id, unit_type) || unit_type != kStruct)
          return false;
        micros = unit_id == 2;
        int16_t end_last = 0, end_id = 0;
        uint8_t end = 0;
        if (!in.skip(kStruct, 1) || !in.field(end_last, end_id, end) ||
            end != kStop)
          return false;
        has_unit = true;
      } else if (!in.skip(ts_type, 1)) {
        return false;
      }
    }
    if (!has_utc || !has_unit || !utc || !micros)
      return false;
    logical = LogicalType::TimestampMicrosUtc;
    decoded = true;
  }
}

bool decode_root(Decoder &in, size_t &children) {
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  uint32_t seen = 0;
  bool has_name = false, has_children = false;
  for (;;) {
    if (!in.field(last, id, type))
      return false;
    if (type == kStop)
      return has_name && has_children;
    if (!once(seen, id) || id == 1) // a physical type makes it a leaf
      return false;
    if (id == 4 && type == kBinary) {
      has_name = in.skip(kBinary, 0);
      if (!has_name)
        return false;
    } else if (id == 5 && type == kI32) {
      int32_t n = 0;
      if (!in.i32(n) || n < 1 || static_cast<size_t>(n) > kMaxColumns)
        return false;
      children = static_cast<size_t>(n);
      has_children = true;
    } else if (!in.skip(type, 0)) {
      return false;
    }
  }
}

bool decode_leaf(Decoder &in, SchemaColumn &column) {
  column = SchemaColumn{};
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  uint32_t seen = 0;
  bool has_type = false, has_repetition = false, has_name = false;
  int32_t converted = -1;
  for (;;) {
    if (!in.field(last, id, type))
      return false;
    if (type == kStop)
      break;
    if (!once(seen, id))
      return false;
    int32_t value = 0;
    if (id == 1 && type == kI32) {
      if (!in.i32(value) || (value != 1 && value != 2 && value != 4))
        return false;
      column.type = static_cast<PhysicalType>(value);
      has_type = true;
    } else if (id == 3 && type == kI32) {
      if (!in.i32(value) || (value != 0 && value != 1)) // no REPEATED
        return false;
      column.nullable = value == 1;
      has_repetition = true;
    } else if (id == 4 && type == kBinary) {
      const uint8_t *name = nullptr;
      size_t length = 0;
      if (!in.binary(name, length) || !length || length >= kMaxNameLength)
        return false;
      memcpy(column.name, name, length);
      column.name[length] = '\0';
      has_name = true;
    } else if (id == 5) { // num_children: nested groups are not flat
      return false;
    } else if (id == 6 && type == kI32) {
      if (!in.i32(converted))
        return false;
    } else if (id == 9 && type == kI32) {
      if (!in.i32(value) || value <= 0)
        return false;
      column.field_id = value;
    } else if (id == 10 && type == kStruct) {
      if (!decode_logical(in, column.logical))
        return false;
    } else if (!in.skip(type, 0)) {
      return false;
    }
  }
  const bool timestamp = column.logical == LogicalType::TimestampMicrosUtc;
  if (!has_type || !has_repetition || !has_name ||
      (timestamp && column.type != PhysicalType::Int64))
    return false;
  // A legacy ConvertedType alone has no UTC flag; accept only the one that
  // matches the logical type already decoded.
  return converted < 0 || (converted == kTimestampMicros && timestamp);
}

bool decode_statistics(Decoder &in, PhysicalType physical,
                       ColumnChunkSummary &chunk) {
  const size_t width = type_width(physical);
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  uint32_t seen = 0;
  bool has_min = false, has_max = false, has_nan = false;
  uint64_t nan_count = 0;
  for (;;) {
    if (!in.field(last, id, type))
      return false;
    if (type == kStop)
      break;
    if (!once(seen, id))
      return false;
    if (id == 3 && type == kI64) {
      if (!in.count(chunk.null_count))
        return false;
      chunk.has_null_count = true;
    } else if ((id == 5 || id == 6) && type == kBinary) {
      // min_value/max_value; deprecated min/max (1, 2) use a signed byte
      // order on some writers and are skipped.
      const uint8_t *bytes = nullptr;
      size_t length = 0;
      if (!in.binary(bytes, length) || length != width)
        return false;
      (id == 5 ? chunk.max : chunk.min) = load_le(bytes, width);
      (id == 5 ? has_max : has_min) = true;
    } else if (id == 9 && type == kI64) {
      if (!in.count(nan_count))
        return false;
      has_nan = true;
    } else if (!in.skip(type, 0)) {
      return false;
    }
  }
  if (has_min != has_max)
    return false;
  if (has_min) {
    if (physical == PhysicalType::Float &&
        (as_float(chunk.min) != as_float(chunk.min) ||
         as_float(chunk.max) != as_float(chunk.max)))
      return false; // NaN is never a valid bound
    if (less(physical, chunk.max, chunk.min))
      return false;
  }
  chunk.has_bounds = has_min;
  if (physical == PhysicalType::Float && has_nan) {
    chunk.nan_count = nan_count;
    chunk.has_nan_count = true;
  }
  return true;
}

bool decode_column_meta(Decoder &in, const SchemaColumn &column,
                        ColumnChunkSummary &chunk) {
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  uint32_t seen = 0;
  for (;;) {
    if (!in.field(last, id, type))
      return false;
    if (type == kStop)
      break;
    if (!once(seen, id))
      return false;
    int32_t value = 0;
    if (id == 1 && type == kI32) {
      if (!in.i32(value) || value != static_cast<int32_t>(column.type))
        return false;
    } else if (id == 3 && type == kList) {
      uint8_t element = 0;
      size_t length = 0;
      const uint8_t *name = nullptr;
      size_t name_length = 0;
      if (!in.list(element, length) || element != kBinary || length != 1 ||
          !in.binary(name, name_length) || name_length != strlen(column.name) ||
          memcmp(name, column.name, name_length) != 0)
        return false;
    } else if (id == 4 && type == kI32) {
      if (!in.i32(value) || value < 0 || value > 7)
        return false;
      chunk.codec = static_cast<Codec>(value);
    } else if (id == 5 && type == kI64) {
      if (!in.count(chunk.num_values))
        return false;
    } else if (id == 6 && type == kI64) {
      if (!in.count(chunk.total_uncompressed_size))
        return false;
    } else if (id == 7 && type == kI64) {
      if (!in.count(chunk.total_compressed_size))
        return false;
    } else if (id == 9 && type == kI64) {
      if (!in.count(chunk.data_page_offset))
        return false;
    } else if (id == 11 && type == kI64) {
      if (!in.count(chunk.start_offset))
        return false;
    } else if (id == 12 && type == kStruct) {
      if (!decode_statistics(in, column.type, chunk))
        return false;
    } else if (!in.skip(type, 0)) {
      return false;
    }
  }
  // type, path_in_schema, codec, num_values, both sizes, data_page_offset.
  constexpr uint32_t required = (1U << 1) | (1U << 3) | (1U << 4) | (1U << 5) |
                                (1U << 6) | (1U << 7) | (1U << 9);
  if ((seen & required) != required)
    return false;
  if (!(seen & (1U << 11)) || chunk.start_offset > chunk.data_page_offset)
    chunk.start_offset = chunk.data_page_offset;
  return true;
}

bool decode_column_chunk(Decoder &in, const SchemaColumn &column,
                         ColumnChunkSummary &chunk) {
  chunk = ColumnChunkSummary{};
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  uint32_t seen = 0;
  bool has_meta = false;
  for (;;) {
    if (!in.field(last, id, type))
      return false;
    if (type == kStop)
      return has_meta;
    if (!once(seen, id) || id == 1) // file_path: chunk lives in another file
      return false;
    if (id == 3 && type == kStruct) {
      if (!decode_column_meta(in, column, chunk))
        return false;
      has_meta = true;
    } else if (!in.skip(type, 0)) {
      return false;
    }
  }
}

// The chunk must lie inside the data region and agree with its row group.
bool check_chunk(const ColumnChunkSummary &chunk, uint64_t rows,
                 uint64_t data_end) {
  if (chunk.num_values != rows || chunk.start_offset < 4 ||
      chunk.total_compressed_size == 0 || chunk.start_offset > data_end ||
      chunk.total_compressed_size > data_end - chunk.start_offset ||
      chunk.data_page_offset - chunk.start_offset >=
          chunk.total_compressed_size)
    return false;
  if (chunk.has_null_count && chunk.null_count > rows)
    return false;
  const uint64_t present =
      chunk.has_null_count ? rows - chunk.null_count : rows;
  return !chunk.has_nan_count || chunk.nan_count <= present;
}

bool decode_row_group(Decoder &in, const FileSummary &file,
                      RowGroupSummary &group, uint64_t data_end) {
  group.num_rows = group.file_offset = group.total_byte_size = 0;
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  uint32_t seen = 0;
  for (;;) {
    if (!in.field(last, id, type))
      return false;
    if (type == kStop)
      break;
    if (!once(seen, id))
      return false;
    if (id == 1 && type == kList) {
      uint8_t element = 0;
      size_t length = 0;
      if (!in.list(element, length) || element != kStruct ||
          length != file.column_count)
        return false;
      for (size_t i = 0; i < length; ++i)
        if (!decode_column_chunk(in, file.columns[i], group.columns[i]))
          return false;
    } else if (id == 2 && type == kI64) {
      if (!in.count(group.total_byte_size))
        return false;
    } else if (id == 3 && type == kI64) {
      if (!in.count(group.num_rows))
        return false;
    } else if (id == 5 && type == kI64) {
      if (!in.count(group.file_offset))
        return false;
    } else if (!in.skip(type, 0)) {
      return false;
    }
  }
  constexpr uint32_t required = (1U << 1) | (1U << 2) | (1U << 3);
  if ((seen & required) != required)
    return false;
  for (size_t i = 0; i < file.column_count; ++i)
    if (!check_chunk(group.columns[i], group.num_rows, data_end))
      return false;
  return true;
}

Result decode_footer(const uint8_t *footer, size_t length, uint64_t file_size,
                     FileSummary &summary) {
  // Reset in place: FileSummary is far too large for a stack temporary.
  summary.version = 0;
  summary.num_rows = 0;
  summary.column_count = 0;
  summary.row_group_count = 0;
  summary.file_size = file_size;
  summary.footer_size = static_cast<uint32_t>(length);
  const uint64_t data_end = file_size - 8 - length;
  Decoder in(footer, length);
  int16_t last = 0, id = 0;
  uint8_t type = 0;
  uint32_t seen = 0;
  for (;;) {
    if (!in.field(last, id, type))
      return fail("corrupt footer field");
    if (type == kStop)
      break;
    if (!once(seen, id))
      return fail("repeated footer field");
    if (id == 1 && type == kI32) {
      if (!in.i32(summary.version))
        return fail("corrupt footer version");
    } else if (id == 2 && type == kList) {
      uint8_t element = 0;
      size_t count = 0, children = 0;
      if (!in.list(element, count) || element != kStruct || count < 2 ||
          !decode_root(in, children) || children != count - 1)
        return fail("schema is not one flat root group");
      for (size_t i = 0; i < children; ++i)
        if (!decode_leaf(in, summary.columns[i]))
          return fail("unsupported or corrupt schema leaf");
      summary.column_count = children;
    } else if (id == 3 && type == kI64) {
      if (!in.count(summary.num_rows))
        return fail("corrupt footer num_rows");
    } else if (id == 4 && type == kList) {
      uint8_t element = 0;
      size_t count = 0;
      if (!summary.column_count)
        return fail("row groups precede the schema");
      if (!in.list(element, count) || element != kStruct ||
          count > kMaxRowGroups)
        return fail("row group list corrupt or over capacity");
      for (size_t g = 0; g < count; ++g)
        if (!decode_row_group(in, summary, summary.row_groups[g], data_end))
          return fail("corrupt or inconsistent row group");
      summary.row_group_count = count;
    } else if (id == 8 || id == 9) {
      return fail("encrypted footer metadata is not supported");
    } else if (!in.skip(type, 0)) {
      return fail("corrupt footer field");
    }
  }
  constexpr uint32_t required = (1U << 1) | (1U << 2) | (1U << 3) | (1U << 4);
  if ((seen & required) != required || in.pos() != length)
    return fail("incomplete footer or trailing bytes");
  uint64_t rows = 0;
  for (size_t g = 0; g < summary.row_group_count; ++g)
    rows += summary.row_groups[g].num_rows; // each <= INT64_MAX, at most 8
  if (rows != summary.num_rows)
    return fail("row group rows do not sum to num_rows");
  return {true, length, nullptr};
}

uint32_t load_u32(const uint8_t *bytes) {
  return static_cast<uint32_t>(load_le(bytes, 4));
}

} // namespace

Result read_parquet_footer(const uint8_t *file_bytes, size_t file_size,
                           FileSummary &summary) {
  summary.column_count = summary.row_group_count = 0;
  if (!file_bytes || file_size < 12)
    return fail("file too small for Parquet");
  if (memcmp(file_bytes, "PAR1", 4) != 0 ||
      memcmp(file_bytes + file_size - 4, "PAR1", 4) != 0)
    return fail("missing PAR1 magic");
  const uint32_t length = load_u32(file_bytes + file_size - 8);
  if (length == 0 || length > file_size - 12)
    return fail("invalid footer length");
  return decode_footer(file_bytes + file_size - 8 - length, length, file_size,
                       summary);
}

Result read_parquet_footer(ReadAt read_at, void *context, uint64_t file_size,
                           uint8_t *footer_scratch, size_t scratch_capacity,
                           FileSummary &summary) {
  summary.column_count = summary.row_group_count = 0;
  if (!read_at || !footer_scratch || file_size < 12)
    return fail("invalid reader arguments");
  uint8_t head[4]{};
  uint8_t tail[8]{};
  if (!read_at(context, 0, head, sizeof(head)) ||
      !read_at(context, file_size - 8, tail, sizeof(tail)))
    return fail("read failed");
  if (memcmp(head, "PAR1", 4) != 0 || memcmp(tail + 4, "PAR1", 4) != 0)
    return fail("missing PAR1 magic");
  const uint32_t length = load_u32(tail);
  if (length == 0 || length > file_size - 12)
    return fail("invalid footer length");
  if (length > scratch_capacity)
    return fail("footer larger than scratch");
  if (!read_at(context, file_size - 8 - length, footer_scratch, length))
    return fail("read failed");
  return decode_footer(footer_scratch, length, file_size, summary);
}

Result iceberg_data_file_metrics(const FileSummary &file,
                                 IcebergDataFileMetrics &metrics) {
  metrics.column_count = metrics.split_offset_count = 0;
  if (!file.column_count || file.column_count > kMaxColumns ||
      file.row_group_count > kMaxRowGroups)
    return fail("summary holds no decoded footer");
  metrics.record_count = file.num_rows;
  metrics.file_size_in_bytes = file.file_size;
  for (size_t g = 0; g < file.row_group_count; ++g) {
    // Iceberg split offsets are row group starts: the first chunk byte.
    const RowGroupSummary &group = file.row_groups[g];
    uint64_t start = group.columns[0].start_offset;
    for (size_t c = 1; c < file.column_count; ++c)
      if (group.columns[c].start_offset < start)
        start = group.columns[c].start_offset;
    metrics.split_offsets[g] = start;
  }
  metrics.split_offset_count = file.row_group_count;
  for (size_t c = 0; c < file.column_count; ++c) {
    const SchemaColumn &schema = file.columns[c];
    if (schema.field_id <= 0)
      return fail("column has no field_id");
    for (size_t prior = 0; prior < c; ++prior)
      if (file.columns[prior].field_id == schema.field_id)
        return fail("duplicate field_id");
    IcebergColumnMetrics &out = metrics.columns[c];
    out = IcebergColumnMetrics{};
    out.field_id = schema.field_id;
    out.type = schema.type;
    out.logical = schema.logical;
    out.has_null_value_count = true;
    out.has_nan_value_count = schema.type == PhysicalType::Float;
    bool bounds_known = true, merged = false;
    uint64_t low = 0, high = 0;
    for (size_t g = 0; g < file.row_group_count; ++g) {
      const ColumnChunkSummary &chunk = file.row_groups[g].columns[c];
      out.column_size += chunk.total_compressed_size;
      out.value_count += chunk.num_values;
      out.null_value_count += chunk.null_count;
      out.nan_value_count += chunk.nan_count;
      out.has_null_value_count &= chunk.has_null_count;
      out.has_nan_value_count &= chunk.has_nan_count;
      if (chunk.has_bounds) {
        if (!merged || less(schema.type, chunk.min, low))
          low = chunk.min;
        if (!merged || less(schema.type, high, chunk.max))
          high = chunk.max;
        merged = true;
        continue;
      }
      // No bounds is fine only when the chunk provably has no non-null,
      // non-NaN value; otherwise any file bound would be a guess.
      if (!chunk.has_null_count)
        bounds_known = false;
      else {
        const uint64_t present = chunk.num_values - chunk.null_count;
        const uint64_t nan = chunk.has_nan_count ? chunk.nan_count : 0;
        if (present != nan)
          bounds_known = false;
      }
    }
    if (!out.has_null_value_count)
      out.null_value_count = 0;
    if (!out.has_nan_value_count)
      out.nan_value_count = 0;
    if (merged && bounds_known) {
      if (schema.type == PhysicalType::Float) {
        // Widen a zero bound to cover both signed zeros.
        if (as_float(low) == 0.0F)
          low = float_bits(-0.0F);
        if (as_float(high) == 0.0F)
          high = float_bits(0.0F);
      }
      out.has_bounds = true;
      out.bound_size = static_cast<uint8_t>(type_width(schema.type));
      store_le(out.lower_bound, low, out.bound_size);
      store_le(out.upper_bound, high, out.bound_size);
    }
  }
  metrics.column_count = file.column_count;
  return {true, 0, nullptr};
}

} // namespace telemetry
