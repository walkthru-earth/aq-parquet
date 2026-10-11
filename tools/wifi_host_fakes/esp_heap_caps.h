#pragma once
#include <cstdint>
#include <cstdlib>
constexpr std::uint32_t MALLOC_CAP_8BIT = 1U << 2;
constexpr std::uint32_t MALLOC_CAP_SPIRAM = 1U << 10;
constexpr std::uint32_t MALLOC_CAP_INTERNAL = 1U << 11;
// Tests may refuse PSRAM to exercise the internal ring fallback.
inline bool fake_heap_no_spiram = false;
inline void *heap_caps_malloc(std::size_t size, std::uint32_t caps) {
  if ((caps & MALLOC_CAP_SPIRAM) && fake_heap_no_spiram)
    return nullptr;
  return std::malloc(size);
}
inline void heap_caps_free(void *pointer) { std::free(pointer); }
