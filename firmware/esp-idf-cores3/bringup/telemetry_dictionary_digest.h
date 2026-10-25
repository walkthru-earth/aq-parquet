#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc, checked by build.sh and host contract tests.
// It hashes raw bytes, so any edit to that file, comments and whitespace
// included, moves the digest and needs a new LIVE fixture and a phone release.
constexpr const char *kDictionarySha256 =
    "4f80c26cde8d7ea78c2160225d98386009a8651d9a049b4bf68a75da96015e94";
} // namespace contract
} // namespace telemetry
