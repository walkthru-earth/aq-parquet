// Compile the real DebugLog: radio diagnostics must never call a blocked sink.
#include <cassert>
#include <cstring>
#include <debug_log.h>
#include <string>

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

int main() {
  DebugLog log;
  Console console;
  log.set_output(
      [](void *context, const std::uint8_t *data, std::size_t size) {
        return static_cast<Console *>(context)->write(data, size);
      },
      &console);
  assert(log.record_only().printf("LAN CMD slot=%u op=0x%02x\n", 0u, 3u) > 0);
  const std::string oversized(DebugLog::kRingBytes + 256, 'R');
  assert(log.record_only().write(
             reinterpret_cast<const std::uint8_t *>(oversized.data()),
             oversized.size()) == oversized.size());
  assert(console.writes == 0);
  char tail[DebugLog::kRingBytes];
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
