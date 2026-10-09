#pragma once
#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
struct ip_addr_t {
  std::uint8_t bytes[16]{};
  int family = AF_INET;
};
inline int ipaddr_aton(const char *name, ip_addr_t *address) {
  *address = {};
  if (inet_pton(AF_INET, name, address->bytes) == 1)
    return 1;
  address->family = AF_INET6;
  return inet_pton(AF_INET6, name, address->bytes) == 1;
}
inline bool ip_addr_isany(const ip_addr_t *address) {
  for (const auto byte : address->bytes)
    if (byte)
      return false;
  return true;
}
inline char *ipaddr_ntoa_r(const ip_addr_t *address, char *out, int length) {
  return const_cast<char *>(
      inet_ntop(address->family, address->bytes, out, length));
}
