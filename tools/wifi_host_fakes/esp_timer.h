#pragma once
#include <cstdint>
inline std::int64_t fake_now_us = 1000000;
inline std::int64_t esp_timer_get_time() { return fake_now_us; }
