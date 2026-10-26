#pragma once
#include "parquet_writer.h"
#define LZ4_MEMORY_USAGE 12
#include "../vendor/lz4/lz4.h"

namespace telemetry {
// One page of at most kLz4PageRows optional rows. The widest PLAIN value is a
// GEOMETRY point, a 4 byte length and a 21 byte WKB Point (#83). Each row
// adds at most one definition run of a varint header and a value byte, and
// the runs carry a 4 byte length, so 90 rows need at most 2524 bytes. The
// aligned, caller-owned LZ4 state avoids hidden heap/large stack use.
inline constexpr size_t kLz4PageRows = 90;
inline constexpr size_t kLz4WidestValueBytes = 4 + 21;
inline constexpr size_t kLz4RawBytes =
    4 + kLz4PageRows * (2 + 1 + kLz4WidestValueBytes);
struct Lz4Workspace {
  LZ4_stream_t state{};
  uint8_t raw[kLz4RawBytes]{};
  uint8_t encoded[LZ4_COMPRESSBOUND(kLz4RawBytes)]{};
  uint64_t codec_us = 0;
  Compression configuration();
};
} // namespace telemetry
