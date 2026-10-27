#pragma once

// Device glue for the uploader (#81). A PSA SHA-256 and an HTTPS transport
// on esp_http_client with the certificate bundle. Compiled on both boards
// and unused until the integration PR wires a Session into the storage
// worker. No task is created here. Off unless CONFIG_OS_UPLOAD_ENABLE.

#include "os_upload_session.h"
#include "os_upload_sigv4.h"

#include <cstdint>

namespace opensensor::upload {

// True only when the image was built with CONFIG_OS_UPLOAD_ENABLE.
[[nodiscard]] bool enabled();

#if defined(ESP_PLATFORM)

class PsaSha256 final : public Sha256 {
public:
  PsaSha256();
  ~PsaSha256();
  PsaSha256(const PsaSha256 &) = delete;
  PsaSha256 &operator=(const PsaSha256 &) = delete;

  void begin() override;
  void update(std::span<const std::uint8_t> data) override;
  using Sha256::update;
  [[nodiscard]] bool finish(Digest &out) override;

private:
  // Opaque PSA operation, sized for psa_hash_operation_t.
  alignas(8) std::uint8_t operation_[256];
  bool ok_ = false;
};

// One HTTPS request per send(). The response body is never logged.
class HttpsTransport final : public Transport {
public:
  explicit HttpsTransport(int timeout_ms = 15000) : timeout_ms_(timeout_ms) {}

  [[nodiscard]] std::expected<HttpResponse, TransportError>
  send(const HttpRequest &request, std::span<char> body_out) override;

private:
  int timeout_ms_;
};

#endif

} // namespace opensensor::upload
