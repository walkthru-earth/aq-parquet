#pragma once

// GET_DESCRIPTOR (0x11) wire encoding, shared by the worker dispatch and the
// host test. No host-stack, controller or board dependencies.
//
// Request body: offset u32, length u16. Reply: DESCRIPTOR_CHUNK (0x4f) with
// total u32, offset u32, then the bytes. One request returns one chunk, so a
// reader re-requests any range it lost. The bytes are copied from the board
// template in .rodata straight into the caller's frame buffer, no heap.
#include "ble_sync.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <span>

namespace ble::wire {
inline constexpr std::size_t kDescriptorRequestBytes = 6;
inline constexpr std::size_t kDescriptorHeaderBytes = 9;

enum class DescriptorError : std::uint8_t {
  Malformed,    // body is not exactly offset u32 + length u16
  ZeroLength,   // length 0 asks for nothing
  Unconfigured, // the board has no descriptor
  Range,        // offset >= total
  Mtu,          // the frame cannot carry the header and one byte
};

// Encodes the chunk into `frame`, whose size is the link payload budget.
// Returns the frame length. The count is the requested length clamped to what
// remains after `offset` and to the frame budget.
[[nodiscard]] inline std::expected<std::size_t, DescriptorError>
encode_descriptor_chunk(std::span<const char> descriptor,
                        std::span<const std::uint8_t> body,
                        std::span<std::uint8_t> frame) {
  if (body.size() != kDescriptorRequestBytes)
    return std::unexpected(DescriptorError::Malformed);
  if (descriptor.empty())
    return std::unexpected(DescriptorError::Unconfigured);
  const std::uint32_t offset =
      std::uint32_t(body[0]) | (std::uint32_t(body[1]) << 8) |
      (std::uint32_t(body[2]) << 16) | (std::uint32_t(body[3]) << 24);
  const std::size_t length = std::size_t(body[4]) | (std::size_t(body[5]) << 8);
  if (!length)
    return std::unexpected(DescriptorError::ZeroLength);
  const auto total = static_cast<std::uint32_t>(descriptor.size());
  if (offset >= total)
    return std::unexpected(DescriptorError::Range);
  if (frame.size() <= kDescriptorHeaderBytes)
    return std::unexpected(DescriptorError::Mtu);
  const std::size_t count = std::min({length, std::size_t(total - offset),
                                      frame.size() - kDescriptorHeaderBytes});
  frame[0] = kFrameDescriptorChunk;
  for (unsigned i = 0; i < 4; ++i) {
    frame[1 + i] = static_cast<std::uint8_t>(total >> (8 * i));
    frame[5 + i] = static_cast<std::uint8_t>(offset >> (8 * i));
  }
  std::memcpy(frame.data() + kDescriptorHeaderBytes, descriptor.data() + offset,
              count);
  return kDescriptorHeaderBytes + count;
}
} // namespace ble::wire
