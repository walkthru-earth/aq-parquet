// Compile-only integration fixture. Never flash as a device firmware.
// No M5Unified, GPIO, board sensors or card mount API participates here.
#include <sync_service.h>
#include <wifi_link.h>

namespace {
bool enqueue(const ble::ControlRequest &) { return false; }
constexpr ble::Identity identity{
    "compile-fixture", "000000000000", "00000000", "compile-only", 0,
    "compile-only",    "compile-only"};
} // namespace

void setup() {
  aqsync::begin(identity, enqueue, false);
  ble::publish_status("{}", 2);
  lan::publish_status("{}", 2);
}
void loop() {}
