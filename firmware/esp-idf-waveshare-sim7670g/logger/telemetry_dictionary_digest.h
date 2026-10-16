#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc; build and host tests verify exact bytes.
constexpr const char *kDictionarySha256 =
    "d0d0129440e04108d8ca1f9b9555f1b45316ee0ccf3af0fcf7c3a88c8c0796b8";
} // namespace contract
} // namespace telemetry
