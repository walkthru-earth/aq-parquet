#pragma once
#include <cstddef>
#include <cstdint>

namespace fake_platform {
inline std::uint32_t random_state = 71;
}
inline std::uint32_t esp_random() {
  fake_platform::random_state =
      fake_platform::random_state * 1664525U + 1013904223U;
  return fake_platform::random_state;
}
inline void esp_fill_random(void *output, std::size_t size) {
  auto *bytes = static_cast<std::uint8_t *>(output);
  for (std::size_t i = 0; i < size; ++i)
    bytes[i] = static_cast<std::uint8_t>(esp_random() >> 24);
}
