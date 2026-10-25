#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc; build and host tests verify exact bytes.
constexpr const char *kDictionarySha256 =
    "6f567b38fecc165f544626168931b3dc3d674482da87ed78cb0bec5bf3dc6ceb";
} // namespace contract
} // namespace telemetry
