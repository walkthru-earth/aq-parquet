#include "os_upload_device.h"

#include "sdkconfig.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <psa/crypto.h>

#include <algorithm>
#include <climits>
#include <new>

namespace opensensor::upload {

bool enabled() {
#if defined(CONFIG_OS_UPLOAD_ENABLE)
  return true;
#else
  return false;
#endif
}

namespace {

static_assert(sizeof(psa_hash_operation_t) <= 256);
static_assert(alignof(psa_hash_operation_t) <= 8);

psa_hash_operation_t &operation(std::uint8_t *storage) {
  return *std::launder(reinterpret_cast<psa_hash_operation_t *>(storage));
}

bool iequals(const char *a, std::string_view b) {
  for (const char c : b) {
    const char x = *a++;
    const auto lower = [](char v) {
      return v >= 'A' && v <= 'Z' ? static_cast<char>(v - 'A' + 'a') : v;
    };
    if (x == '\0' || lower(x) != lower(c))
      return false;
  }
  return *a == '\0';
}

// Copies a view into a NUL terminated buffer. False when it does not fit.
template <std::size_t N>
bool terminated(std::string_view text, std::array<char, N> &out) {
  if (text.size() + 1 > N)
    return false;
  std::copy(text.begin(), text.end(), out.begin());
  out[text.size()] = '\0';
  return true;
}

esp_err_t on_event(esp_http_client_event_t *event) {
  if (event->event_id != HTTP_EVENT_ON_HEADER || event->user_data == nullptr ||
      event->header_key == nullptr || event->header_value == nullptr)
    return ESP_OK;
  if (!iequals(event->header_key, "etag"))
    return ESP_OK;
  auto &response = *static_cast<HttpResponse *>(event->user_data);
  const std::string_view value(event->header_value);
  // An oversized ETag is dropped, the ledger then records "-".
  if (value.size() <= response.etag.size()) {
    std::copy(value.begin(), value.end(), response.etag.begin());
    response.etag_bytes = value.size();
  }
  return ESP_OK;
}

// Buffers for one request. Static, so send() adds no large stack frame to
// the task that calls it. Only one upload runs at a time.
struct Scratch {
  std::array<char, 8 + 255 + 1 + 1800> url{};
  std::array<char, 64> name{};
  std::array<char, 2100> value{};
  std::array<std::uint8_t, kChunkBytes> chunk{};
};
Scratch scratch;

} // namespace

PsaSha256::PsaSha256() {
  ok_ = psa_crypto_init() == PSA_SUCCESS;
  new (operation_) psa_hash_operation_t(PSA_HASH_OPERATION_INIT);
}

PsaSha256::~PsaSha256() {
  psa_hash_abort(&operation(operation_));
  operation(operation_).~psa_hash_operation_t();
}

void PsaSha256::begin() {
  psa_hash_operation_t &op = operation(operation_);
  psa_hash_abort(&op);
  op = PSA_HASH_OPERATION_INIT;
  ok_ = psa_crypto_init() == PSA_SUCCESS &&
        psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS;
}

void PsaSha256::update(std::span<const std::uint8_t> data) {
  if (ok_ && !data.empty())
    ok_ = psa_hash_update(&operation(operation_), data.data(), data.size()) ==
          PSA_SUCCESS;
}

bool PsaSha256::finish(Digest &out) {
  if (!ok_) {
    psa_hash_abort(&operation(operation_));
    return false;
  }
  std::size_t length = 0;
  ok_ = psa_hash_finish(&operation(operation_), out.data(), out.size(),
                        &length) == PSA_SUCCESS &&
        length == out.size();
  return ok_;
}

std::expected<HttpResponse, TransportError>
HttpsTransport::send(const HttpRequest &request, std::span<char> body_out) {
  if (request.body_bytes > static_cast<std::uint64_t>(INT_MAX) ||
      (request.body_bytes > 0 && request.body == nullptr) ||
      !request.path.starts_with('/'))
    return std::unexpected(TransportError::Body);
  esp_http_client_method_t method = HTTP_METHOD_GET;
  if (request.method == "PUT")
    method = HTTP_METHOD_PUT;
  else if (request.method == "POST")
    method = HTTP_METHOD_POST;
  else if (request.method != "GET")
    return std::unexpected(TransportError::Body);

  auto &url = scratch.url;
  const std::string_view scheme = "https://";
  if (scheme.size() + request.host.size() + request.path.size() + 1 >
      url.size())
    return std::unexpected(TransportError::Body);
  auto end = std::copy(scheme.begin(), scheme.end(), url.begin());
  end = std::copy(request.host.begin(), request.host.end(), end);
  end = std::copy(request.path.begin(), request.path.end(), end);
  *end = '\0';

  HttpResponse response;
  // Zeroed, then set by name. The SDK struct grows between releases.
  esp_http_client_config_t config{};
  config.url = url.data();
  config.user_agent = "opensensor-space-firmware";
  config.method = method;
  config.timeout_ms = timeout_ms_;
  config.disable_auto_redirect = true;
  config.event_handler = on_event;
  config.transport_type = HTTP_TRANSPORT_OVER_SSL;
  config.buffer_size_tx = 1024;
  config.user_data = &response;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = false;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr)
    return std::unexpected(TransportError::Connect);
  const auto fail = [&](TransportError error) {
    esp_http_client_cleanup(client);
    std::fill(scratch.value.begin(), scratch.value.end(), '\0');
    return std::unexpected(error);
  };

  for (const Header &header : request.headers) {
    // The client writes Host and Content-Length itself.
    if (header.name == "host" || header.name == "content-length")
      continue;
    if (!terminated(header.name, scratch.name) ||
        !terminated(header.value, scratch.value) ||
        esp_http_client_set_header(client, scratch.name.data(),
                                   scratch.value.data()) != ESP_OK)
      return fail(TransportError::Body);
  }
  std::fill(scratch.value.begin(), scratch.value.end(), '\0');

  const int length = static_cast<int>(request.body_bytes);
  if (esp_http_client_open(client, length) != ESP_OK)
    return fail(TransportError::Connect);
  std::uint64_t offset = 0;
  while (offset < request.body_bytes) {
    const auto got = request.body->read(offset, scratch.chunk);
    if (!got || *got == 0)
      return fail(TransportError::Body);
    const int want = static_cast<int>(
        std::min<std::uint64_t>(*got, request.body_bytes - offset));
    int written = 0;
    while (written < want) {
      const int n = esp_http_client_write(
          client,
          reinterpret_cast<const char *>(scratch.chunk.data()) + written,
          want - written);
      if (n <= 0)
        return fail(TransportError::Io);
      written += n;
    }
    offset += static_cast<std::uint64_t>(want);
  }

  if (esp_http_client_fetch_headers(client) < 0)
    return fail(TransportError::Io);
  response.status = esp_http_client_get_status_code(client);
  // The body is kept for the caller and never logged.
  while (response.body_bytes < body_out.size()) {
    const int n = esp_http_client_read(
        client, body_out.data() + response.body_bytes,
        static_cast<int>(std::min<std::size_t>(
            body_out.size() - response.body_bytes, INT_MAX)));
    if (n < 0)
      return fail(TransportError::Io);
    if (n == 0)
      break;
    response.body_bytes += static_cast<std::size_t>(n);
  }
  if (response.body_bytes == body_out.size() &&
      !esp_http_client_is_complete_data_received(client))
    response.truncated = true;
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return response;
}

} // namespace opensensor::upload
