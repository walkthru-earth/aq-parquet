// Exercise the real BLE link policy (notify credits, advert window, interval
// mode and parameter validation) on the host. Run with
// tools/test_ble_policy.py.
#include <ble_policy.h>

#include <cassert>
#include <cstdint>
#include <cstdio>

using aqsync::EnqueueResult;
using ble::AdvertWindow;
using ble::ConnParams;
using ble::IntervalPolicy;
using ble::NotifyCredits;
using ble::NotifyStatus;
using ble::Options;

namespace {
constexpr std::int64_t kMs = 1000;
constexpr std::int64_t kSec = 1000 * kMs;

static_assert(ble::valid_options(Options{}), "default options are valid");
static_assert(ble::valid_conn_params(Options{}.active), "default active");
static_assert(ble::valid_conn_params(Options{}.relaxed), "default relaxed");

NotifyCredits opened(std::uint32_t generation, std::int64_t now_us) {
  NotifyCredits credits;
  credits.configure(4, 15000);
  credits.open(generation, now_us);
  return credits;
}

void exhaust(NotifyCredits &credits, std::uint32_t generation,
             std::int64_t now_us) {
  while (credits.available())
    assert(credits.take(generation, now_us) == EnqueueResult::Ok);
  assert(credits.take(generation, now_us) == EnqueueResult::Full);
}

void test_credit_exhaustion() {
  NotifyCredits credits = opened(7, 0);
  assert(credits.capacity() == 4 && credits.available() == 4);
  assert(credits.idle() && credits.open_for(7));
  for (int i = 0; i < 4; ++i)
    assert(credits.take(7, 0) == EnqueueResult::Ok);
  assert(credits.take(7, 0) == EnqueueResult::Full);
  assert(credits.available() == 0 && !credits.idle());
  assert(credits.on_notify_tx(7, NotifyStatus::Sent, 1 * kMs));
  assert(credits.available() == 1);
  assert(credits.take(7, 2 * kMs) == EnqueueResult::Ok);
  assert(credits.take(7, 2 * kMs) == EnqueueResult::Full);
  // Every credit returning makes the link idle again.
  for (int i = 0; i < 4; ++i)
    assert(credits.on_notify_tx(7, NotifyStatus::Sent, 3 * kMs));
  assert(credits.idle());
  // An extra Sent never pushes past capacity.
  assert(credits.on_notify_tx(7, NotifyStatus::Sent, 4 * kMs));
  assert(credits.available() == 4);
}

void test_refused_and_failed() {
  NotifyCredits credits = opened(3, 0);
  const std::int64_t t0 = 5 * kSec;
  exhaust(credits, 3, t0);
  assert(credits.on_notify_tx(3, NotifyStatus::Refused, t0 + 10 * kSec));
  assert(credits.available() == 0);
  assert(credits.take(3, t0 + 10 * kSec) == EnqueueResult::Full);
  // Refused is no progress, so the stall still fires 15 s after exhaustion.
  assert(!credits.stalled(t0 + 15 * kSec - 1));
  assert(credits.stalled(t0 + 15 * kSec));

  NotifyCredits failing = opened(4, 0);
  assert(failing.take(4, 0) == EnqueueResult::Ok);
  assert(failing.on_notify_tx(4, NotifyStatus::Failed, 1 * kMs));
  assert(!failing.open_for(4));
  assert(failing.take(4, 2 * kMs) == EnqueueResult::Closed);
  assert(!failing.stalled(100 * kSec));
  // Events after the close belong to no open generation.
  assert(!failing.on_notify_tx(4, NotifyStatus::Sent, 3 * kMs));
}

void test_stale_generation() {
  NotifyCredits credits = opened(1, 0);
  assert(credits.take(1, 0) == EnqueueResult::Ok);
  assert(credits.available() == 3);
  assert(!credits.on_notify_tx(2, NotifyStatus::Sent, 1 * kMs));
  assert(!credits.on_notify_tx(2, NotifyStatus::Failed, 1 * kMs));
  assert(credits.available() == 3 && credits.open_for(1));
  assert(credits.take(2, 1 * kMs) == EnqueueResult::Closed);

  credits.open(2, 2 * kMs);
  assert(credits.available() == 4 && credits.open_for(2));
  assert(!credits.open_for(1));
  assert(credits.take(1, 3 * kMs) == EnqueueResult::Closed);
  assert(!credits.on_notify_tx(1, NotifyStatus::Sent, 3 * kMs));
  assert(credits.take(2, 3 * kMs) == EnqueueResult::Ok);
  assert(credits.available() == 3);
}

void test_disconnect_while_waiting() {
  NotifyCredits credits = opened(9, 0);
  exhaust(credits, 9, 1 * kSec);
  credits.close();
  assert(!credits.open_for(9));
  assert(credits.take(9, 2 * kSec) == EnqueueResult::Closed);
  assert(!credits.stalled(100 * kSec));
  assert(!credits.on_notify_tx(9, NotifyStatus::Sent, 3 * kSec));
}

void test_progress_timeout() {
  NotifyCredits credits = opened(5, 0);
  const std::int64_t t0 = 1 * kSec;
  exhaust(credits, 5, t0);
  assert(!credits.stalled(t0 + 14999 * kMs));
  assert(credits.stalled(t0 + 15 * kSec));

  // A return at t0 + 10 s moves the clock, and a take while busy does not.
  NotifyCredits reset = opened(5, 0);
  exhaust(reset, 5, t0);
  assert(reset.on_notify_tx(5, NotifyStatus::Sent, t0 + 10 * kSec));
  assert(reset.available() == 1);
  assert(!reset.stalled(t0 + 15 * kSec));
  assert(reset.take(5, t0 + 12 * kSec) == EnqueueResult::Ok);
  assert(!reset.stalled(t0 + 25 * kSec - 1));
  assert(reset.stalled(t0 + 25 * kSec));

  // One frame refused forever stalls even with credits to spare.
  NotifyCredits single = opened(8, 0);
  assert(single.take(8, 2 * kSec) == EnqueueResult::Ok);
  assert(single.on_notify_tx(8, NotifyStatus::Refused, 3 * kSec));
  assert(!single.stalled(17 * kSec - 1));
  assert(single.stalled(17 * kSec));

  // A long idle spell before a burst never counts against the host.
  NotifyCredits idle = opened(6, 0);
  assert(!idle.stalled(60 * kSec));
  exhaust(idle, 6, 60 * kSec);
  assert(!idle.stalled(60 * kSec + 1 * kMs));
  assert(!idle.stalled(75 * kSec - 1));
  assert(idle.stalled(75 * kSec));

  // Every credit home means no stall however long ago progress was.
  NotifyCredits home = opened(8, 0);
  assert(home.take(8, 0) == EnqueueResult::Ok);
  assert(home.on_notify_tx(8, NotifyStatus::Sent, 1 * kSec));
  assert(home.idle() && !home.stalled(1000 * kSec));
  // The next burst starts a fresh clock.
  assert(home.take(8, 1000 * kSec) == EnqueueResult::Ok);
  assert(!home.stalled(1015 * kSec - 1));
  assert(home.stalled(1015 * kSec));
}

void test_cancel() {
  NotifyCredits credits = opened(2, 0);
  assert(credits.take(2, 0) == EnqueueResult::Ok);
  assert(credits.available() == 3);
  credits.cancel(1);
  assert(credits.available() == 3);
  credits.cancel(2);
  assert(credits.available() == 4 && credits.idle());
  credits.cancel(2);
  assert(credits.available() == 4);
  exhaust(credits, 2, 1 * kSec);
  credits.cancel(2);
  assert(credits.available() == 1);
  // Cancelling every credit makes the link idle, so it cannot stall.
  credits.cancel(2);
  credits.cancel(2);
  credits.cancel(2);
  assert(credits.idle() && !credits.stalled(100 * kSec));
}

void test_configure_clamps() {
  NotifyCredits credits;
  credits.configure(0, 15000);
  assert(credits.capacity() == 1);
  assert(!credits.open_for(0));
  credits.open(1, 0);
  assert(credits.take(1, 0) == EnqueueResult::Ok);
  assert(credits.take(1, 0) == EnqueueResult::Full);
  assert(credits.on_notify_tx(1, NotifyStatus::Sent, 1 * kMs));
  assert(credits.idle());
}

void test_advert_window() {
  AdvertWindow window;
  assert(!window.fast(0) && window.remaining_ms(0) == 0);
  const std::int64_t now = 100 * kSec;
  window.extend(now, 30000);
  assert(window.fast(now));
  assert(window.remaining_ms(now) == 30000);
  assert(window.fast(now + 30 * kSec - 1));
  assert(!window.fast(now + 30 * kSec));
  assert(window.remaining_ms(now + 30 * kSec) == 0);
  // 0.5 ms left rounds up to 1 ms.
  assert(window.remaining_ms(now + 29999 * kMs + 500) == 1);
  assert(window.remaining_ms(now + 1) == 30000);
  // A shorter window never shortens the fast spell.
  window.extend(now + 1 * kSec, 5000);
  assert(window.fast(now + 29 * kSec));
  assert(window.remaining_ms(now + 10 * kSec) == 20000);
  // A longer one extends it.
  window.extend(now + 10 * kSec, 60000);
  assert(window.remaining_ms(now + 10 * kSec) == 60000);
  window.clear();
  assert(!window.fast(now + 10 * kSec));
  assert(window.remaining_ms(now + 10 * kSec) == 0);
}

void test_interval_policy() {
  using Action = IntervalPolicy::Action;
  IntervalPolicy policy;
  std::uint32_t rearm = 99;
  const std::int64_t t = 10 * kSec;
  policy.connect(t);
  assert(!policy.relaxed());
  assert(policy.on_timer(t + 20 * kSec, 60000, rearm) == Action::None);
  assert(rearm == 40000);
  assert(policy.on_timer(t + 60 * kSec - 1, 60000, rearm) == Action::None);
  assert(rearm == 1);
  assert(policy.on_timer(t + 60 * kSec, 60000, rearm) ==
         Action::RequestRelaxed);
  assert(rearm == 0 && policy.relaxed());
  rearm = 99;
  assert(policy.on_timer(t + 61 * kSec, 60000, rearm) == Action::None);
  assert(rearm == 0 && policy.relaxed());
  // A response frame never wakes a relaxed link.
  policy.on_activity(t + 62 * kSec);
  assert(policy.relaxed());

  assert(policy.on_request(t + 70 * kSec) == Action::RequestActive);
  assert(!policy.relaxed());
  assert(policy.on_request(t + 71 * kSec) == Action::None);
  // Activity pushes the due time.
  policy.on_activity(t + 100 * kSec);
  assert(policy.on_timer(t + 131 * kSec, 60000, rearm) == Action::None);
  assert(rearm == 29000);
  assert(policy.on_timer(t + 160 * kSec, 60000, rearm) ==
         Action::RequestRelaxed);
  // A refused relax request keeps the link active, and the timer asks again.
  policy.request_failed();
  assert(!policy.relaxed());
  assert(policy.on_timer(t + 161 * kSec, 60000, rearm) ==
         Action::RequestRelaxed);
  // A refused active request leaves it relaxed, so the next write retries.
  assert(policy.on_request(t + 162 * kSec) == Action::RequestActive);
  policy.request_failed();
  assert(policy.relaxed());
  assert(policy.on_request(t + 163 * kSec) == Action::RequestActive);

  // A reconnect starts active again.
  policy.connect(t + 200 * kSec);
  assert(!policy.relaxed());
  assert(policy.on_timer(t + 200 * kSec, 60000, rearm) == Action::None);
  assert(rearm == 60000);
}

void test_conn_params() {
  using ble::valid_conn_params;
  assert(valid_conn_params({12, 24, 0, 400}));
  assert(valid_conn_params({80, 160, 4, 400}));
  assert(valid_conn_params({12, 12, 0, 400}));
  assert(!valid_conn_params({}));
  assert(!valid_conn_params({6, 24, 0, 400}));   // below 15 ms
  assert(!valid_conn_params({11, 24, 0, 400}));  // below 15 ms
  assert(!valid_conn_params({24, 12, 0, 400}));  // max below min
  assert(!valid_conn_params({12, 20, 0, 400}));  // max not 15 ms above min
  assert(!valid_conn_params({12, 23, 0, 400}));  // one unit short
  assert(!valid_conn_params({16, 16, 0, 400}));  // equal only at 15 ms
  assert(valid_conn_params({12, 24, 30, 400}));  // latency limit
  assert(!valid_conn_params({12, 24, 31, 400})); // latency over 30
  assert(valid_conn_params({12, 24, 0, 200}));   // 2 s timeout
  assert(valid_conn_params({12, 24, 0, 600}));   // 6 s timeout
  assert(!valid_conn_params({12, 24, 0, 100}));  // 1 s timeout
  assert(!valid_conn_params({12, 24, 0, 199}));
  assert(!valid_conn_params({12, 24, 0, 601}));
  assert(!valid_conn_params({12, 24, 0, 700})); // 7 s timeout
  // 200 ms times 10 is exactly 2 s effective, but a 6 s timeout is not over
  // three effective intervals.
  assert(!valid_conn_params({80, 160, 9, 600}));
  // 200 ms times 11 is a 2.2 s effective interval. Any effective interval
  // above 2 s also fails the timeout rule, since the timeout is at most 6 s.
  assert(!valid_conn_params({80, 160, 10, 600}));
  assert(!valid_conn_params({800, 1600, 0, 600}));  // 2 s, 6 s not over 3x
  assert(!valid_conn_params({1600, 3200, 0, 600})); // 4 s interval
  // 200 ms times 5 is 1 s effective, so the timeout must exceed 3 s.
  assert(!valid_conn_params({80, 160, 4, 300}));
  assert(valid_conn_params({80, 160, 4, 301}));
  // 200 ms times 8 is 1.6 s effective, so 4.8 s is not enough.
  assert(!valid_conn_params({80, 160, 7, 480}));
  assert(valid_conn_params({80, 160, 7, 481}));
}

void test_options() {
  assert(ble::valid_options(Options{}));
  Options o;
  o.notify_credits = 0;
  assert(!ble::valid_options(o));
  o = Options{};
  o.wait_step_ms = o.progress_timeout_ms;
  assert(!ble::valid_options(o));
  o.wait_step_ms = o.progress_timeout_ms + 1;
  assert(!ble::valid_options(o));
  o.wait_step_ms = o.progress_timeout_ms - 1;
  assert(ble::valid_options(o));
  o = Options{};
  o.wait_step_ms = 0;
  assert(!ble::valid_options(o));
  o = Options{};
  o.refused_retry_ms = 0;
  assert(!ble::valid_options(o));
  o.refused_retry_ms = o.progress_timeout_ms;
  assert(!ble::valid_options(o));
  o.refused_retry_ms = o.progress_timeout_ms - 1;
  assert(ble::valid_options(o));
  o = Options{};
  o.progress_timeout_ms = 0;
  assert(!ble::valid_options(o));
  o = Options{};
  o.data_len_octets = 252;
  assert(!ble::valid_options(o));
  o.data_len_octets = 27;
  assert(ble::valid_options(o));
  o.data_len_octets = 26;
  assert(!ble::valid_options(o));
  o = Options{};
  o.active = {12, 20, 0, 400};
  assert(!ble::valid_options(o));
  o = Options{};
  o.relaxed = {80, 160, 10, 600};
  assert(!ble::valid_options(o));
  o = Options{};
  o.fast_adv_min = 31;
  assert(!ble::valid_options(o));
  o = Options{};
  o.slow_adv_max = 16385;
  assert(!ble::valid_options(o));
}
} // namespace

int main() {
  test_credit_exhaustion();
  test_refused_and_failed();
  test_stale_generation();
  test_disconnect_while_waiting();
  test_progress_timeout();
  test_cancel();
  test_configure_clamps();
  test_advert_window();
  test_interval_policy();
  test_conn_params();
  test_options();
  std::printf("ble policy passed\n");
  return 0;
}
