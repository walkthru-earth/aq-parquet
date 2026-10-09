#pragma once

#include <M5Unified.h>
#include <aq_ltr553.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace telemetry {

using Ltr553Reading = aq::Ltr553Reading;

// The CoreS3 adapter borrows M5Unified's initialized internal I2C controller.
// Call from the task that owns M5.update(), after M5.begin().
class Ltr553 : public aq::Ltr553 {
public:
  Ltr553() : aq::Ltr553({nullptr, read_register, write_register, wait, now}) {}

private:
  static bool read_register(void *, std::uint8_t address, std::uint8_t reg,
                            std::uint8_t *data, std::size_t size) {
    // readRegister8 cannot report an I2C error; keep the status-returning API.
    return M5.In_I2C.readRegister(address, reg, data, size, kFrequency);
  }
  static bool write_register(void *, std::uint8_t address, std::uint8_t reg,
                             std::uint8_t value) {
    return M5.In_I2C.writeRegister8(address, reg, value, kFrequency);
  }
  static void wait(void *, std::uint32_t milliseconds) {
    const auto ticks = pdMS_TO_TICKS(milliseconds);
    vTaskDelay(ticks ? ticks : 1);
  }
  static std::uint32_t now(void *) {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
  }
  static constexpr std::uint32_t kFrequency = 100000;
};

} // namespace telemetry
