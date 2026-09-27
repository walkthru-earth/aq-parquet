#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace aq {
namespace sync {
inline void put_u16(std::uint8_t *out, std::uint16_t value) {
  out[0] = value & 0xff;
  out[1] = value >> 8;
}
inline void put_u32(std::uint8_t *out, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    out[i] = (value >> (8 * i)) & 0xff;
}
inline void put_i64(std::uint8_t *out, std::int64_t value) {
  const auto bits = static_cast<std::uint64_t>(value);
  for (unsigned i = 0; i < 8; ++i)
    out[i] = (bits >> (8 * i)) & 0xff;
}
inline std::uint16_t get_u16(const std::uint8_t *in) {
  return static_cast<std::uint16_t>(in[0] | (in[1] << 8));
}
inline std::uint32_t get_u32(const std::uint8_t *in) {
  return std::uint32_t(in[0]) | (std::uint32_t(in[1]) << 8) |
         (std::uint32_t(in[2]) << 16) | (std::uint32_t(in[3]) << 24);
}
inline std::int64_t get_i64(const std::uint8_t *in) {
  std::uint64_t bits = 0;
  for (unsigned i = 0; i < 8; ++i)
    bits |= std::uint64_t(in[i]) << (8 * i);
  return static_cast<std::int64_t>(bits);
}

// CRC-32/ISO-HDLC running state: initialize to 0xffffffff, then XOR the final
// state with 0xffffffff. Chunk boundaries do not alter the result.
inline std::uint32_t crc_update(std::uint32_t crc, const std::uint8_t *data,
                                std::size_t size) {
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return crc;
}

// Relative finalized-file names accepted by the sync protocol. Mount roots
// and legacy routing belong to the archive owner, not this validator.
inline bool safe_parquet_name(const char *name, std::size_t max_length = 384) {
  if (!name)
    return false;
  const std::size_t length = std::strlen(name);
  if (length < 9 || length > max_length || name[0] == '/' ||
      std::strcmp(name + length - 8, ".parquet") != 0)
    return false;
  for (std::size_t i = 0; i < length; ++i)
    if (!((name[i] >= '0' && name[i] <= '9') ||
          (name[i] >= 'a' && name[i] <= 'z') || name[i] == '-' ||
          name[i] == '.' || name[i] == '_' || name[i] == '=' || name[i] == '/'))
      return false;
  return std::strstr(name, "..") == nullptr &&
         std::strstr(name, "//") == nullptr;
}

// Structural completion only; reader conformance remains a separate test.
// `head` is the first four bytes, `tail` the final eight (footer size + PAR1).
inline bool parquet_complete(std::uint64_t size, const std::uint8_t *head,
                             const std::uint8_t *tail,
                             std::uint64_t max_size = 1048576) {
  if (!head || !tail || size < 12 || size > max_size)
    return false;
  const std::uint32_t footer = get_u32(tail);
  return std::memcmp(head, "PAR1", 4) == 0 &&
         std::memcmp(tail + 4, "PAR1", 4) == 0 && footer > 0 &&
         footer <= size - 12;
}
} // namespace sync
} // namespace aq
