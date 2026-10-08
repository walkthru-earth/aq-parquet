#pragma once

// Serial log tee. Normal writes go to the console and an 8 KiB ring buffer
// that `LOG_TAIL` (protocol v2)
// can hand to a phone, so an advanced user sees the same `PARQUET …`/`BLE …`/
// `WIFI …` lines a bench log shows, without a cable. Text for humans, not an
// API. Writers may run on any task; the ring is guarded by a spinlock and the
// forwarding write to Serial is outside it. Radio/archive hot-path diagnostics
// use record_only(): they stay in LOG_TAIL without calling the console sink.

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

  // Diagnostic-only Print facade: retain full bytes in the ring without any
  // console IO. Do not use for serial protocol/data or owner provisioning
  // replies.
  Print &record_only() { return record_only_; }

  // Copies up to `max` of the newest bytes into `out` (not terminated) and
  // returns the copied length; `total` receives bytes logged since boot.
  std::size_t tail(char *out, std::size_t max, std::uint32_t &total) const;

private:
  class RingOutput : public Print {
  public:
    explicit RingOutput(DebugLog &owner) : owner_(owner) {}
    size_t write(uint8_t byte) override {
      return owner_.write_record_only(&byte, 1);
    }
    size_t write(const uint8_t *buffer, size_t size) override {
      return owner_.write_record_only(buffer, size);
    }

  private:
    DebugLog &owner_;
  };

  size_t write_record_only(const uint8_t *buffer, size_t size);
  RingOutput record_only_{*this};
  Print *output_ = nullptr;
  char ring_[kRingBytes]{};
  std::size_t head_ = 0; // next write position
  std::uint32_t total_ = 0;
};

extern DebugLog aqlog;
