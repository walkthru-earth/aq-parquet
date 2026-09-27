#pragma once
#include "parquet_writer.h"
#define LZ4_MEMORY_USAGE 12
#include "../vendor/lz4/lz4.h"

namespace telemetry {
// 90 optional INT64 rows need at most 904 bytes including definition runs.
// The aligned, caller-owned LZ4 state avoids hidden heap/large stack use.
struct Lz4Workspace {
  LZ4_stream_t state{};
  uint8_t raw[1024]{};
  uint8_t encoded[LZ4_COMPRESSBOUND(1024)]{};
  uint64_t codec_us = 0;
  Compression configuration();
};
} // namespace telemetry
