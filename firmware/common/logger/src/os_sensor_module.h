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
#include <iterator>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ossensor {

// One board column index. The writer caps a board at kMaxColumns, so a byte
// holds every index and leaves kNoSlot free.
using Slot = std::uint8_t;
template <std::size_t N> using Slots = std::array<Slot, N>;
// A column of a segment the board does not select. The write helpers below
// skip it.
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

// Runs one Module::init. A call through the function pointer drops the
// [[nodiscard]] of the result, so board code calls this instead and a
// dropped result warns again. A module with no init succeeds.
[[nodiscard]] inline std::expected<void, InitErrorCode>
init_module(const Module &module) {
  if (module.init == nullptr)
    return {};
  return module.init(module.context);
}

// Runtime slot writes for module code. Each skips kNoSlot and any slot past
// the row, so a column of a segment the board does not select stays null and
// nothing outside the row is touched. Prefer Writer, which also checks the
// column type at build time.
inline void integer(oslogger::Row &row, Slot slot, std::int32_t value) {
  if (slot < row.data.size())
    row.integer(slot, value);
}
inline void counter(oslogger::Row &row, Slot slot, std::int64_t value) {
  if (slot < row.data.size())
    row.counter(slot, value);
}
inline void number(oslogger::Row &row, Slot slot, float value) {
  if (slot < row.data.size())
    row.number(slot, value);
}

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

// Never defined and never constexpr, like the guards above. Writer calls it
// when a module writes a column with a call of another physical type than
// the board field, so the build stops with this name in the diagnostic.
void module_column_type_differs_from_board_field();

template <class ModuleColumn, const auto &Fields, const auto &Slots,
          auto Column>
consteval Slot typed_slot(telemetry::PhysicalType type) {
  static_assert(std::is_same_v<decltype(Column), ModuleColumn>,
                "the Column belongs to another module than this Writer");
  static_assert(std::to_underlying(Column) < std::size(Slots),
                "the Column is past the slots of this Writer");
  const Slot slot = Slots[std::to_underlying(Column)];
  if (slot != kNoSlot &&
      (slot >= std::size(Fields) || Fields[slot].type != type))
    module_column_type_differs_from_board_field();
  return slot;
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

// Typed writes of one module. ModuleColumn is that module's generated
// Column enum, Fields the board table (kFields) and Slots that module's
// resolve() result, both static constexpr. A write names a Column. The build
// stops when the Column is of another module, and when the board field has
// another type than the call. integer writes Int32, counter Int64 and number
// Float, as the manifest firmware.type says. A kNoSlot column compiles to no
// write.
//
//   static constexpr auto kSlots = ossensor::resolve(pms5003t::kColumns,
//                                                    std::span{kFields});
//   const ossensor::Writer<pms5003t::Column, kFields, kSlots> out{row};
//   out.integer<pms5003t::Column::pms_status>(status);
template <class ModuleColumn, const auto &Fields, const auto &Slots>
  requires std::is_enum_v<ModuleColumn>
class Writer {
  static_assert(std::size(Fields) <=
                    std::tuple_size_v<decltype(oslogger::Row::data)>,
                "the board table fits in a row");

public:
  constexpr explicit Writer(oslogger::Row &row) : row_(row) {}

  template <auto Column> void integer(std::int32_t value) const {
    constexpr Slot slot =
        detail::typed_slot<ModuleColumn, Fields, Slots, Column>(
            telemetry::PhysicalType::Int32);
    if constexpr (slot != kNoSlot)
      row_.integer(slot, value);
  }
  template <auto Column> void counter(std::int64_t value) const {
    constexpr Slot slot =
        detail::typed_slot<ModuleColumn, Fields, Slots, Column>(
            telemetry::PhysicalType::Int64);
    if constexpr (slot != kNoSlot)
      row_.counter(slot, value);
  }
  template <auto Column> void number(float value) const {
    constexpr Slot slot =
        detail::typed_slot<ModuleColumn, Fields, Slots, Column>(
            telemetry::PhysicalType::Float);
    if constexpr (slot != kNoSlot)
      row_.number(slot, value);
  }

private:
  oslogger::Row &row_;
};

} // namespace ossensor
