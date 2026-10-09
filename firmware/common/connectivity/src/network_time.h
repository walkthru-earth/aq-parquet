#pragma once

#include <cstddef>
#include <cstdint>

// Board-neutral native SNTP service. One network owner drives lifecycle;
// status and the bounded pending anchor may be consumed by other tasks.
namespace aq::network_time {
constexpr std::size_t kServerCount = 2;
constexpr std::size_t kServerMax = 253;
constexpr std::uint32_t kMinIntervalSeconds = 60;
constexpr std::uint32_t kMaxIntervalSeconds = 86400;
struct Policy {
  bool enabled = true;
  bool dhcp = true;
  bool public_fallback = true;
  char servers[kServerCount][kServerMax + 1]{};
  std::uint32_t interval_s = 3600;
};
struct Anchor {
  std::int64_t seconds = 0;
  std::uint32_t subsecond_us = 0;
  std::int64_t monotonic_us = 0;
};
struct Status {
  const char *state = "offline";
  // disabled | invalid | offline | unconfigured | resolving | waiting |
  // retry | synchronized | stale. Offline/stale never erase real evidence.
  bool enabled = true;
  bool running = false;
  bool has_anchor = false;
  bool stale = false;
  bool public_active = false; // last successful anchor used public fallback
  std::int32_t source = 0;    // 3 only after a supported SNTP response
  Anchor last_sync{};
  std::uint64_t age_s = 0; // meaningful only when has_anchor
  std::uint32_t sync_count = 0;
  std::uint32_t interval_s = 3600;
  unsigned server_count = 0;
  int error = 0;
  char server[kServerMax + 1]{}; // successful endpoint, never inferred country
};
// Invalid policies stop SNTP and return false. An empty local list is valid;
// DHCP option 42 is preferred, then explicit locals, then permitted public NTP.
// No country inference or claim of DNS endpoint residency/authentication.
bool configure(const Policy &policy);
// Call before association/DHCP: enables option 42 and clears old native slots.
void prepare_network();
// Call from the owner task after a new IP event, not the event callback.
void ip_changed();
void set_network_available(bool available);
// No network response waits. Async DNS contexts carry a generation and remain
// alive until completion; only numeric addresses are handed to native SNTP.
void poll();
bool take_anchor(Anchor &anchor);
Status status();
} // namespace aq::network_time
