// Upstream implementation calls its own compatibility APIs. As in lz4.c,
// disable their deprecation attributes only in this implementation unit.
#define LZ4_DISABLE_DEPRECATE_WARNINGS
#include "lz4_codec.h"
#ifdef ARDUINO
#include <esp_timer.h>
#else
#include <chrono>
#endif

// Compile exactly the pinned upstream source with the same state-size setting.
#include "../vendor/lz4/lz4.c"

namespace telemetry {
namespace {
uint64_t micros_now() {
#ifdef ARDUINO
  return esp_timer_get_time();
#else
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
#endif
}
size_t compress(void *context, const uint8_t *source, size_t size,
                uint8_t *destination, size_t capacity) {
  auto &workspace = *static_cast<Lz4Workspace *>(context);
  if (size > INT32_MAX || capacity > INT32_MAX)
    return 0;
  const auto start = micros_now();
  const int result = LZ4_compress_fast_extState(
      &workspace.state, reinterpret_cast<const char *>(source),
      reinterpret_cast<char *>(destination), static_cast<int>(size),
      static_cast<int>(capacity), 1);
  workspace.codec_us += micros_now() - start;
  return result > 0 ? static_cast<size_t>(result) : 0;
}
} // namespace
Compression Lz4Workspace::configuration() {
  return {Codec::Lz4Raw,   raw,      sizeof(raw), encoded,
          sizeof(encoded), compress, this};
}
} // namespace telemetry
