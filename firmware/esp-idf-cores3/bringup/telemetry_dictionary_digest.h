#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc, checked by build.sh and host contract tests.
constexpr const char *kDictionarySha256 =
    "f35d73981ca65d9de587a4ec7988ccea19e71dce988c69eb4bb99713f2352bb3";
} // namespace contract
} // namespace telemetry
