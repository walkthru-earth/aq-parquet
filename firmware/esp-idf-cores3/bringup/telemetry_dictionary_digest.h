#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc, checked by build.sh and host contract tests.
// It hashes raw bytes, so any edit to that file, comments and whitespace
// included, moves the digest and needs a new LIVE fixture and a phone release.
constexpr const char *kDictionarySha256 =
    "0763a1bb8ad3f8b185694fbb0382f7ec712191d8eb5ece8df01873a34804462c";
} // namespace contract
} // namespace telemetry
