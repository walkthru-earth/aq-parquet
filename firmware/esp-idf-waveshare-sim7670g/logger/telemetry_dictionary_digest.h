#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc; build and host tests verify exact bytes.
constexpr const char *kDictionarySha256 =
    "715685cf83ac8d6975fde845ce58fd15fa6eaaf3df32644ec15dfc89875d2e9e";
} // namespace contract
} // namespace telemetry
