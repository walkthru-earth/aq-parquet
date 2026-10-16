#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc, checked by build.sh and host contract tests.
constexpr const char *kDictionarySha256 =
    "22291d3aab63365404394f30c1549fb18bde0699df9b2de863de70bb3f4edf65";
} // namespace contract
} // namespace telemetry
