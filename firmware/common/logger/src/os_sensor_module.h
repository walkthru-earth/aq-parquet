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
// holds every index.
using Slot = std::uint8_t;
static_assert(telemetry::kMaxColumns <= 256, "a Slot holds every column index");
template <std::size_t N> using Slots = std::array<Slot, N>;

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
// Same, for a column found past the last index a Slot holds.
void module_column_index_over_slot();
} // namespace detail

// The board column index of each module column, in kColumns order. Field is
// any table row with a `const char *name`, such as the board kFields entry.
template <std::size_t N, class Field, std::size_t Extent>
[[nodiscard]] consteval Slots<N>
resolve(const std::array<std::string_view, N> &columns,
        std::span<Field, Extent> fields) {
  static_assert(Extent == std::dynamic_extent || Extent <= 256,
                "a Slot holds every column index");
  Slots<N> out{};
  for (std::size_t i = 0; i < N; ++i) {
    std::size_t found = fields.size();
    for (std::size_t j = 0; j < fields.size(); ++j) {
      if (std::string_view{fields[j].name} == columns[i]) {
        found = j;
        break;
      }
    }
    if (found == fields.size())
      detail::module_column_missing_from_board_fields();
    if (found > 255)
      detail::module_column_index_over_slot();
    out[i] = static_cast<Slot>(found);
  }
  return out;
}

} // namespace ossensor
