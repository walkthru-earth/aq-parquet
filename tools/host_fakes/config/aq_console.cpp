#include <aq_console.h>

namespace aq::console {
bool begin() { return true; }
int read() { return -1; }
std::size_t write(const std::uint8_t *, std::size_t size) { return size; }
void flush() {}
int printf(const char *, ...) { return 0; }
} // namespace aq::console
