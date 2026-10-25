#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc; build and host tests verify exact bytes.
constexpr const char *kDictionarySha256 =
    "0835ea4a4589983d8ffbd57fd040db24c98885f93c42f01f65c3b9622e38d93f";
} // namespace contract
} // namespace telemetry
