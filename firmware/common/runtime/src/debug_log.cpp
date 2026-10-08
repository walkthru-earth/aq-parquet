#include "debug_log.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

#include <cstring>

namespace {
portMUX_TYPE ring_mutex = portMUX_INITIALIZER_UNLOCKED;
} // namespace

DebugLog aqlog;
DebugLog::DebugLog() : output_(&Serial) {}

size_t DebugLog::write(uint8_t byte) { return write(&byte, 1); }

size_t DebugLog::write(const uint8_t *buffer, size_t size) {
  if (write_record_only(buffer, size) == 0)
    return 0;
  return output_->write(buffer, size);
}

size_t DebugLog::write_record_only(const uint8_t *buffer, size_t size) {
  if (!buffer || size == 0)
    return 0;
  // Keep only the newest kRingBytes of an oversized write.
  const uint8_t *source = buffer;
  std::size_t count = size;
  if (count > kRingBytes) {
    source += count - kRingBytes;
    count = kRingBytes;
  }
  portENTER_CRITICAL(&ring_mutex);
  const std::size_t first =
      kRingBytes - head_ < count ? kRingBytes - head_ : count;
  std::memcpy(ring_ + head_, source, first);
  if (count > first)
    std::memcpy(ring_, source + first, count - first);
  head_ = (head_ + count) % kRingBytes;
  total_ += static_cast<std::uint32_t>(size);
  portEXIT_CRITICAL(&ring_mutex);
  return size;
}

std::size_t DebugLog::tail(char *out, std::size_t max,
                           std::uint32_t &total) const {
  portENTER_CRITICAL(&ring_mutex);
  total = total_;
  const std::size_t available = total_ < kRingBytes ? total_ : kRingBytes;
  const std::size_t count = available < max ? available : max;
  // Newest `count` bytes end at head_.
  const std::size_t start = (head_ + kRingBytes - count) % kRingBytes;
  const std::size_t first =
      kRingBytes - start < count ? kRingBytes - start : count;
  std::memcpy(out, ring_ + start, first);
  if (count > first)
    std::memcpy(out + first, ring_, count - first);
  portEXIT_CRITICAL(&ring_mutex);
  return count;
}
