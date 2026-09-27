#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// The platform surface used by the real DebugLog/Settings implementation.
class Print {
public:
  virtual ~Print() = default;
  virtual std::size_t write(std::uint8_t byte) = 0;
  virtual std::size_t write(const std::uint8_t *data, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i)
      write(data[i]);
    return size;
  }
  std::size_t println(const char *text) {
    const auto length = std::strlen(text);
    return write(reinterpret_cast<const std::uint8_t *>(text), length) +
           write(static_cast<std::uint8_t>('\n'));
  }
  std::size_t printf(const char *format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (length <= 0)
      return 0;
    const auto count = static_cast<std::size_t>(length) < sizeof(text)
                           ? static_cast<std::size_t>(length)
                           : sizeof(text) - 1;
    return write(reinterpret_cast<const std::uint8_t *>(text), count);
  }
};
