#pragma once

// One upload step (#81). Ties the policy, the STS exchange, the signer and
// the ledger together behind small interfaces, so the whole flow runs on
// the host against fakes. The device supplies an HTTPS transport
// (os_upload_device.h) and, in the integration PR, an archive and a ledger
// routed through the storage worker, the only task that touches files.

#include "os_upload_ledger.h"
#include "os_upload_policy.h"
#include "os_upload_sigv4.h"
#include "os_upload_sts.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace opensensor::upload {

inline constexpr std::string_view kParquetContentType =
    "application/vnd.apache.parquet";
// Credentials closer than this to expiry are refreshed first.
inline constexpr std::int64_t kRefreshMarginSeconds = 300;
inline constexpr std::size_t kResponseMax = 4096;
inline constexpr std::size_t kChunkBytes = 1024;

enum class IoError : std::uint8_t { Failed };

class BodySource {
public:
  [[nodiscard]] virtual std::expected<std::size_t, IoError>
  read(std::uint64_t offset, std::span<std::uint8_t> out) = 0;

protected:
  ~BodySource() = default;
};

struct HttpRequest {
  std::string_view method;
  std::string_view host;
  // Already encoded, sent as is.
  std::string_view path;
  // Sent as given. `host` and `content-length` come from the transport.
  std::span<const Header> headers;
  std::uint64_t body_bytes = 0;
  BodySource *body = nullptr;
};

struct HttpResponse {
  int status = 0;
  std::size_t body_bytes = 0;
  bool truncated = false;
  std::array<char, kMaxEtag + 4> etag{};
  std::size_t etag_bytes = 0;

  [[nodiscard]] std::string_view etag_view() const {
    return {etag.data(), etag_bytes};
  }
};

enum class TransportError : std::uint8_t { Connect, Io, Body, Busy };

class Transport {
public:
  // Sends the request, then reads up to `body_out.size()` response bytes.
  [[nodiscard]] virtual std::expected<HttpResponse, TransportError>
  send(const HttpRequest &request, std::span<char> body_out) = 0;

protected:
  ~Transport() = default;
};

class Archive {
public:
  // Offers every file below the archive root to `next`. False on I/O error.
  [[nodiscard]] virtual bool list(NextFile &next) = 0;
  [[nodiscard]] virtual std::expected<std::size_t, IoError>
  read(std::string_view name, std::uint64_t offset,
       std::span<std::uint8_t> out) = 0;

protected:
  ~Archive() = default;
};

class Ledger : public LedgerView {
public:
  // Appends one line, adding a newline first after a torn tail. Never
  // rewrites or truncates earlier lines.
  [[nodiscard]] virtual bool append(const LedgerEntry &entry) = 0;

protected:
  ~Ledger() = default;
};

struct SessionConfig {
  // "data.source.coop"
  std::string_view host;
  // The source.coop account, the first path segment.
  std::string_view bucket;
  std::string_view region;
  Destination destination;
  std::string_view role_arn;
  std::string_view session_name;
  // The board API key, a `<sck key>`. Only ever sent in the STS body.
  std::string_view api_key;
};

struct StepResult {
  Gate gate = Gate::Disabled;
  // Meaningful only when `gate == Gate::Run`.
  Outcome outcome = Outcome::NothingPending;
};

class Session {
public:
  Session(const SessionConfig &config, Sha256 &hash, Transport &transport,
          Archive &archive, Ledger &ledger)
      : config_(config), hash_(hash), transport_(transport), archive_(archive),
        ledger_(ledger) {}

  // At most one file per call. `utc_s` is only read when the gate opens.
  StepResult step(const Inputs &in, std::int64_t utc_s);

  [[nodiscard]] const PolicyState &state() const { return state_; }
  [[nodiscard]] bool has_credentials() const { return creds_.present(); }
  void forget_credentials() { creds_.wipe(); }

private:
  Outcome run(std::int64_t utc_s);
  // nullopt when fresh credentials are in place, else the failure.
  std::optional<Outcome> refresh(std::int64_t utc_s);
  Outcome put(std::string_view name, std::uint64_t bytes, std::int64_t utc_s);

  const SessionConfig &config_;
  Sha256 &hash_;
  Transport &transport_;
  Archive &archive_;
  Ledger &ledger_;
  PolicyState state_{};
  Credentials creds_{};
  std::array<char, kResponseMax> response_{};
  std::array<std::uint8_t, kChunkBytes> chunk_{};
};

} // namespace opensensor::upload
