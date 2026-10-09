#pragma once

// Serial log tee. Normal writes go to the console and an 8 KiB ring buffer
// that `LOG_TAIL` (protocol v2)
// can hand to a phone, so an advanced user sees the same `PARQUET …`/`BLE …`/
// `WIFI …` lines a bench log shows, without a cable. Text for humans, not an
// API. Writers may run on any task; the ring is guarded by a spinlock and the
// forwarding write to the console is outside it. Radio/archive hot-path
// diagnostics use record_only(): they stay in LOG_TAIL without calling the
// console sink.

#include <cstddef>
#include <cstdint>

// Text-only diagnostic writer. This is deliberately independent of Arduino's
// stream and numeric formatting interfaces.
class LogOutput {
public:
  virtual ~LogOutput() = default;
  virtual std::size_t write(const std::uint8_t *data, std::size_t size) = 0;
  std::size_t write(std::uint8_t byte) { return write(&byte, 1); }
  std::size_t print(const char *text);
  std::size_t println(const char *text);
  int printf(const char *format, ...) __attribute__((format(printf, 2, 3)));
};

class DebugLog : public LogOutput {
public:
  static constexpr std::size_t kRingBytes = 8192;

  using Sink = std::size_t (*)(void *, const std::uint8_t *, std::size_t);
  DebugLog();
  // Set once before tasks start. Context must outlive this log.
  void set_output(Sink output, void *context) {
    output_ = output;
    output_context_ = context;
  }

  using LogOutput::write;
  std::size_t write(const std::uint8_t *buffer, std::size_t size) override;

  // Diagnostic-only output; never use for protocol or provisioning replies.
  LogOutput &record_only() { return record_only_; }

  // Copies up to `max` of the newest bytes into `out` (not terminated) and
  // returns the copied length; `total` receives bytes logged since boot.
  std::size_t tail(char *out, std::size_t max, std::uint32_t &total) const;

private:
  class RingOutput : public LogOutput {
  public:
    explicit RingOutput(DebugLog &owner) : owner_(owner) {}
    size_t write(const uint8_t *buffer, size_t size) override {
      return owner_.write_record_only(buffer, size);
    }

  private:
    DebugLog &owner_;
  };

  size_t write_record_only(const uint8_t *buffer, size_t size);
  RingOutput record_only_{*this};
  Sink output_ = nullptr;
  void *output_context_ = nullptr;
  char ring_[kRingBytes]{};
  std::size_t head_ = 0; // next write position
  std::uint32_t total_ = 0;
};

extern DebugLog aqlog;
