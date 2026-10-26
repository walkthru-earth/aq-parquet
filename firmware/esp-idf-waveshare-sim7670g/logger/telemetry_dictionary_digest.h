#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc; build and host tests verify exact bytes.
constexpr const char *kDictionarySha256 =
    "8be98b66ab3090be16aaa4f33086cdf76ed403648d5700daa00e83889572197b";
} // namespace contract
} // namespace telemetry
