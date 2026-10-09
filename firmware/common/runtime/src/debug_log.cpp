#include "debug_log.h"

#include "aq_console.h"
#include <freertos/FreeRTOS.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
portMUX_TYPE ring_mutex = portMUX_INITIALIZER_UNLOCKED;
} // namespace

DebugLog aqlog;
DebugLog::DebugLog()
    : output_([](void *, const std::uint8_t *data, std::size_t size) {
        return aq::console::write(data, size);
      }) {}

std::size_t LogOutput::print(const char *text) {
  return text ? write(reinterpret_cast<const std::uint8_t *>(text),
                      std::strlen(text))
              : 0;
}

std::size_t LogOutput::println(const char *text) {
  return print(text) + write(static_cast<std::uint8_t>('\n'));
}

int LogOutput::printf(const char *format, ...) {
  char local[256];
  va_list args;
  va_start(args, format);
  va_list measured;
  va_copy(measured, args);
  const int length = std::vsnprintf(local, sizeof(local), format, measured);
  va_end(measured);
  if (length <= 0) {
    va_end(args);
    return length;
  }
  char *text = local;
  if (static_cast<std::size_t>(length) >= sizeof(local)) {
    text =
        static_cast<char *>(std::malloc(static_cast<std::size_t>(length) + 1));
    if (!text) {
      va_end(args);
      return -1;
    }
    std::vsnprintf(text, static_cast<std::size_t>(length) + 1, format, args);
  }
  va_end(args);
  const auto written =
      write(reinterpret_cast<const std::uint8_t *>(text), length);
  if (text != local)
    std::free(text);
  return static_cast<int>(written);
}

size_t DebugLog::write(const uint8_t *buffer, size_t size) {
  if (write_record_only(buffer, size) == 0)
    return 0;
  return output_ ? output_(output_context_, buffer, size) : size;
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
