#pragma once

// BLE link policy (docs/shared/ble-sync-protocol.md, "Link parameters and
// notify flow"). Pure state with no host stack, controller or RTOS
// dependency, so the native transport and host tests share it. The transport
// serialises every call; nothing here is thread safe on its own.

#include "frame_ring.h"

#include <cstdint>

namespace ble {
// The response queue holds one slot per credit inline.
constexpr std::uint8_t kMaxNotifyCredits = 16;

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
  // LE data length request once the link is secure: 251 octets at 1M PHY
  // timing.
  std::uint16_t data_len_octets = 251;
  std::uint16_t data_len_time_us = 2120;
  bool prefer_2m_phy = true;
  // 15 to 30 ms, latency 0, 4 s timeout while requests flow.
  ConnParams active{12, 24, 0, 400};
  // 100 to 200 ms, latency 4, 4 s timeout once the link is idle.
  ConnParams relaxed{80, 160, 4, 400};
  std::uint32_t relax_after_ms = 60000;
  // Response frames the host may hold before the worker waits, at most
  // kMaxNotifyCredits.
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
         o.notify_credits <= kMaxNotifyCredits && o.progress_timeout_ms > 0 &&
         o.wait_step_ms > 0 && o.wait_step_ms < o.progress_timeout_ms &&
         o.refused_retry_ms > 0 && o.refused_retry_ms < o.progress_timeout_ms;
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

// One queued response frame. The bytes live in the transport, indexed by slot.
struct TxSlot {
  std::uint16_t length = 0;
  std::uint16_t handle = 0;
  std::uint32_t session = 0;
};

// The response transmit queue: credits plus a ring of one slot per credit. A
// producer reserves the slot after the queued ones, fills its bytes outside
// any lock and then publishes it. The host alone reads and pops the head.
//
// Every clear (open, close, a failed or foreign frame) starts a new epoch, so
// a reservation made before it can never publish into the new ring, even when
// the stream reopens for the same generation while the bytes are copied.
// The caller serialises every call (tx_lock in the transport).
class ResponseQueue {
public:
  struct Reservation {
    aqsync::EnqueueResult result = aqsync::EnqueueResult::Closed;
    std::uint8_t slot = 0;
    std::uint32_t epoch = 0;
  };
  // What one NOTIFY_TX did. `current` is false when the frame belonged to a
  // stream that is gone, `stalled` when a refused frame hit the timeout.
  struct Outcome {
    bool current = false;
    bool stalled = false;
  };

  void configure(std::uint8_t slots, std::uint32_t progress_timeout_ms);
  // A new stream for `generation`, all credits free and nothing queued.
  void open(std::uint32_t generation, std::int64_t now_us);
  // The stream ended. Every reserve() and publish() for it is Closed.
  void close();

  // Ok takes a credit and names the slot to fill, Full waits for a credit,
  // Closed never retries.
  Reservation reserve(std::uint32_t generation, std::int64_t now_us);
  // Queues a filled slot. False when the stream closed or reopened since the
  // reservation; the slot bytes are then ignored and no credit is owed.
  bool publish(const Reservation &reservation, std::uint32_t generation,
               std::uint16_t length, std::uint16_t handle);
  // The head frame, if any.
  bool front(TxSlot &slot, std::uint8_t &index) const;
  // BLE_GAP_EVENT_NOTIFY_TX for the head frame. Sent pops it, Refused keeps
  // it, Failed or a frame of another generation clears the queue.
  Outcome on_notify_tx(std::uint32_t session, NotifyStatus status,
                       std::int64_t now_us);

  bool open_for(std::uint32_t generation) const {
    return credits_.open_for(generation);
  }
  // Nothing queued and no credit out.
  bool idle() const { return queued_ == 0 && credits_.idle(); }
  std::uint8_t capacity() const { return credits_.capacity(); }
  std::uint8_t queued() const { return queued_; }
  std::uint32_t epoch() const { return epoch_; }

private:
  void clear();

  NotifyCredits credits_;
  TxSlot slots_[kMaxNotifyCredits]{};
  std::uint8_t head_ = 0;
  std::uint8_t queued_ = 0;
  std::uint32_t epoch_ = 0;
};

// How a Response CCCD write changes the stream.
enum class StreamChange : std::uint8_t {
  None,   // nothing to do
  Close,  // notifications off: end the stream at once
  Reopen, // on again after an end: a new generation and a fresh stream
};

// `link_closing` is NimBLE's BLE_GAP_SUBSCRIBE_REASON_TERM during disconnect,
// which the disconnect handler covers. `open` says whether the current
// generation's stream is open, which it is from connect until it ends.
constexpr StreamChange response_subscribe(bool notify, bool link_closing,
                                          bool open) {
  if (link_closing)
    return StreamChange::None;
  if (!notify)
    return StreamChange::Close;
  return open ? StreamChange::None : StreamChange::Reopen;
}

// What a request to terminate the link did.
enum class CloseStart : std::uint8_t {
  Started,        // the controller will report a disconnect
  AlreadyClosing, // a terminate is already pending; it will report one
  Failed,         // nothing pending, the link stays up
};

// `already_rc` is the stack's "a terminate is pending" code (BLE_HS_EALREADY).
constexpr CloseStart classify_close(int rc, int already_rc) {
  return rc == 0            ? CloseStart::Started
         : rc == already_rc ? CloseStart::AlreadyClosing
                            : CloseStart::Failed;
}

// Slow peer handling for a refused head frame. A stalled stream is logged and
// closed once per session. A close that started, or one already pending for
// another reason (a reboot, say), ends the retries; only a failed terminate
// keeps them going so the close is tried again.
class StallClose {
public:
  enum class Action : std::uint8_t {
    Retry, // offer the frame again after refused_retry_ms
    Close, // log once if log() says so, then terminate as SlowPeer
    Wait,  // the close is under way, DISCONNECT clears the queue
  };
  Action on_refused(std::uint32_t session, bool stalled) const;
  // True once per session, so the stall is logged a single time.
  bool log(std::uint32_t session);
  // The terminate result. Returns the action that follows it.
  Action on_close(std::uint32_t session, CloseStart start);

private:
  // Connections count generations from 1, so 0 matches none.
  std::uint32_t closing_ = 0;
  std::uint32_t logged_ = 0;
};

// Advert flag flips. The first published advert only sets the baseline, so
// the boot placeholder and the logger's first real flags are no flip and the
// boot keeps its own fast window. Counter changes are never flips.
class FlagWatch {
public:
  // A published advert was applied. True when its flag byte differs from the
  // last published one.
  bool on_publish(std::uint8_t flags);

private:
  bool seen_ = false;
  std::uint8_t flags_ = 0;
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
  // The stack refused the request on_request() or on_timer() asked for, so
  // the link keeps its previous interval: flip the mode back.
  void request_failed() { relaxed_ = !relaxed_; }
  bool relaxed() const { return relaxed_; }

private:
  std::int64_t last_us_ = 0;
  bool relaxed_ = false;
};

// Where the post-connect link sequence is, see link_on_host() in ble_sync.cpp.
enum class LinkStep : std::uint8_t { Idle, Dle, Phy, Interval, Done };

// The Dle step asks for `want` TX octets only when the central's own data
// length update did not already reach it. `central_tx_octets` is the last
// DATA_LEN_CHG max_tx_octets on this link, 0 when none came.
[[nodiscard]] constexpr bool dle_request_needed(std::uint16_t central_tx_octets,
                                                std::uint16_t want) {
  return central_tx_octets < want;
}

// A procedure the central ran (MTU exchange, connection update, PHY update)
// while the local PHY request still waits. The wait starts over, so the PHY
// request goes out only after a full settle with no central procedure. An
// Android central's MTU 517 exchange and connection update land about 2 s
// after a link that was secure at CONNECT, where the old fixed wait put the
// PHY request (CoreS3 bench 2026-10-18). The host sees a central's connection
// update only after its instant, so this narrows the window for crossing it
// (the lld_con.c 3275 controller assert) and cannot close it.
[[nodiscard]] constexpr bool central_procedure_rearms(LinkStep step) {
  return step == LinkStep::Phy;
}

// The 2M PHY code in HCI and NimBLE (BLE_GAP_LE_PHY_2M).
constexpr std::uint8_t kPhy2M = 2;
// The PHY step asks for 2M only when the link is not on 2M both ways, so a
// central that already moved the link to 2M sees no local procedure. A failed
// read asks, as before.
[[nodiscard]] constexpr bool phy_request_needed(bool read_ok, std::uint8_t tx,
                                                std::uint8_t rx) {
  return !read_ok || tx != kPhy2M || rx != kPhy2M;
}

// BLE HCI fail lines for the connection update. While a procedure runs the
// host refuses it (BLE_HS_EBUSY, BLE_HS_EALREADY). The link step and the relax
// timer retry every second, and a refused active request from a Control write
// is retried by the next write, however late. Only the first failure of a run
// with the same rc is logged, so a later failure with that rc stays silent
// until a success, a new connection or a host reset ends the run.
class HciFailLog {
public:
  // True when this result of the command should be logged.
  [[nodiscard]] bool on_result(int rc) {
    const bool fresh = rc != 0 && rc != last_rc_;
    last_rc_ = rc;
    return fresh;
  }
  void reset() { last_rc_ = 0; }

private:
  int last_rc_ = 0;
};
} // namespace ble
