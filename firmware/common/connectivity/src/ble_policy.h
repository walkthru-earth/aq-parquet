#pragma once

// BLE link policy (docs/shared/ble-sync-protocol.md, "Link parameters and
// notify flow"). Pure state with no host stack, controller or RTOS
// dependency, so the native transport and host tests share it. The transport
// serialises every call; nothing here is thread safe on its own.

#include "frame_ring.h"

#include <cstdint>

namespace ble {
// Connection parameters in Bluetooth units: interval 1.25 ms, supervision
// timeout 10 ms.
struct ConnParams {
  std::uint16_t itvl_min = 0;
  std::uint16_t itvl_max = 0;
  std::uint16_t latency = 0;
  std::uint16_t supervision_timeout = 0;
};

// Core spec ranges, plus the rule that the supervision timeout outlasts two
// effective intervals, and the Apple accessory limits (minimum interval
// 15 ms, maximum 15 ms above the minimum unless both are 15 ms, latency at
// most 30, an effective interval of at most 2 s, timeout 2 to 6 s and over
// three effective intervals), so one request suits Android and iOS centrals.
constexpr bool valid_conn_params(const ConnParams &p) {
  const std::uint32_t effective_us =
      std::uint32_t(p.itvl_max) * 1250U * (std::uint32_t(p.latency) + 1U);
  const std::uint32_t timeout_us =
      std::uint32_t(p.supervision_timeout) * 10000U;
  return p.itvl_min >= 12 && p.itvl_min <= p.itvl_max && p.itvl_max <= 3200 &&
         (p.itvl_max >= p.itvl_min + 12 || p.itvl_max == 12) &&
         p.latency <= 30 && p.supervision_timeout >= 200 &&
         p.supervision_timeout <= 600 && effective_us <= 2000000U &&
         timeout_us > 3U * effective_us;
}

// Board-neutral link policy, set from the sync service config.
struct Options {
  // Advertising intervals in 0.625 ms units. Fast is 30 to 60 ms, slow about
  // one second.
  std::uint16_t fast_adv_min = 48;
  std::uint16_t fast_adv_max = 96;
  std::uint16_t slow_adv_min = 1600;
  std::uint16_t slow_adv_max = 2048;
  // Fast advertising after enable, disconnect or wake_advertising().
  std::uint32_t fast_window_ms = 30000;
  // Fast advertising after an advert flag flips, so a low-power scanner
  // filtering on the flag byte sees it within a minute.
  std::uint32_t flag_window_ms = 60000;
  // LE data length request on connect: 251 octets at 1M PHY timing.
  std::uint16_t data_len_octets = 251;
  std::uint16_t data_len_time_us = 2120;
  bool prefer_2m_phy = true;
  // 15 to 30 ms, latency 0, 4 s timeout while requests flow.
  ConnParams active{12, 24, 0, 400};
  // 100 to 200 ms, latency 4, 4 s timeout once the link is idle.
  ConnParams relaxed{80, 160, 4, 400};
  std::uint32_t relax_after_ms = 60000;
  // Response frames the host may hold before the worker waits.
  std::uint8_t notify_credits = 4;
  // A frame outstanding and none sent for this long closes the link.
  std::uint32_t progress_timeout_ms = 15000;
  // One worker wait for a credit; samples are serviced between steps.
  std::uint32_t wait_step_ms = 200;
  // A frame the host refused for lack of buffers is retried after this.
  std::uint32_t refused_retry_ms = 10;
};

constexpr bool valid_options(const Options &o) {
  return o.fast_adv_min >= 32 && o.fast_adv_min <= o.fast_adv_max &&
         o.slow_adv_min >= o.fast_adv_min && o.slow_adv_min <= o.slow_adv_max &&
         o.slow_adv_max <= 16384 && o.data_len_octets >= 27 &&
         o.data_len_octets <= 251 && o.data_len_time_us >= 328 &&
         o.data_len_time_us <= 17040 && valid_conn_params(o.active) &&
         valid_conn_params(o.relaxed) && o.notify_credits > 0 &&
         o.progress_timeout_ms > 0 && o.wait_step_ms > 0 &&
         o.wait_step_ms < o.progress_timeout_ms && o.refused_retry_ms > 0 &&
         o.refused_retry_ms < o.progress_timeout_ms;
}

// How the host stack reported one response notification.
enum class NotifyStatus : std::uint8_t {
  Sent,    // the host took the frame, its credit returns
  Refused, // out of buffers now, the same frame is retried later
  Failed,  // the link cannot carry it, the response stream is broken
};

// Response notify credits for one connection. A producer takes a credit per
// frame before queueing it, and the host returns it when the stack accepts the
// frame (BLE_GAP_EVENT_NOTIFY_TX status 0). A refused frame keeps its credit.
// A credit out with no return for the progress timeout marks a stall, so a
// host that refuses one frame forever stalls as surely as a full queue.
class NotifyCredits {
public:
  void configure(std::uint8_t slots, std::uint32_t progress_timeout_ms);
  // A new authenticated connection, all credits free.
  void open(std::uint32_t generation, std::int64_t now_us);
  // Disconnect or a broken stream. Every take() for the generation is Closed.
  void close();

  // Ok consumes a credit, Full waits for one, Closed never retries.
  aqsync::EnqueueResult take(std::uint32_t generation, std::int64_t now_us);
  // A producer that took a credit gives it back unused.
  void cancel(std::uint32_t generation);
  // BLE_GAP_EVENT_NOTIFY_TX for a queued response frame. Returns false when
  // the event belongs to another generation and changed nothing.
  bool on_notify_tx(std::uint32_t generation, NotifyStatus status,
                    std::int64_t now_us);

  // A credit is out and none returned within the progress timeout.
  bool stalled(std::int64_t now_us) const;
  bool open_for(std::uint32_t generation) const {
    return open_ && generation == generation_;
  }
  std::uint8_t available() const { return available_; }
  std::uint8_t capacity() const { return capacity_; }
  // Every credit is home, so nothing is queued or being queued.
  bool idle() const { return available_ == capacity_; }

private:
  std::uint8_t capacity_ = 4;
  std::uint8_t available_ = 0;
  std::uint32_t generation_ = 0;
  std::int64_t progress_timeout_us_ = 15000000;
  // The last credit return, or the first take after idle.
  std::int64_t progress_us_ = 0;
  bool open_ = false;
};

// Two stage advertising. Fast until the window ends, then slow. Each event
// extends the fast window and never shortens it.
class AdvertWindow {
public:
  void extend(std::int64_t now_us, std::uint32_t window_ms);
  void clear() { fast_until_us_ = 0; }
  bool fast(std::int64_t now_us) const { return now_us < fast_until_us_; }
  // Milliseconds of fast advertising left, rounded up, 0 when slow.
  std::uint32_t remaining_ms(std::int64_t now_us) const;

private:
  std::int64_t fast_until_us_ = 0;
};

// Connection interval mode. Active on connect and on every request, relaxed
// after Options::relax_after_ms without one.
class IntervalPolicy {
public:
  enum class Action : std::uint8_t { None, RequestActive, RequestRelaxed };
  void connect(std::int64_t now_us);
  // A control write arrived.
  Action on_request(std::int64_t now_us);
  // A response frame left, which counts as activity but never wakes the link.
  void on_activity(std::int64_t now_us) { last_us_ = now_us; }
  // The idle timer fired. Relaxes once the link was quiet long enough, and
  // otherwise reports how many ms remain through `rearm_ms`.
  Action on_timer(std::int64_t now_us, std::uint32_t relax_after_ms,
                  std::uint32_t &rearm_ms);
  bool relaxed() const { return relaxed_; }

private:
  std::int64_t last_us_ = 0;
  bool relaxed_ = false;
};
} // namespace ble
