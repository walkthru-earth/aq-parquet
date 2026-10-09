#include "ble_advert.h"

#include <cassert>
#include <cstdio>

namespace {
void hex(const std::uint8_t *bytes, std::size_t length) {
  for (std::size_t i = 0; i < length; ++i)
    std::printf("%02x", unsigned(bytes[i]));
  std::putchar('\n');
}
} // namespace

int main() {
  // Emit actual firmware wire bytes for independent decoding by the shipped
  // Python client. This is host interoperability evidence, not a radio test.
  for (unsigned selector = 1; selector <= 6; ++selector) {
    const auto value = ble::wire::uuid(selector);
    hex(value.data(), value.size());
  }
  for (const auto flags :
       {ble::kAdvNoUtc, ble::kAdvNewFiles, ble::kAdvSd, ble::kAdvLan,
        ble::kAdvFail, ble::kAdvClockRestored}) {
    const auto advert = ble::wire::legacy_advert(flags, 0x89abcdefU, 0x7654);
    hex(advert.data(), advert.size());
  }
  for (const auto counter : {0U, 0xffffffffU}) {
    const auto advert = ble::wire::legacy_advert(0, counter, 0xffff);
    hex(advert.data(), advert.size());
  }
  for (const char *name : {"AQ-6b40", "12345678901"}) {
    ble::wire::ScanResponse response;
    assert(ble::wire::legacy_scan_response(name, response));
    hex(response.bytes.data(), response.length);
  }
  ble::wire::ScanResponse response;
  assert(!ble::wire::legacy_scan_response("123456789012", response));
  assert(!ble::wire::legacy_scan_response(nullptr, response));
  assert(ble::wire::response_capacity(0) == 0);
  assert(ble::wire::response_capacity(3) == 0);
  assert(ble::wire::response_capacity(23) == 20);
  assert(ble::wire::response_capacity(247) == 244);
  assert(ble::wire::response_capacity(517) == 512);
  assert(ble::wire::response_capacity(65535) == 512);
}
