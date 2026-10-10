// Compile the real DebugLog: radio diagnostics must never call a blocked sink.
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <debug_log.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class Console {
public:
  bool blocked = true;
  unsigned writes = 0;
  std::string received;
  std::size_t write(const std::uint8_t *bytes, std::size_t size) {
    assert(!blocked); // any diagnostic forwarding would block in the real sink
    ++writes;
    received.append(reinterpret_cast<const char *>(bytes), size);
    return size;
  }
};

namespace {
// The default board layout (runtime Kconfig), 64 KiB and 64 events.
constexpr DebugLog::Layout kLayout{65536, 64};
constexpr std::size_t kBlockBytes = DebugLog::persistent_bytes(kLayout);

void write_text(DebugLog &log, const std::string &text) {
  assert(log.record_only().write(
             reinterpret_cast<const std::uint8_t *>(text.data()),
             text.size()) == text.size());
}

std::string read_all(const DebugLog &log, std::size_t max, std::size_t slice) {
  std::uint32_t total = 0;
  auto cursor = log.tail_cursor(max, total);
  std::string out;
  std::vector<char> buffer(slice);
  while (cursor.remaining) {
    const auto count = log.read(cursor, buffer.data(), buffer.size());
    assert(count > 0 && count <= slice);
    out.append(buffer.data(), count);
  }
  return out;
}

std::string pattern(std::size_t size, unsigned seed) {
  std::string text(size, '\0');
  for (std::size_t i = 0; i < size; ++i)
    text[i] = static_cast<char>('a' + (i * 7 + seed) % 26);
  return text;
}

void persistent_ring() {
  // 4-byte aligned storage that survives a "warm reboot" (a fresh DebugLog
  // attaching the same bytes).
  std::vector<std::uint32_t> storage(kBlockBytes / 4 + 1, 0xa5a5a5a5u);
  void *block = storage.data();

  auto boot = std::make_unique<DebugLog>();
  boot->set_output(nullptr, nullptr);
  write_text(*boot, "early-line\n");
  std::uint8_t event[DebugLog::kEventBytes]{};
  event[0] = 0xe0;
  boot->record_event(event, 3);
  assert(boot->boot_seq() == 0 && !boot->persistent());
  assert(boot->ring_bytes() == DebugLog::kFallbackRingBytes);
  // Undersized or misaligned blocks are refused and keep the internal ring.
  assert(!boot->attach(block, kBlockBytes - 1, kLayout, true));
  assert(!boot->attach(static_cast<char *>(block) + 1, kBlockBytes, kLayout,
                       true));
  assert(!boot->persistent());
  assert(!boot->attach(block, kBlockBytes, kLayout, false)); // garbage header
  assert(boot->persistent() && boot->boot_seq() == 1);
  assert(boot->ring_bytes() == 65536);
  assert(read_all(*boot, 65535, 512) == "early-line\n");
  assert(boot->counter(3) == 1);
  std::uint8_t events[64][DebugLog::kEventBytes];
  assert(boot->events(events, 64) == 1 && events[0][0] == 0xe0);

  // 64 KiB ring wraps and keeps exactly the newest bytes.
  const std::string first = pattern(40000, 1);
  const std::string second = pattern(50000, 2);
  write_text(*boot, first);
  write_text(*boot, second);
  assert(boot->retained() == 65536);
  const std::string expected = (first + second).substr(90000 - 65535);
  assert(read_all(*boot, 65535, 1023) == expected);
  assert(read_all(*boot, 100000, 4096).size() == 65536);
  std::uint32_t total = 0;
  char newest[16];
  assert(boot->tail(newest, sizeof(newest), total) == sizeof(newest));
  assert(std::string(newest, sizeof(newest)) == second.substr(50000 - 16));
  assert(total == 11 + 90000);

  // A cursor overtaken by the writer is clipped forward, never misread.
  auto cursor = boot->tail_cursor(65535, total);
  char slice[1000];
  assert(boot->read(cursor, slice, sizeof(slice)) == sizeof(slice));
  const std::string overwrite = pattern(30000, 3);
  write_text(*boot, overwrite);
  std::string rest;
  while (cursor.remaining) {
    const auto count = boot->read(cursor, slice, sizeof(slice));
    if (!count)
      break;
    rest.append(slice, count);
  }
  const std::string window = (second + overwrite).substr(80000 - 65536);
  assert(rest.size() < 65535 - 1000);
  assert(window.compare(window.size() - rest.size() - 30000, rest.size(),
                        rest) == 0);

  // 64 events wrap; counters saturate per reason.
  for (unsigned i = 0; i < 70; ++i) {
    event[0] = static_cast<std::uint8_t>(i);
    boot->record_event(event, 5);
  }
  assert(boot->events(events, 64) == 64 && events[0][0] == 6 &&
         events[63][0] == 69);
  assert(boot->counter(5) == 70 && boot->counter(40) == 0);

  // Warm reboot keeps lines, events and counters and bumps boot_seq.
  const std::string before = read_all(*boot, 65535, 777);
  boot = std::make_unique<DebugLog>();
  boot->set_output(nullptr, nullptr);
  write_text(*boot, "second-boot\n");
  assert(boot->attach(block, kBlockBytes, kLayout, false));
  assert(boot->boot_seq() == 2);
  const std::string after = read_all(*boot, 65535, 512);
  assert(after == (before + "second-boot\n").substr(12));
  assert(boot->counter(5) == 70 && boot->counter(3) == 1);
  assert(boot->events(events, 64) == 64 && events[63][0] == 69);
  // Only bytes logged since this boot count toward LOG_END total.
  boot->tail(newest, 1, total);
  assert(total == 12);

  // A corrupt header (bit flip) is rejected and the block starts empty.
  static_cast<std::uint8_t *>(block)[20] ^= 0x01;
  boot = std::make_unique<DebugLog>();
  boot->set_output(nullptr, nullptr);
  assert(!boot->attach(block, kBlockBytes, kLayout, false));
  assert(boot->boot_seq() == 1 && boot->retained() == 0);
  assert(boot->counter(5) == 0 && boot->events(events, 64) == 0);

  // A cold boot clears a valid header too.
  write_text(*boot, "kept?\n");
  boot = std::make_unique<DebugLog>();
  boot->set_output(nullptr, nullptr);
  assert(!boot->attach(block, kBlockBytes, kLayout, true));
  assert(boot->retained() == 0 && boot->boot_seq() == 1);
  // A second attach is refused.
  assert(!boot->attach(block, kBlockBytes, kLayout, false));
}
} // namespace

// Writers and a LOG_TAIL reader race on a small ring. Every read is a
// contiguous span of the single writer's stream, located by the cursor.
void concurrent_slices() {
  constexpr DebugLog::Layout small{4096, 8};
  std::vector<std::uint32_t> storage(DebugLog::persistent_bytes(small) / 4 + 1);
  DebugLog log;
  log.set_output(nullptr, nullptr);
  assert(!log.attach(storage.data(), storage.size() * 4, small, true));
  const auto byte_at = [](std::uint64_t position) {
    return static_cast<char>('a' + position % 26);
  };
  constexpr std::size_t kWrites = 2000;
  std::thread writer([&] {
    std::uint64_t position = 0;
    std::string chunk;
    for (std::size_t i = 0; i < kWrites; ++i) {
      chunk.resize(1 + (i * 131) % 700); // some writes span several slices
      for (auto &c : chunk)
        c = byte_at(position++);
      write_text(log, chunk);
      std::uint8_t event[DebugLog::kEventBytes]{};
      std::memcpy(event, &i, sizeof(i));
      log.record_event(event, 1);
    }
  });
  std::vector<char> buffer(3000);
  std::uint8_t events[8][DebugLog::kEventBytes];
  for (unsigned pass = 0; pass < 400; ++pass) {
    std::uint32_t total = 0;
    auto cursor = log.tail_cursor(4096, total);
    while (cursor.remaining) {
      const auto count = log.read(cursor, buffer.data(), buffer.size());
      if (count == 0)
        break;
      const auto start = cursor.position - count;
      for (std::size_t i = 0; i < count; ++i)
        assert(buffer[i] == byte_at(start + i));
    }
    const auto n = log.events(events, 8);
    std::size_t previous = 0;
    for (std::size_t i = 0; i < n; ++i) {
      std::size_t sequence;
      std::memcpy(&sequence, events[i], sizeof(sequence));
      assert(i == 0 || sequence > previous); // oldest first, never reordered
      previous = sequence;
    }
  }
  writer.join();
  assert(log.counter(1) == kWrites);
}

int main() {
  persistent_ring();
  concurrent_slices();
  DebugLog log;
  Console console;
  log.set_output(
      [](void *context, const std::uint8_t *data, std::size_t size) {
        return static_cast<Console *>(context)->write(data, size);
      },
      &console);
  assert(log.record_only().printf("LAN CMD slot=%u op=0x%02x\n", 0u, 3u) > 0);
  const std::string oversized(DebugLog::kFallbackRingBytes + 256, 'R');
  assert(log.record_only().write(
             reinterpret_cast<const std::uint8_t *>(oversized.data()),
             oversized.size()) == oversized.size());
  assert(console.writes == 0);
  char tail[DebugLog::kFallbackRingBytes];
  std::uint32_t total = 0;
  assert(log.tail(tail, sizeof(tail), total) == sizeof(tail));
  assert(std::string(tail, sizeof(tail)) == oversized.substr(256));
  const auto diagnostic_total = total;

  // Serial protocol/list/status paths still forward every byte via normal log.
  console.blocked = false;
  const std::string protocol(549, 'D');
  assert(log.write(reinterpret_cast<const std::uint8_t *>(protocol.data()),
                   protocol.size()) == protocol.size());
  assert(console.received == protocol && console.writes == 1);
  assert(log.tail(tail, protocol.size(), total) == protocol.size());
  assert(std::string(tail, protocol.size()) == protocol);
  assert(total == diagnostic_total + protocol.size());

  const std::string long_text(2000, 'F');
  assert(log.record_only().printf("%s", long_text.c_str()) == 2000);
  assert(log.tail(tail, long_text.size(), total) == long_text.size());
  assert(std::string(tail, long_text.size()) == long_text);

  // Diagnostics remain ring-only even when the optional console is writable.
  const char final[] = "BLE READ file_handle=1 offset=0 bytes=16384\n";
  assert(log.record_only().write(reinterpret_cast<const std::uint8_t *>(final),
                                 sizeof(final) - 1) == sizeof(final) - 1);
  assert(console.writes == 1 && console.received == protocol);
  assert(log.tail(tail, sizeof(final) - 1, total) == sizeof(final) - 1);
  assert(std::memcmp(tail, final, sizeof(final) - 1) == 0);
}
