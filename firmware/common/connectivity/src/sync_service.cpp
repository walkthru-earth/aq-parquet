#include "sync_service.h"
#include "device_config.h"
#include "wifi_link.h"

#include <cstdio>

namespace aqsync {
namespace {
ble::RequestHandler worker_handler = nullptr;
bool dispatch_request(const ble::ControlRequest &request) {
  if (request.length && request.bytes[0] == ble::kOpWifiScan &&
      lan::request_scan(request.link, request.link_generation, request.peer))
    return true;
  // A pending scan is queued so common control emits busy on its link.
  return worker_handler(request);
}
} // namespace
StartResult begin(const ble::Identity &identity, ble::RequestHandler handler,
                  bool display_detected) {
  StartResult result;
  if (!handler)
    return result;
  worker_handler = handler;
  result.settings_loaded = config::load(display_detected);
  const config::Settings settings = config::get();
  result.ble_started =
      ble::begin(identity, settings.pair, settings.pin, dispatch_request);
  // mDNS host label mirrors the BLE name: AQ-6b40 -> aq-6b40.
  char host[24];
  std::snprintf(host, sizeof(host), "%s", ble::local_name());
  for (char *p = host; *p; ++p)
    if (*p >= 'A' && *p <= 'Z')
      *p = static_cast<char>(*p - 'A' + 'a');
  result.lan_started = lan::begin(host, identity, dispatch_request);
  return result;
}
} // namespace aqsync
