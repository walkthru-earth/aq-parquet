#pragma once

// Bluetooth wire encoding shared by the native transport and the host
// interoperability fixture. No host-stack, controller or board dependencies.
#include "ble_sync.h"

#include <array>
#include <cstring>

namespace ble::wire {
constexpr std::array<std::uint8_t, 16> uuid(std::uint16_t selector) {
  return {0x01,
          0x4e,
          0x6b,
          0x8a,
          0x7f,
          0x2d,
          0x3e,
          0x9c,
          0x1a,
          0x4b,
          static_cast<std::uint8_t>(selector),
          static_cast<std::uint8_t>(selector >> 8),
          0xf0,
          0xe9,
          0xa5,
          0xc0};
}

constexpr std::array<std::uint8_t, 31>
legacy_advert(std::uint8_t flags, std::uint32_t finalized, std::uint16_t boot) {
  // Flags AD (3 bytes) + 128-bit service data AD (28): the whole legacy PDU.
  std::array<std::uint8_t, 31> result{2, 0x01, 0x06, 27, 0x21};
  const auto service = uuid(1);
  for (unsigned i = 0; i < service.size(); ++i)
    result[5 + i] = service[i];
  result[21] = kAdvertVersion;
  result[22] = flags;
  for (unsigned i = 0; i < 4; ++i)
    result[23 + i] = static_cast<std::uint8_t>(finalized >> (8 * i));
  result[27] = static_cast<std::uint8_t>(boot);
  result[28] = static_cast<std::uint8_t>(boot >> 8);
  // The final two reserved bytes remain zero, never an IP address or port.
  return result;
}

struct ScanResponse {
  std::array<std::uint8_t, 31> bytes{};
  std::uint8_t length = 0;
};

inline bool legacy_scan_response(const char *name, ScanResponse &result) {
  if (!name)
    return false;
  // Twenty bytes of overhead: two for name AD + eighteen for UUID list AD.
  const auto length = std::strlen(name);
  if (length > 11)
    return false;
  result = {};
  result.bytes[0] = static_cast<std::uint8_t>(length + 1);
  result.bytes[1] = 0x09; // Complete Local Name.
  std::memcpy(result.bytes.data() + 2, name, length);
  result.bytes[length + 2] = 17;
  result.bytes[length + 3] = 0x07; // Complete List of 128-bit Service UUIDs.
  const auto service = uuid(1);
  std::memcpy(result.bytes.data() + length + 4, service.data(), service.size());
  result.length = static_cast<std::uint8_t>(length + 20);
  return true;
}

constexpr std::uint16_t response_capacity(std::uint16_t negotiated_mtu) {
  if (negotiated_mtu <= 3)
    return 0;
  const auto capacity = static_cast<std::uint16_t>(negotiated_mtu - 3);
  return capacity > kMaxFrame ? kMaxFrame : capacity;
}
} // namespace ble::wire
