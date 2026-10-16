#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc; build and host tests verify exact bytes.
constexpr const char *kDictionarySha256 =
    "0a024b85c632f621c0e1223f1ecafe3a9c4524a1ec3c0d03d910d191aaee2f82";
} // namespace contract
} // namespace telemetry
