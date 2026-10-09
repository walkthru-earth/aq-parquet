#pragma once
#include <cstddef>
#include <cstdint>

// This adapter owns V2 UART1, SDMMC and the optional gauge/RGB controllers.
// It is deliberately local to the Waveshare trial; shared services borrow
// hooks.
namespace board {
bool start_sensor();
int read_sensor();
bool mount_card();
std::uint64_t card_bytes();
std::uint64_t storage_total_bytes();
std::uint64_t storage_used_bytes();
bool start_gauge();
bool read_gauge_word(std::uint8_t reg, std::uint16_t &value, int &error);
bool start_indicator();
void indicator(std::uint8_t red, std::uint8_t green, std::uint8_t blue);
} // namespace board
