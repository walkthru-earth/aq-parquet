#pragma once

#include <cstddef>
#include <device_config.h>

namespace aqlogger {
// Sensitive output: callers must write this directly to the physical UART,
// never DebugLog, network transports or retained captures.
std::size_t format_wifi_profile(const config::Settings &, char *, std::size_t);
struct ProvisionResult {
  bool ok = false;
  bool reboot_required = false;
  char bad_key[48]{}; // allowlisted identifiers only; never untrusted input
};
// Physical serial worker only. Reuses the real common SET_CONFIG handler and
// its validators/actions with replies captured locally instead of transported.
ProvisionResult apply_config_hex(const char *hex);
} // namespace aqlogger
