// Host test for the column semantics alignment check (#84). The board tables
// are checked at compile time here, the same way telemetry_contract.h checks
// them in the firmware. Copies with one defect each must fail the check.

#include "../firmware/common/src/column_semantics.h"

#include <array>
#include <cstdio>
#include <string_view>
#include <utility>

namespace {

struct Name {
  const char *name;
};

constexpr Name kCores3Fields[] = {
#define FIELD(name, type, procedure, unit, validity, live) {#name},
#include "../firmware/esp-idf-cores3/bringup/telemetry_fields.inc"
#undef FIELD
};
constexpr aq::semantics::Column kCores3Semantics[] = {
#define SEM(name, ucum, role, cf_standard_name, aggregate, plausible_min,      \
            plausible_max, settle_s, valid_when, property, sensor_vendor,      \
            sensor_model)                                                      \
  {#name,                                                                      \
   ucum,                                                                       \
   aq::semantics::Role::role,                                                  \
   cf_standard_name,                                                           \
   aq::semantics::Aggregate::aggregate,                                        \
   #plausible_min,                                                             \
   #plausible_max,                                                             \
   settle_s,                                                                   \
   valid_when,                                                                 \
   property,                                                                   \
   sensor_vendor,                                                              \
   sensor_model},
#include "../firmware/esp-idf-cores3/bringup/telemetry_semantics.inc"
#undef SEM
};

constexpr Name kWaveshareFields[] = {
#define FIELD(name, type, procedure, unit, validity, live) {#name},
#include "../firmware/esp-idf-waveshare-sim7670g/logger/telemetry_fields.inc"
#undef FIELD
};
constexpr aq::semantics::Column kWaveshareSemantics[] = {
#define SEM(name, ucum, role, cf_standard_name, aggregate, plausible_min,      \
            plausible_max, settle_s, valid_when, property, sensor_vendor,      \
            sensor_model)                                                      \
  {#name,                                                                      \
   ucum,                                                                       \
   aq::semantics::Role::role,                                                  \
   cf_standard_name,                                                           \
   aq::semantics::Aggregate::aggregate,                                        \
   #plausible_min,                                                             \
   #plausible_max,                                                             \
   settle_s,                                                                   \
   valid_when,                                                                 \
   property,                                                                   \
   sensor_vendor,                                                              \
   sensor_model},
#include "../firmware/esp-idf-waveshare-sim7670g/logger/telemetry_semantics.inc"
#undef SEM
};

static_assert(std::size(kCores3Semantics) == 81);
static_assert(std::size(kWaveshareSemantics) == 62);
static_assert(aq::semantics::aligned(kCores3Fields, kCores3Semantics));
static_assert(aq::semantics::aligned(kWaveshareFields, kWaveshareSemantics));
static_assert(aq::semantics::well_formed(kCores3Fields, kCores3Semantics));
static_assert(aq::semantics::well_formed(kWaveshareFields,
                                         kWaveshareSemantics));

// Spot checks against values copied from the mobile Metrics.kt and the
// stac-opensensor example template.
constexpr const aq::semantics::Column &
row(std::span<const aq::semantics::Column> rows, std::string_view name) {
  for (const auto &r : rows)
    if (r.name == name)
      return r;
  return rows.front();
}
static_assert(row(kCores3Semantics, "pm25_atmospheric_ug_m3").settle_s == 180);
static_assert(row(kCores3Semantics, "pm25_atmospheric_ug_m3").valid_when ==
              "pms_status == 4");
static_assert(row(kCores3Semantics, "imu_temperature_c").plausible_min ==
              "-20");
static_assert(row(kCores3Semantics, "imu_temperature_c").role ==
              aq::semantics::Role::auxiliaryInformation);
static_assert(row(kWaveshareSemantics, "pm25_batch_candidate_ug_m3").role ==
              aq::semantics::Role::modelResult);
static_assert(row(kWaveshareSemantics, "pm25_batch_candidate_ug_m3")
                  .valid_when == "pm25_batch_candidate_status == 3");
static_assert(row(kWaveshareSemantics, "ambient_temperature_c").settle_s == 0);

int failures = 0;

void expect(bool condition, const char *what) {
  if (condition)
    return;
  std::fprintf(stderr, "FAIL %s\n", what);
  ++failures;
}

template <std::size_t N>
std::array<aq::semantics::Column, N>
copy(const aq::semantics::Column (&rows)[N]) {
  std::array<aq::semantics::Column, N> out{};
  for (std::size_t i = 0; i < N; ++i)
    out[i] = rows[i];
  return out;
}

template <typename Fields, std::size_t N>
void check_board(const char *board, const Fields &fields,
                 const aq::semantics::Column (&rows)[N]) {
  using aq::semantics::aligned;
  using aq::semantics::well_formed;
  std::printf("board %s, %zu rows\n", board, N);
  expect(aligned(fields, std::span{rows}), "real table aligned");
  expect(well_formed(fields, std::span{rows}), "real table well formed");

  {
    const auto dropped = std::span{rows}.first(N - 1);
    expect(!aligned(fields, dropped), "dropped last row is caught");
  }
  {
    auto swapped = copy(rows);
    std::swap(swapped[3], swapped[4]);
    expect(!aligned(fields, std::span{swapped}), "swapped rows are caught");
  }
  {
    auto renamed = copy(rows);
    renamed[N / 2].name = "not_a_column";
    expect(!aligned(fields, std::span{renamed}), "renamed row is caught");
  }
  {
    auto broken = copy(rows);
    broken[12].valid_when = "missing_column == 4";
    expect(!well_formed(fields, std::span{broken}),
           "gate on an unknown column is caught");
  }
  {
    auto broken = copy(rows);
    broken[12].valid_when = "pms_status == 4.5";
    expect(!well_formed(fields, std::span{broken}),
           "non integer gate value is caught");
  }
  {
    auto broken = copy(rows);
    broken[12].valid_when = "";
    expect(!well_formed(fields, std::span{broken}),
           "physical measurement without a gate is caught");
  }
  {
    auto broken = copy(rows);
    broken[12].plausible_min = "1000";
    expect(!well_formed(fields, std::span{broken}),
           "min not below max is caught");
  }
  {
    auto broken = copy(rows);
    broken[12].plausible_max = "";
    expect(!well_formed(fields, std::span{broken}), "half a range is caught");
  }
  {
    auto broken = copy(rows);
    broken[0].ucum = "";
    expect(!well_formed(fields, std::span{broken}), "empty UCUM is caught");
  }
  {
    auto broken = copy(rows);
    broken[10].sensor_model = "";
    expect(!well_formed(fields, std::span{broken}),
           "vendor without model is caught");
  }
}

} // namespace

int main() {
  expect(aq::semantics::is_decimal("-12.5"), "decimal with sign");
  expect(!aq::semantics::is_decimal("1."), "trailing point rejected");
  expect(!aq::semantics::is_decimal(""), "empty rejected");
  expect(aq::semantics::decimal_value("-12.5") == -12.5, "decimal value");
  check_board("cores3", kCores3Fields, kCores3Semantics);
  check_board("waveshare-sim7670g", kWaveshareFields, kWaveshareSemantics);
  if (failures != 0)
    return 1;
  std::puts("PASS column semantics align with both FIELD tables");
  return 0;
}
