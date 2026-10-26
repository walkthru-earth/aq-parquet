#pragma once

// Bluetooth wire encoding shared by the native transport and the host
// interoperability fixture. No host-stack, controller or board dependencies.
#include "ble_sync.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <expected>
#include <span>

namespace ble::wire {
constexpr std::array<std::uint8_t, 16> uuid(std::uint16_t selector) {
  return {0x80,
          0xb6,
          0x99,
          0x41,
          0x99,
          0x59,
          0x0d,
          0x8e,
          0xed,
          0x47,
          static_cast<std::uint8_t>(selector),
          static_cast<std::uint8_t>(selector >> 8),
          0x5e,
          0x59,
          0x5f,
          0x25};
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

// Longest name that leaves the legacy scan response room for the complete
// 128-bit service UUID list (see legacy_scan_response).
inline constexpr std::size_t kMaxLocalName = 11;
inline bool legacy_scan_response(const char *name, ScanResponse &result) {
  if (!name)
    return false;
  // Twenty bytes of overhead: two for name AD + eighteen for UUID list AD.
  const auto length = std::strlen(name);
  if (length > kMaxLocalName)
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

// Device id characters kept after the prefix.
inline constexpr std::size_t kNameDeviceChars = 4;

// A prefix is 1 to 7 characters of [A-Za-z0-9-], so the name stays within
// kMaxLocalName and its lower case form is a valid mDNS host label.
[[nodiscard]] constexpr bool valid_name_prefix(const char *prefix) {
  if (!prefix || !*prefix)
    return false;
  std::size_t length = 0;
  for (; prefix[length]; ++length) {
    const char c = prefix[length];
    if (length >= kMaxLocalName - kNameDeviceChars ||
        !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-'))
      return false;
  }
  return true;
}

// BLE local name, the board prefix plus the last four device id characters
// (OS- and 0123456789ab give OS-89ab). False with an empty `out` when the
// prefix is invalid or the result does not fit `out` or kMaxLocalName.
[[nodiscard]] inline bool local_name(std::span<char> out, const char *prefix,
                                     const char *device) {
  if (out.empty())
    return false;
  out[0] = '\0';
  if (!device || !valid_name_prefix(prefix))
    return false;
  const auto device_length = std::strlen(device);
  const char *suffix = device_length >= kNameDeviceChars
                           ? device + device_length - kNameDeviceChars
                           : device;
  const auto length = std::strlen(prefix) + std::strlen(suffix);
  if (length > kMaxLocalName || length >= out.size())
    return false;
  std::memcpy(out.data(), prefix, std::strlen(prefix));
  std::memcpy(out.data() + std::strlen(prefix), suffix, std::strlen(suffix));
  out[length] = '\0';
  return true;
}

// mDNS host label mirrors the BLE name in lower case (OS-6b40 to os-6b40).
[[nodiscard]] inline bool mdns_host(std::span<char> out, const char *name) {
  if (out.empty() || !name)
    return false;
  const auto length = std::strlen(name);
  if (length == 0 || length >= out.size()) {
    out[0] = '\0';
    return false;
  }
  for (std::size_t i = 0; i <= length; ++i) {
    const char c = name[i];
    out[i] = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
  }
  return true;
}

enum class InfoError : std::uint8_t {
  Missing,  // a required identity string is null or desc is under 16 hex
  Overflow, // the JSON and its NUL do not fit `out`
};

// Characters of desc that info carries. The full 64 hex value is inside the
// template GET_DESCRIPTOR serves.
inline constexpr std::size_t kInfoDescChars = 16;

// The info JSON shared by the BLE info characteristic and the LAN HELLO.
// Returns the length without the NUL that always follows it. Built with
// appends, no snprintf, so the 400 byte cap is checked in one place.
[[nodiscard]] inline std::expected<std::size_t, InfoError>
build_info(std::span<char> out, const Identity &identity) {
  const char *strings[] = {identity.firmware,
                           identity.schema,
                           identity.dictionary_sha256,
                           identity.descriptor_sha256,
                           identity.station,
                           identity.device,
                           identity.boot};
  if (std::ranges::any_of(strings, [](const char *text) { return !text; }) ||
      std::strlen(identity.descriptor_sha256) < kInfoDescChars)
    return std::unexpected(InfoError::Missing);
  std::size_t used = 0;
  bool fits = true;
  const auto append = [&](const char *text, std::size_t length) {
    if (!fits || length >= out.size() - used) {
      fits = false;
      return;
    }
    std::memcpy(out.data() + used, text, length);
    used += length;
  };
  const auto literal = [&](const char *text) {
    append(text, std::strlen(text));
  };
  const auto number = [&](std::uint32_t value) {
    char digits[10];
    std::size_t count = 0;
    do {
      digits[sizeof(digits) - 1 - count++] =
          static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value);
    append(digits + sizeof(digits) - count, count);
  };
  if (out.empty())
    return std::unexpected(InfoError::Overflow);
  literal("{\"proto\":");
  number(kProtocolVersion);
  literal(",\"fw\":\"");
  literal(identity.firmware);
  literal("\",\"schema\":\"");
  literal(identity.schema);
  literal("\",\"cols\":");
  number(identity.columns);
  literal(",\"dict\":\"");
  literal(identity.dictionary_sha256);
  literal("\",\"desc\":\"");
  append(identity.descriptor_sha256, kInfoDescChars);
  literal("\",\"station\":\"");
  literal(identity.station);
  literal("\",\"dev\":\"");
  literal(identity.device);
  literal("\",\"boot\":\"");
  literal(identity.boot);
  literal("\",\"max_read\":");
  number(kMaxRead);
  literal("}");
  if (!fits) {
    out[0] = '\0';
    return std::unexpected(InfoError::Overflow);
  }
  out[used] = '\0';
  return used;
}

constexpr std::uint16_t response_capacity(std::uint16_t negotiated_mtu) {
  if (negotiated_mtu <= 3)
    return 0;
  const auto capacity = static_cast<std::uint16_t>(negotiated_mtu - 3);
  return capacity > kMaxFrame ? kMaxFrame : capacity;
}
} // namespace ble::wire
