#include "board_io.h"
#include <driver/i2c_master.h>
#include <driver/sdmmc_host.h>
#include <driver/uart.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <led_strip.h>
#include <sdmmc_cmd.h>

namespace board {
namespace {
sdmmc_card_t *card = nullptr;
i2c_master_bus_handle_t gauge_bus = nullptr;
i2c_master_dev_handle_t gauge = nullptr;
led_strip_handle_t led = nullptr;
bool sensor_ready = false;
std::uint64_t cached_total_bytes = 0;
std::uint64_t cached_used_bytes = 0;

void refresh_capacity() {
  if (!card)
    return;
  std::uint64_t total = 0, free = 0;
  if (esp_vfs_fat_info("/sd", &total, &free) == ESP_OK && free <= total) {
    cached_total_bytes = total;
    cached_used_bytes = total - free;
  }
}
} // namespace
bool start_sensor() {
  if (sensor_ready)
    return true;
  uart_config_t config{};
  config.baud_rate = 9600;
  config.data_bits = UART_DATA_8_BITS;
  config.parity = UART_PARITY_DISABLE;
  config.stop_bits = UART_STOP_BITS_1;
  config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  config.source_clk = UART_SCLK_DEFAULT;
  sensor_ready =
      uart_param_config(UART_NUM_1, &config) == ESP_OK &&
      uart_set_pin(UART_NUM_1, 2, 1, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) ==
          ESP_OK &&
      uart_driver_install(UART_NUM_1, 1024, 0, 0, nullptr, 0) == ESP_OK;
  return sensor_ready;
}
int read_sensor() {
  std::uint8_t value;
  return sensor_ready && uart_read_bytes(UART_NUM_1, &value, 1, 0) == 1 ? value
                                                                        : -1;
}
bool mount_card() {
  if (card)
    return true;
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_1BIT;
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1;
  slot.clk = GPIO_NUM_5;
  slot.cmd = GPIO_NUM_4;
  slot.d0 = GPIO_NUM_6;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
  esp_vfs_fat_sdmmc_mount_config_t config{};
  config.format_if_mount_failed = false;
  config.max_files = 5;
  sdmmc_card_t *mounted = nullptr;
  const esp_err_t result =
      esp_vfs_fat_sdmmc_mount("/sd", &host, &slot, &config, &mounted);
  // IDF mount helper releases its host/FAT resources on failure.
  if (result != ESP_OK)
    return false;
  card = mounted;
  return true;
}
std::uint64_t card_bytes() {
  return card ? static_cast<std::uint64_t>(card->csd.capacity) *
                    card->csd.sector_size
              : 0;
}
std::uint64_t storage_total_bytes() {
  refresh_capacity();
  return cached_total_bytes;
}
std::uint64_t storage_used_bytes() {
  refresh_capacity();
  return cached_used_bytes;
}
bool start_gauge() {
  if (gauge_bus && gauge)
    return true;
  i2c_master_bus_config_t config{};
  config.i2c_port = I2C_NUM_0;
  config.sda_io_num = GPIO_NUM_15;
  config.scl_io_num = GPIO_NUM_16;
  config.clk_source = I2C_CLK_SRC_DEFAULT;
  config.glitch_ignore_cnt = 7;
  config.flags.enable_internal_pullup = true;
  i2c_master_bus_handle_t created_bus = nullptr;
  if (i2c_new_master_bus(&config, &created_bus) != ESP_OK)
    return false;
  i2c_device_config_t device{};
  device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  device.device_address = 0x36;
  device.scl_speed_hz = 100000;
  i2c_master_dev_handle_t created_gauge = nullptr;
  if (i2c_master_bus_add_device(created_bus, &device, &created_gauge) !=
      ESP_OK) {
    i2c_del_master_bus(created_bus);
    return false;
  }
  gauge_bus = created_bus;
  gauge = created_gauge;
  return true;
}
bool read_gauge_word(std::uint8_t reg, std::uint16_t &value, int &error) {
  std::uint8_t data[2]{};
  error = gauge ? i2c_master_transmit_receive(gauge, &reg, 1, data, 2, 20)
                : ESP_ERR_INVALID_STATE;
  if (error != ESP_OK)
    return false;
  value = (static_cast<std::uint16_t>(data[0]) << 8) | data[1];
  return true;
}
bool start_indicator() {
  if (led)
    return true;
  led_strip_config_t config{};
  config.strip_gpio_num = 38;
  config.max_leds = 1;
  config.led_model = LED_MODEL_WS2812;
  config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
  led_strip_rmt_config_t rmt{};
  rmt.clk_src = RMT_CLK_SRC_DEFAULT;
  rmt.resolution_hz = 10000000;
  led_strip_handle_t created_led = nullptr;
  if (led_strip_new_rmt_device(&config, &rmt, &created_led) != ESP_OK)
    return false;
  led = created_led;
  return true;
}
void indicator(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
  if (led && led_strip_set_pixel(led, 0, red, green, blue) == ESP_OK)
    led_strip_refresh(led);
}
} // namespace board
