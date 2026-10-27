#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc; build and host tests verify exact bytes.
constexpr const char *kDictionarySha256 =
    "a8605aafb6f76b794e9e7fb32927be6d203afeeb14527206c0e49c1eb0436809";
} // namespace contract
} // namespace telemetry
