#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc, checked by build.sh and host contract tests.
// It hashes raw bytes, so any edit to that file, comments and whitespace
// included, moves the digest and needs a new LIVE fixture and a phone release.
constexpr const char *kDictionarySha256 =
    "3b4d8500a19f42c6743b0f7d725276c96ca14a0c249f613048603afbc3cfaff9";
} // namespace contract
} // namespace telemetry
