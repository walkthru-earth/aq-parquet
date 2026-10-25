#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc, checked by build.sh and host contract tests.
// It hashes raw bytes, so any edit to that file, comments and whitespace
// included, moves the digest and needs a new LIVE fixture and a phone release.
constexpr const char *kDictionarySha256 =
    "b0df83dc7a259448ca90c3d6911dc9ec74ebab371e8fc32329b924684bce128c";
} // namespace contract
} // namespace telemetry
