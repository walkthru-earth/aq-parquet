#pragma once

#include "os_gnss.h"

#include <cstdint>
#include <expected>

// Reusable receiver task. A board fills ReaderConfig with its UART, pins and
// power callback, calls start() once, and passes latest() to the logger
// through oslogger::Hooks::gnss_fix. The task owns the UART after start().
namespace opensensor::gnss {
struct ReaderConfig {
  int uart_port = -1; // a free UART, never the console or a sensor UART
  int tx_pin = -1;    // ESP TX, to the receiver RXD
  int rx_pin = -1;    // ESP RX, from the receiver TXD
  int baud = 115200;
  std::uint32_t poll_period_ms = 10000;
  // Keep this default. tools/stack_budget.py reads it as the task stack.
  std::uint32_t reader_stack_bytes = 4096;
  unsigned priority = 1; // below the board task (2)
  int core = 1;
  const Dialect *dialect = nullptr;
  // Optional, called once from the task before the first query. Switch the
  // receiver supply on and return true. It must never switch power off.
  bool (*power_on)(void *) = nullptr;
  void *context = nullptr;
};

enum class [[nodiscard]] StartError : std::uint8_t {
  InvalidConfig,
  AlreadyStarted,
  Uart,
  Task,
};

// Installs the UART driver and creates the "os-gnss" task.
[[nodiscard]] std::expected<void, StartError> start(const ReaderConfig &);

// Copy the newest fix. False when there is none or the last reply had none.
// Safe from any task, never blocks, fits Hooks::gnss_fix.
[[nodiscard]] bool latest(Fix &fix);
} // namespace opensensor::gnss
