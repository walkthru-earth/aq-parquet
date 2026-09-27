#pragma once

// Serial log tee. Everything the firmware prints goes to the USB serial port
// as before *and* into an 8 KiB ring buffer that `LOG_TAIL` (protocol v2)
// can hand to a phone, so an advanced user sees the same `PARQUET …`/`BLE …`/
// `WIFI …` lines a bench log shows, without a cable. Text for humans, not an
// API. Writers may run on any task; the ring is guarded by a spinlock and the
// forwarding write to Serial is outside it.

#include <Print.h>

#include <cstddef>
#include <cstdint>

class DebugLog : public Print {
public:
  static constexpr std::size_t kRingBytes = 8192;

  DebugLog();
  // Optional board-selected console. Set once before worker/radio tasks start;
  // the sink must outlive this log. The default preserves Arduino Serial.
  void set_output(Print &output) { output_ = &output; }

  size_t write(uint8_t byte) override;
  size_t write(const uint8_t *buffer, size_t size) override;

  // Copies up to `max` of the newest bytes into `out` (not terminated) and
  // returns the copied length; `total` receives bytes logged since boot.
  std::size_t tail(char *out, std::size_t max, std::uint32_t &total) const;

private:
  Print *output_ = nullptr;
  char ring_[kRingBytes]{};
  std::size_t head_ = 0; // next write position
  std::uint32_t total_ = 0;
};

extern DebugLog aqlog;
