#include "sync_service.h"
#include "ble_advert.h"
#include "device_config.h"
#include "wifi_link.h"

namespace ossync {
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
                  bool display_detected, const ServiceConfig &config) {
  StartResult result;
  if (!handler)
    return result;
  worker_handler = handler;
  result.settings_loaded = config::load(display_detected);
  const config::Settings settings = config::get();
  result.ble_started = ble::begin(identity, settings.pair, settings.pin,
                                  dispatch_request, config.ble);
  // mDNS host label mirrors the BLE name in lower case (OS-6b40, os-6b40).
  // An empty host when BLE did not start keeps the earlier behavior.
  char host[24]{};
  (void)ble::wire::mdns_host(host, ble::local_name());
  result.lan_started = lan::begin(host, identity, dispatch_request, config.lan);
  return result;
}
} // namespace ossync
