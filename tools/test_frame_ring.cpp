// Exercise the real FrameRing against byte exact expectations and a deque
// model of the wire stream. Run with tools/test_frame_ring.py.
#include <frame_ring.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <random>
#include <vector>

using aqsync::EnqueueResult;
using aqsync::FrameRing;

namespace {
std::vector<std::uint8_t> body(std::size_t length, std::uint8_t seed) {
  std::vector<std::uint8_t> bytes(length);
  for (std::size_t i = 0; i < length; ++i)
    bytes[i] = static_cast<std::uint8_t>(seed + i * 7);
  return bytes;
}

std::vector<std::uint8_t> wire(const std::vector<std::uint8_t> &frame) {
  std::vector<std::uint8_t> bytes{
      static_cast<std::uint8_t>(frame.size() & 0xff),
      static_cast<std::uint8_t>((frame.size() >> 8) & 0xff)};
  bytes.insert(bytes.end(), frame.begin(), frame.end());
  return bytes;
}

// Reads every queued byte through peek() and consume() without losing any.
std::vector<std::uint8_t> drain(FrameRing &ring) {
  std::vector<std::uint8_t> out;
  const std::uint8_t *data = nullptr;
  while (std::size_t span = ring.peek(&data)) {
    out.insert(out.end(), data, data + span);
    ring.consume(span);
  }
  assert(data == nullptr);
  return out;
}

struct Snapshot {
  std::size_t used, frames;
  std::uint64_t pushed, consumed;
  bool mid, closed;
};

Snapshot snapshot(const FrameRing &ring) {
  return {ring.used(),           ring.frames(),    ring.pushed_total(),
          ring.consumed_total(), ring.mid_frame(), ring.closed()};
}

void assert_same(const Snapshot &a, const Snapshot &b) {
  assert(a.used == b.used && a.frames == b.frames && a.pushed == b.pushed &&
         a.consumed == b.consumed && a.mid == b.mid && a.closed == b.closed);
}

void test_prefix_and_results() {
  std::uint8_t storage[16];
  FrameRing ring;
  ring.attach(storage, sizeof(storage));
  assert(ring.capacity() == 16 && ring.empty() && ring.free() == 16);
  const std::uint8_t *data = nullptr;
  assert(ring.peek(&data) == 0 && data == nullptr);

  const auto frame = body(5, 0x10);
  EnqueueResult result = ring.try_push(frame.data(), frame.size());
  assert(result == EnqueueResult::Ok);
  assert(ring.used() == 7 && ring.frames() == 1 && ring.pushed_total() == 7);
  assert(ring.peek(&data) == 7 && data == storage);
  assert(storage[0] == 5 && storage[1] == 0);
  for (std::size_t i = 0; i < frame.size(); ++i)
    assert(storage[2 + i] == frame[i]);

  // 7 used, 9 free. A 7 byte body needs 9 and fits exactly. An 8 byte body
  // does not fit and must leave everything as it was.
  const auto big = body(8, 0x40);
  Snapshot before = snapshot(ring);
  result = ring.try_push(big.data(), big.size());
  assert(result == EnqueueResult::Full);
  assert_same(before, snapshot(ring));
  for (std::size_t i = 0; i < frame.size(); ++i)
    assert(storage[2 + i] == frame[i]);
  const auto exact = body(7, 0x60);
  result = ring.try_push(exact.data(), exact.size());
  assert(result == EnqueueResult::Ok);
  assert(ring.used() == 16 && ring.free() == 0 && ring.frames() == 2);

  // Closed refuses pushes but keeps queued bytes for the final flush.
  ring.consume(7);
  ring.close();
  assert(ring.closed());
  before = snapshot(ring);
  result = ring.try_push(frame.data(), frame.size());
  assert(result == EnqueueResult::Closed);
  assert_same(before, snapshot(ring));
  assert(drain(ring) == wire(exact));
  assert(ring.empty() && ring.frames() == 0);

  // reset() reopens and empties. Discarded bytes count as consumed so every
  // pushed_total() mark stays reachable.
  result = ring.try_push(frame.data(), frame.size());
  assert(result == EnqueueResult::Closed);
  ring.reset();
  assert(!ring.closed() && ring.empty());
  result = ring.try_push(frame.data(), frame.size());
  assert(result == EnqueueResult::Ok);
  assert(ring.consumed_total() == 16);
  ring.reset();
  assert(ring.empty() && ring.frames() == 0 && !ring.mid_frame());
  assert(ring.pushed_total() == 23 && ring.consumed_total() == 23);
  assert(ring.peek(&data) == 0);

  // Reset in the middle of a partly sent frame discards its remainder too.
  result = ring.try_push(exact.data(), exact.size());
  assert(result == EnqueueResult::Ok);
  ring.consume(3);
  assert(ring.mid_frame());
  ring.reset();
  assert(ring.empty() && !ring.mid_frame() && ring.frames() == 0);
  assert(ring.pushed_total() == 32 && ring.consumed_total() == 32);

  // A ring without storage is closed for good.
  FrameRing detached;
  detached.attach(nullptr, 64);
  assert(detached.capacity() == 0 && !detached.fits(1));
  result = detached.try_push(frame.data(), frame.size());
  assert(result == EnqueueResult::Closed);
}

void test_refused_lengths() {
  std::vector<std::uint8_t> storage(0x20000);
  FrameRing ring;
  ring.attach(storage.data(), storage.size());
  const auto seed = body(3, 1);
  EnqueueResult result = ring.try_push(seed.data(), seed.size());
  assert(result == EnqueueResult::Ok);
  ring.consume(1);
  const Snapshot before = snapshot(ring);

  const std::uint8_t one = 0;
  result = ring.try_push(&one, 0);
  assert(result == EnqueueResult::Rejected);
  assert_same(before, snapshot(ring));
  const auto huge = body(FrameRing::kMaxFrame + 1, 9);
  result = ring.try_push(huge.data(), huge.size());
  assert(result == EnqueueResult::Rejected);
  assert_same(before, snapshot(ring));
  assert(!ring.fits(0) && !ring.fits(FrameRing::kMaxFrame + 1));
  assert(ring.fits(FrameRing::kMaxFrame));

  // The largest body still encodes as ff ff.
  ring.reset();
  assert(ring.consumed_total() == ring.pushed_total());
  const auto max = body(FrameRing::kMaxFrame, 3);
  result = ring.try_push(max.data(), max.size());
  assert(result == EnqueueResult::Ok);
  assert(storage[0] == 0xff && storage[1] == 0xff);
  assert(drain(ring) == wire(max));
}

void test_fits() {
  std::uint8_t storage[10];
  FrameRing ring;
  ring.attach(storage, sizeof(storage));
  assert(!ring.fits(0) && ring.fits(1) && ring.fits(8) && !ring.fits(9));
  // fits() ignores what is queued now.
  const auto frame = body(4, 2);
  EnqueueResult result = ring.try_push(frame.data(), frame.size());
  assert(result == EnqueueResult::Ok);
  assert(ring.fits(8));
  const auto eight = body(8, 2);
  result = ring.try_push(eight.data(), eight.size());
  assert(result == EnqueueResult::Full);
}

void test_wrap() {
  std::uint8_t storage[16];
  FrameRing ring;
  ring.attach(storage, sizeof(storage));
  const auto first = body(8, 0x20); // 10 wire bytes
  EnqueueResult result = ring.try_push(first.data(), first.size());
  assert(result == EnqueueResult::Ok);
  ring.consume(10);
  assert(ring.empty() && ring.frames() == 0 && !ring.mid_frame());

  // Head is at 10. This frame is 12 wire bytes, so 6 go to the end and 6
  // wrap to the start of storage.
  const auto straddle = body(10, 0x80);
  result = ring.try_push(straddle.data(), straddle.size());
  assert(result == EnqueueResult::Ok);
  const auto expected = wire(straddle);
  const std::uint8_t *data = nullptr;
  std::size_t span = ring.peek(&data);
  assert(span == 6 && data == storage + 10);
  std::vector<std::uint8_t> got(data, data + span);
  ring.consume(span);
  assert(ring.mid_frame() && ring.frames() == 1);
  span = ring.peek(&data);
  assert(span == 6 && data == storage);
  got.insert(got.end(), data, data + span);
  ring.consume(span);
  assert(got == expected);
  assert(ring.empty() && !ring.mid_frame() && ring.frames() == 0);

  // The prefix itself straddles the end. Head at 6, push 9 bytes to land
  // at 15 so the next prefix is split across 15 and 0.
  const auto pad = body(7, 1);
  result = ring.try_push(pad.data(), pad.size());
  assert(result == EnqueueResult::Ok);
  ring.consume(9);
  const auto split = body(4, 0xc0);
  result = ring.try_push(split.data(), split.size());
  assert(result == EnqueueResult::Ok);
  span = ring.peek(&data);
  assert(span == 1 && data == storage + 15);
  got.assign(data, data + span);
  ring.consume(1);
  assert(ring.mid_frame());
  got.insert(got.end(), storage, storage + ring.used());
  assert(got == wire(split));
  assert(drain(ring) == std::vector<std::uint8_t>(got.begin() + 1, got.end()));
}

void test_partial_consume() {
  std::uint8_t storage[64];
  FrameRing ring;
  ring.attach(storage, sizeof(storage));
  const auto a = body(5, 0x11), b = body(3, 0x22);
  EnqueueResult result = ring.try_push(a.data(), a.size());
  assert(result == EnqueueResult::Ok);
  result = ring.try_push(b.data(), b.size());
  assert(result == EnqueueResult::Ok);
  assert(ring.frames() == 2 && !ring.mid_frame());

  ring.consume(1); // half of the prefix of a
  assert(ring.mid_frame() && ring.frames() == 2 && ring.used() == 11);
  ring.consume(2); // rest of the prefix and one body byte
  assert(ring.mid_frame() && ring.frames() == 2);
  ring.consume(4); // exactly the end of a
  assert(!ring.mid_frame() && ring.frames() == 1 && ring.used() == 5);
  ring.consume(3); // prefix of b plus one body byte
  assert(ring.mid_frame() && ring.frames() == 1);
  const std::uint8_t *data = nullptr;
  assert(ring.peek(&data) == 2 && data[0] == b[1] && data[1] == b[2]);
  ring.consume(100); // clamps to what is queued
  assert(ring.empty() && ring.frames() == 0 && !ring.mid_frame());
  assert(ring.consumed_total() == ring.pushed_total());

  // One consume across several whole frames and into the next.
  for (int i = 0; i < 3; ++i) {
    result = ring.try_push(a.data(), a.size());
    assert(result == EnqueueResult::Ok);
  }
  ring.consume(15);
  assert(ring.frames() == 1 && ring.mid_frame() && ring.used() == 6);
  ring.consume(0);
  assert(ring.frames() == 1 && ring.used() == 6);
  ring.consume(6);
  assert(ring.empty() && ring.frames() == 0);
}

void test_drop_unstarted() {
  std::uint8_t storage[64];
  FrameRing ring;
  ring.attach(storage, sizeof(storage));
  const auto a = body(6, 0x31), b = body(4, 0x41), c = body(2, 0x51);
  for (const auto *frame : {&a, &b, &c}) {
    const EnqueueResult result = ring.try_push(frame->data(), frame->size());
    assert(result == EnqueueResult::Ok);
  }
  const std::uint64_t pushed = ring.pushed_total();
  ring.consume(3);
  assert(ring.mid_frame());
  std::size_t dropped = ring.drop_unstarted();
  assert(dropped == 2 && ring.frames() == 1 && ring.used() == 5);
  assert(ring.mid_frame());
  assert(ring.pushed_total() == pushed);
  assert(ring.consumed_total() + ring.used() == ring.pushed_total());
  const auto rest = wire(a);
  assert(drain(ring) ==
         std::vector<std::uint8_t>(rest.begin() + 3, rest.end()));
  assert(ring.empty() && !ring.mid_frame() && ring.frames() == 0);

  // A new frame after the drop lands on a boundary.
  EnqueueResult result = ring.try_push(c.data(), c.size());
  assert(result == EnqueueResult::Ok);
  assert(drain(ring) == wire(c));

  // On a boundary everything goes.
  for (const auto *frame : {&a, &b}) {
    result = ring.try_push(frame->data(), frame->size());
    assert(result == EnqueueResult::Ok);
  }
  dropped = ring.drop_unstarted();
  assert(dropped == 2 && ring.empty() && ring.frames() == 0);
  assert(!ring.mid_frame());
  assert(ring.consumed_total() == ring.pushed_total());
  assert(ring.drop_unstarted() == 0);

  // Partly sent frame alone stays intact.
  result = ring.try_push(b.data(), b.size());
  assert(result == EnqueueResult::Ok);
  ring.consume(1);
  assert(ring.drop_unstarted() == 0 && ring.used() == 5 && ring.frames() == 1);
  ring.consume(5);
  assert(ring.empty() && ring.frames() == 0);
}

void test_lan_bound() {
  std::vector<std::uint8_t> storage(24 * 1024);
  FrameRing ring;
  ring.attach(storage.data(), storage.size());
  const auto frame = body(1024, 0x5a);
  std::size_t pushed = 0;
  for (;;) {
    const EnqueueResult result = ring.try_push(frame.data(), frame.size());
    assert(ring.used() <= ring.capacity());
    if (result == EnqueueResult::Full)
      break;
    assert(result == EnqueueResult::Ok);
    ++pushed;
  }
  assert(pushed == 24576 / 1026 && pushed == 23);
  assert(ring.frames() == 23 && ring.used() == 23 * 1026);
  // Once one frame drains another fits, and the ring wraps.
  ring.consume(1026);
  const EnqueueResult result = ring.try_push(frame.data(), frame.size());
  assert(result == EnqueueResult::Ok);
  assert(ring.used() <= ring.capacity() && ring.frames() == 23);
  const auto out = drain(ring);
  const auto one = wire(frame);
  assert(out.size() == 23 * one.size());
  for (std::size_t i = 0; i < out.size(); ++i)
    assert(out[i] == one[i % one.size()]);
}

void test_differential() {
  constexpr std::size_t kCapacity = 97;
  std::uint8_t storage[kCapacity];
  FrameRing ring;
  ring.attach(storage, kCapacity);
  std::deque<std::uint8_t> model;
  // Wire sizes of frames still queued, the front one possibly partly sent.
  std::deque<std::size_t> frames;
  std::size_t head_sent = 0;
  std::uint64_t pushed = 0, consumed = 0;
  bool closed = false;
  std::mt19937 rng(0x5eed);
  std::size_t oks = 0, fulls = 0, wraps = 0;

  for (int op = 0; op < 5000; ++op) {
    const unsigned choice = rng() % 100;
    if (choice < 45) {
      const std::size_t length = 1 + rng() % 40;
      const auto frame = body(length, static_cast<std::uint8_t>(rng()));
      const std::size_t free = kCapacity - model.size();
      const EnqueueResult result = ring.try_push(frame.data(), frame.size());
      if (closed) {
        assert(result == EnqueueResult::Closed);
      } else if (length + 2 > free) {
        assert(result == EnqueueResult::Full);
        ++fulls;
      } else {
        assert(result == EnqueueResult::Ok);
        const auto bytes = wire(frame);
        model.insert(model.end(), bytes.begin(), bytes.end());
        frames.push_back(bytes.size());
        pushed += bytes.size();
        ++oks;
      }
    } else if (choice < 90) {
      const std::uint8_t *data = nullptr;
      const std::size_t span = ring.peek(&data);
      if (model.empty()) {
        assert(span == 0 && data == nullptr);
      } else {
        assert(span > 0 && span <= model.size());
        assert(data >= storage && data + span <= storage + kCapacity);
        for (std::size_t i = 0; i < span; ++i)
          assert(data[i] == model[i]);
        if (span < model.size())
          ++wraps;
      }
      const std::size_t take = span ? 1 + rng() % (span + 3) : rng() % 3;
      const std::size_t actual = take < model.size() ? take : model.size();
      ring.consume(take);
      model.erase(model.begin(), model.begin() + actual);
      consumed += actual;
      std::size_t left = actual;
      while (left) {
        const std::size_t remaining = frames.front() - head_sent;
        if (left >= remaining) {
          left -= remaining;
          frames.pop_front();
          head_sent = 0;
        } else {
          head_sent += left;
          left = 0;
        }
      }
    } else if (choice < 95) {
      const std::size_t keep = head_sent ? frames.front() - head_sent : 0;
      const std::size_t expected = frames.size() - (head_sent ? 1 : 0);
      const std::size_t dropped = ring.drop_unstarted();
      assert(dropped == expected);
      consumed += model.size() - keep;
      model.resize(keep);
      frames.resize(head_sent ? 1 : 0);
    } else if (choice < 97) {
      ring.close();
      closed = true;
    } else {
      ring.reset();
      closed = false;
      consumed += model.size();
      model.clear();
      frames.clear();
      head_sent = 0;
    }
    assert(ring.used() == model.size());
    assert(ring.used() <= ring.capacity());
    assert(ring.free() == kCapacity - model.size());
    assert(ring.frames() == frames.size());
    assert(ring.mid_frame() == (head_sent != 0));
    assert(ring.closed() == closed);
    assert(ring.pushed_total() == pushed);
    // reset() and drop_unstarted() count discarded bytes as consumed.
    assert(ring.consumed_total() == consumed);
    assert(ring.consumed_total() + ring.used() == ring.pushed_total());
    if (model.empty())
      assert(ring.consumed_total() == ring.pushed_total());
  }
  assert(oks > 500 && fulls > 50 && wraps > 50);
  std::printf("differential: %zu ok, %zu full, %zu wrapped peeks\n", oks, fulls,
              wraps);
}
} // namespace

int main() {
  test_prefix_and_results();
  test_refused_lengths();
  test_fits();
  test_wrap();
  test_partial_consume();
  test_drop_unstarted();
  test_lan_bound();
  test_differential();
  std::printf("frame ring passed\n");
  return 0;
}
