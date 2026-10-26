#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc; build and host tests verify exact bytes.
constexpr const char *kDictionarySha256 =
    "3e0d0278be007b22d426171c8e4167ef2e117078bd3db066029b830b8df2dcae";
} // namespace contract
} // namespace telemetry
