#include "aq_console.h"

#include <sdkconfig.h>
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#else
#include <driver/uart.h>
#include <driver/uart_vfs.h>
#endif
#include <freertos/FreeRTOS.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace aq::console {
namespace {
bool ready = false;
#if !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
constexpr uart_port_t port =
    static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);
#endif
} // namespace

bool begin() {
  if (ready)
    return true;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  usb_serial_jtag_driver_config_t config{};
  config.tx_buffer_size = 4096;
  config.rx_buffer_size = 1024;
  ready = usb_serial_jtag_driver_install(&config) == ESP_OK;
  if (ready)
    usb_serial_jtag_vfs_use_driver();
#else
  uart_config_t config{};
  config.baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE;
  config.data_bits = UART_DATA_8_BITS;
  config.parity = UART_PARITY_DISABLE;
  config.stop_bits = UART_STOP_BITS_1;
  config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  config.source_clk = UART_SCLK_DEFAULT;
  ready = uart_param_config(port, &config) == ESP_OK &&
          (uart_is_driver_installed(port) ||
           uart_driver_install(port, 2048, 0, 0, nullptr, 0) == ESP_OK);
  if (ready)
    uart_vfs_dev_use_driver(port);
#endif
  return ready;
}

int read() {
  if (!ready)
    return -1;
  std::uint8_t byte;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  const int count = usb_serial_jtag_read_bytes(&byte, 1, 0);
#else
  const int count = uart_read_bytes(port, &byte, 1, 0);
#endif
  return count == 1 ? byte : -1;
}

std::size_t write(const std::uint8_t *data, std::size_t size) {
  if (!ready || !data || size == 0)
    return 0;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  // A detached USB reader must not stall the archive worker indefinitely.
  const int written =
      usb_serial_jtag_write_bytes(data, size, pdMS_TO_TICKS(50));
#else
  const int written = uart_write_bytes(port, data, size);
#endif
  return written > 0 ? static_cast<std::size_t>(written) : 0;
}

void flush() {
  if (!ready)
    return;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(50));
#else
  uart_wait_tx_done(port, pdMS_TO_TICKS(100));
#endif
}

int printf(const char *format, ...) {
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
} // namespace aq::console
