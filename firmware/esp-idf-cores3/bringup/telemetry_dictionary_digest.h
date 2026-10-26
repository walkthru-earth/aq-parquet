#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc, checked by build.sh and host contract tests.
// It hashes raw bytes, so any edit to that file, comments and whitespace
// included, moves the digest and needs a new LIVE fixture and a phone release.
constexpr const char *kDictionarySha256 =
    "81b08af883fb56d3b32d7a0b813748ff93d4ab6f6669e5502d639574aab9fb1e";
} // namespace contract
} // namespace telemetry
