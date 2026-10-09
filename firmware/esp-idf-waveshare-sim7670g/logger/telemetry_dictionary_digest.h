#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc; build and host tests verify exact bytes.
constexpr const char *kDictionarySha256 =
    "7cd1c69da4ecd728dde2736e4c55063d9707ef53cdba1745b8896efcfd60f5da";
} // namespace contract
} // namespace telemetry
