#pragma once

// Sensor module glue of the plugin contract (#17, docs/shared/plugins/README.md
// section 10). Each module has a generated <id>_module.h beside its manifest
// with a Column enum and kColumns in manifest order. resolve() maps those
// names to the board's column indices at build time, and a board hook loops
// over a static constexpr table of Module entries from its collect callback,
// so the logger and Hooks::collect keep their signatures.
//
// It lives in the logger component because Module::collect takes
// oslogger::Row. The sensor drivers stay free of the logger.

#include "os_logger.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ossensor {

// One board column index. The writer caps a board at kMaxColumns, so a byte
// holds every index and leaves kNoSlot free.
using Slot = std::uint8_t;
template <std::size_t N> using Slots = std::array<Slot, N>;
// A column of a segment the board does not select. Collect skips it.
inline constexpr Slot kNoSlot = 0xFF;
static_assert(telemetry::kMaxColumns < kNoSlot,
              "a Slot holds every column index and kNoSlot");

// The init error of any module, as the underlying value of that module's
// generated InitError. Error only, it has no Ok value.
enum class [[nodiscard]] InitErrorCode : std::uint8_t {};

template <class Error>
  requires std::is_enum_v<Error> &&
           std::is_same_v<std::underlying_type_t<Error>, std::uint8_t>
[[nodiscard]] constexpr InitErrorCode init_error(Error error) {
  return static_cast<InitErrorCode>(std::to_underlying(error));
}

struct Module {
  std::string_view procedure;
  std::expected<void, InitErrorCode> (*init)(void *context) = nullptr;
  // Main loop only, from the board Hooks::collect.
  void (*collect)(oslogger::Row &row, std::int64_t now_us,
                  void *context) = nullptr;
  void *context = nullptr;
};

namespace detail {
// Never defined and never constexpr. resolve() calls it only for a module
// column the board table does not carry, which ends constant evaluation, so
// the build stops with this name in the diagnostic.
void module_column_missing_from_board_fields();
// Same, for a column found at or past kNoSlot.
void module_column_index_over_slot();

template <class Field, std::size_t Extent>
consteval Slot find(std::string_view column, std::span<Field, Extent> fields) {
  std::size_t found = fields.size();
  for (std::size_t j = 0; j < fields.size(); ++j) {
    if (std::string_view{fields[j].name} == column) {
      found = j;
      break;
    }
  }
  if (found == fields.size())
    module_column_missing_from_board_fields();
  if (found >= kNoSlot)
    module_column_index_over_slot();
  return static_cast<Slot>(found);
}
} // namespace detail

// The board column index of each module column, in kColumns order. Field is
// any table row with a `const char *name`, such as the board kFields entry.
// Every column must be on the board.
template <std::size_t N, class Field, std::size_t Extent>
[[nodiscard]] consteval Slots<N>
resolve(const std::array<std::string_view, N> &columns,
        std::span<Field, Extent> fields) {
  Slots<N> out{};
  for (std::size_t i = 0; i < N; ++i)
    out[i] = detail::find(columns[i], fields);
  return out;
}

// The same for a board that selects only some of the module's segments.
// Each column of a selected segment must be on the board. Each column of
// another segment maps to kNoSlot.
template <std::size_t N, class Segment, std::size_t S, class Field,
          std::size_t Extent>
  requires std::is_enum_v<Segment>
[[nodiscard]] consteval Slots<N>
resolve(const std::array<std::string_view, N> &columns,
        const std::array<Segment, N> &column_segments,
        const std::array<Segment, S> &selected,
        std::span<Field, Extent> fields) {
  Slots<N> out{};
  for (std::size_t i = 0; i < N; ++i) {
    bool wanted = false;
    for (const Segment segment : selected)
      wanted = wanted || segment == column_segments[i];
    out[i] = wanted ? detail::find(columns[i], fields) : kNoSlot;
  }
  return out;
}

} // namespace ossensor
