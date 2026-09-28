#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc; build and host tests verify exact bytes.
constexpr const char *kDictionarySha256 =
    "e840c946c10378d28831b38f71daf66981d2ea99cf47ccba80fcd2e64f920ffd";
} // namespace contract
} // namespace telemetry
