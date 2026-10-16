#pragma once

#include <aq_logger.h>
#include <cstddef>

namespace aqlogger {
// LIVE keys are short JSON names chosen by each board's field table. They are
// emitted verbatim, so keep them to 1..15 characters of [a-z0-9_].
constexpr std::size_t kMaxLiveKey = 15;

// True when every non-null live_key is well formed and no two fields share
// one. A table with no live keys is valid and publishes `{}`.
bool valid_live_keys(const Field *fields, std::size_t count);

// Formats one row as the LIVE JSON object. Emits every field that declares a
// live_key and holds a valid value, in field-table order. Null values and
// non-finite floats are omitted, never written as zero. INT32/INT64 print as
// integers and FLOAT with one decimal. Returns the length without the NUL, or
// zero when `capacity` is too small. Never reports truncated JSON as valid.
std::size_t format_live(const Field *fields, std::size_t count, const Row &row,
                        char *out, std::size_t capacity);
} // namespace aqlogger
