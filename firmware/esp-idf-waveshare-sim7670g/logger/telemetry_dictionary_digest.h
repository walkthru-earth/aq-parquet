#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc; build and host tests verify exact bytes.
constexpr const char *kDictionarySha256 =
    "50a53393e940be182ded0b1119de6ac6d9521e7d28dfd244a24666ccf2a616c7";
} // namespace contract
} // namespace telemetry
