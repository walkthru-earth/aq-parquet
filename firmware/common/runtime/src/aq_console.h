#pragma once

#include <cstddef>
#include <cstdint>

// Physical console only. Provisioning replies must bypass the diagnostic ring.
namespace aq::console {
bool begin();
int read(); // nonblocking, -1 when no byte is available
std::size_t write(const std::uint8_t *data, std::size_t size);
void flush();
int printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
} // namespace aq::console
