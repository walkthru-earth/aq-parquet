#pragma once

// Serial log tee. Normal writes go to the console and a ring buffer that
// `LOG_TAIL` (protocol v3) can hand to a phone, so an advanced user sees the
// same `PARQUET …`/`BLE …`/`WIFI …` lines a bench log shows, without a cable.
// Text for humans, not an API. Writers may run on any task; the ring is
// guarded by a spinlock and the forwarding write to the console is outside it.
// Radio/archive hot-path diagnostics use record_only(): they stay in LOG_TAIL
// without calling the console sink.
//
// The log starts on a small internal ring. A board with PSRAM attaches a
// persistent block (see log_persist.h) holding a larger ring plus a ring of
// fixed-size diagnostic events and per-reason counters. A validated header
// lets a warm reboot keep the previous boot's lines; a cold boot or a bad
// header starts empty. Without the block the internal ring keeps working.

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
  static constexpr std::size_t kFallbackRingBytes = 4096;
  static constexpr std::size_t kFallbackEventSlots = 8;
  static constexpr std::size_t kEventBytes = 16;
  static constexpr std::size_t kCounters = 32;

  // Most bytes copied inside one critical section. Longer writes and reads
  // run in slices so neither a large line nor a LOG_TAIL or DIAG reader holds
  // the spinlock for long. Writers keep the spinlock so they stay ISR safe.
  static constexpr std::size_t kLockedCopyBytes = 256;

  // Sizes of an attached persistent block. A board chooses them through
  // aq::log::attach_persistent (log_persist.h), never in this file.
  struct Layout {
    std::size_t ring_bytes;
    std::size_t event_slots;
  };
  static constexpr std::size_t kHeaderBytes = 112;
  static constexpr std::size_t persistent_bytes(const Layout &layout) {
    return kHeaderBytes + layout.ring_bytes + layout.event_slots * kEventBytes;
  }

  // Absolute read position into the ring. A reader that is overtaken by the
  // writer is clipped forward to the oldest retained byte.
  struct Cursor {
    std::uint64_t position = 0;
    std::size_t remaining = 0;
  };

  using Sink = std::size_t (*)(void *, const std::uint8_t *, std::size_t);
  DebugLog();
  // Set once before tasks start. Context must outlive this log.
  void set_output(Sink output, void *context) {
    output_ = output;
    output_context_ = context;
  }

  // Moves the log onto `block` (persistent_bytes(layout) bytes, 4-byte
  // aligned, outliving this log). A valid header from the previous boot is
  // kept unless `cold`; otherwise the block starts empty. Lines and events
  // logged before the attach are appended. Returns true when the previous
  // contents were retained.
  bool attach(void *block, std::size_t bytes, const Layout &layout, bool cold);
  bool persistent() const;
  // 0 on the internal ring; 1 after a cold start, then +1 per warm reboot.
  std::uint16_t boot_seq() const;
  std::size_t ring_bytes() const;
  std::size_t retained() const;

  using LogOutput::write;
  std::size_t write(const std::uint8_t *buffer, std::size_t size) override;

  // Diagnostic-only output; never use for protocol or provisioning replies.
  LogOutput &record_only() { return record_only_; }

  // Copies up to `max` of the newest bytes into `out` (not terminated) and
  // returns the copied length; `total` receives bytes logged since boot.
  // Copies in slices. A writer that overtakes the copy shortens it, and the
  // result is always one contiguous span.
  std::size_t tail(char *out, std::size_t max, std::uint32_t &total) const;
  // Streams the newest `max` retained bytes in slices without a ring-sized
  // copy. `total` receives bytes logged since boot.
  Cursor tail_cursor(std::size_t max, std::uint32_t &total) const;
  // One call returns a contiguous span ending at cursor.position. It is
  // shorter than requested when a writer overtakes it between slices.
  std::size_t read(Cursor &cursor, char *out, std::size_t max) const;

  // Fixed-size diagnostic events and saturating counters. Both persist with
  // the attached block.
  void record_event(const std::uint8_t (&event)[kEventBytes],
                    std::size_t counter_index);
  // Copies up to `max` newest events, oldest first, and returns the count.
  // Events overwritten while the copy runs are skipped.
  std::size_t events(std::uint8_t (*out)[kEventBytes], std::size_t max) const;
  std::uint16_t counter(std::size_t index) const;

  struct Header; // persistent block layout, defined in debug_log.cpp

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
  void append_locked(const std::uint8_t *source, std::size_t count);
  void append_event_locked(const std::uint8_t *event);
  void seal_locked();
  std::size_t copy_locked(std::uint64_t position, char *out,
                          std::size_t count) const;
  void copy_events_locked(std::uint64_t sequence,
                          std::uint8_t (*out)[kEventBytes],
                          std::size_t count) const;

  RingOutput record_only_{*this};
  Sink output_ = nullptr;
  void *output_context_ = nullptr;

  Header *header_ = nullptr; // set once attached
  char *ring_ = fallback_ring_;
  std::size_t ring_size_ = kFallbackRingBytes;
  std::uint8_t (*event_ring_)[kEventBytes] = fallback_events_;
  std::size_t event_slots_ = kFallbackEventSlots;
  std::uint16_t *counters_ = fallback_counters_;

  std::size_t head_ = 0;      // next write position
  std::size_t retained_ = 0;  // valid bytes ending at head_
  std::uint64_t written_ = 0; // absolute end of the retained bytes
  std::size_t event_head_ = 0;
  std::size_t event_count_ = 0;
  std::uint64_t events_written_ = 0; // absolute end of the retained events
  std::uint32_t total_ = 0;          // bytes logged since boot
  std::uint16_t boot_seq_ = 0;

  char fallback_ring_[kFallbackRingBytes]{};
  std::uint8_t fallback_events_[kFallbackEventSlots][kEventBytes]{};
  std::uint16_t fallback_counters_[kCounters]{};
};

extern DebugLog aqlog;
