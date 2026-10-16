#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc; build and host tests verify exact bytes.
constexpr const char *kDictionarySha256 =
    "67688d0fd2e86053c1c66b961a1446ad3c225ee71823e0fadb771b645c7183ff";
} // namespace contract
} // namespace telemetry
