// Host test for the column semantics alignment check (#84). The board tables
// are checked at compile time here, the same way telemetry_contract.h checks
// them in the firmware. Copies with one defect each must fail the check.

#include "../firmware/common/src/column_semantics.h"

#include <array>
#include <cstdint>
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

constexpr const aq::semantics::Column &
row(std::span<const aq::semantics::Column> rows, std::string_view name) {
  for (const auto &r : rows)
    if (r.name == name)
      return r;
  return rows.front();
}
constexpr bool has(std::span<const aq::semantics::Column> rows,
                   std::string_view name) {
  for (const auto &r : rows)
    if (r.name == name)
      return true;
  return false;
}

// Every column both boards write has the same property id on both, since a
// property names the quantity, never the board.
constexpr bool shared_columns_agree() {
  for (const auto &r : kWaveshareSemantics)
    if (has(kCores3Semantics, r.name) &&
        row(kCores3Semantics, r.name).property != r.property)
      return false;
  return true;
}
static_assert(shared_columns_agree());

// stac-opensensor templates/cores3-telemetry-v5.json at a017edf. Every value
// the template assigns is copied here verbatim, so a drift on either side
// fails this test. One value differs on purpose. imu_temperature_c uses the
// BMI270 datasheet -40 where the template example says -20.
struct TemplateColumn {
  std::string_view name, unit, property, valid_when, min, max;
  aq::semantics::Role role;
  aq::semantics::Aggregate aggregate;
  std::uint32_t settle_s;
  bool has_aggregate = true;
};
using enum aq::semantics::Role;
using enum aq::semantics::Aggregate;
constexpr TemplateColumn kTemplate[] = {
    {"event_time_utc_us", "us", "", "", "", "", coordinate, point, 0, false},
    {"pm1_atmospheric_ug_m3", "ug/m3",
     "http://vocab.nerc.ac.uk/standard_name/"
     "mass_concentration_of_pm1_ambient_aerosol_particles_in_air/",
     "pms_status == 4", "0", "1000", physicalMeasurement, mean, 180},
    {"pm25_atmospheric_ug_m3", "ug/m3",
     "http://vocab.nerc.ac.uk/collection/P07/current/YUDG9DXK/",
     "pms_status == 4", "0", "1000", physicalMeasurement, mean, 180},
    {"pm10_atmospheric_ug_m3", "ug/m3",
     "http://vocab.nerc.ac.uk/standard_name/"
     "mass_concentration_of_pm10_ambient_aerosol_particles_in_air/",
     "pms_status == 4", "0", "1000", physicalMeasurement, mean, 180},
    {"pm25_cf1_ug_m3", "ug/m3",
     "urn:walkthru-earth:property:pm25_cf1_mass_concentration",
     "pms_status == 4", "0", "1000", physicalMeasurement, mean, 180},
    {"particles_gt03_per_01l", "{particles}/dL",
     "urn:walkthru-earth:property:particle_count_gt_0p3um", "pms_status == 4",
     "0", "65535", physicalMeasurement, mean, 180},
    {"imu_temperature_c", "Cel", "", "finite", "-40", "85",
     auxiliaryInformation, mean, 0},
    {"accel_z_g", "[g]", "", "finite", "", "", auxiliaryInformation, mean, 0},
    {"battery_percent", "%", "", "", "0", "100", auxiliaryInformation, point,
     0},
};
// The template leaves property out on four rows. The FIELD table needs one
// on every column, so those rows keep their own board neutral id.
constexpr bool matches_template() {
  for (const auto &t : kTemplate) {
    if (!has(kCores3Semantics, t.name))
      return false;
    const auto &r = row(kCores3Semantics, t.name);
    if (r.ucum != t.unit || r.role != t.role ||
        (t.has_aggregate && r.aggregate != t.aggregate) ||
        r.valid_when != t.valid_when || r.plausible_min != t.min ||
        r.plausible_max != t.max || r.settle_s != t.settle_s)
      return false;
    if (!t.property.empty() && r.property != t.property)
      return false;
  }
  return true;
}
static_assert(matches_template());

// opensensor-space-mobile core/history Metrics.kt at 7f70fb72, one check per
// copied value with its source line. Kotlin units are display strings, so
// the UCUM code of the same unit is checked (µg/m³ is ug/m3, per 0.1 L is
// {particles}/dL, °C is Cel). A Kotlin null gate means every non-null value
// counts, and valueOf drops a non-finite value (Metrics.kt:69). That is
// "finite" here, except battery and vbus which stay "" as in the template.
// Kotlin has no ranges and no aggregates, so those are new values.
struct KotlinValue {
  std::string_view column, ucum, valid_when;
  std::uint32_t settle_s;
  int line;
};
constexpr KotlinValue kKotlin[] = {
    {"pm25_cf1_ug_m3", "ug/m3", "pms_status == 4", 180, 97},
    {"pm25_atmospheric_ug_m3", "ug/m3", "pms_status == 4", 180, 103},
    {"pm10_atmospheric_ug_m3", "ug/m3", "pms_status == 4", 180, 107},
    {"pm1_atmospheric_ug_m3", "ug/m3", "pms_status == 4", 180, 108},
    {"particles_gt03_per_01l", "{particles}/dL", "pms_status == 4", 180, 109},
    {"particles_gt25_per_01l", "{particles}/dL", "pms_status == 4", 180, 110},
    {"imu_temperature_c", "Cel", "finite", 0, 111},
    {"ambient_temperature_c", "Cel", "finite", 0, 114},
    {"relative_humidity_percent", "%", "finite", 0, 115},
    {"battery_percent", "%", "", 0, 116},
    {"vbus_mv", "mV", "", 0, 117},
};
// Waveshare only, Metrics.kt:106, gate Gate("pm25_batch_candidate_status", 3).
constexpr KotlinValue kKotlinWaveshare[] = {
    {"pm25_batch_candidate_ug_m3", "ug/m3", "pm25_batch_candidate_status == 3",
     180, 106},
};
template <std::size_t N>
constexpr int kotlin_mismatch(std::span<const aq::semantics::Column> rows,
                              const KotlinValue (&values)[N]) {
  for (const auto &k : values) {
    if (!has(rows, k.column))
      continue;
    const auto &r = row(rows, k.column);
    if (r.ucum != k.ucum || r.valid_when != k.valid_when ||
        r.settle_s != k.settle_s)
      return k.line;
  }
  return 0;
}
static_assert(kotlin_mismatch(kCores3Semantics, kKotlin) == 0);
static_assert(kotlin_mismatch(kWaveshareSemantics, kKotlin) == 0);
static_assert(kotlin_mismatch(kWaveshareSemantics, kKotlinWaveshare) == 0);
// PMS_VALID is "shared by every particle metric" (Metrics.kt:86), and
// PMS_SETTLE_S is 180 (Metrics.kt:90), so the other bins and CF=1 columns
// carry the same gate and settle.
constexpr bool
particle_rows_follow_pms(std::span<const aq::semantics::Column> rows) {
  for (const auto &r : rows)
    if (r.ucum == "ug/m3" || r.ucum == "{particles}/dL")
      if (r.role == physicalMeasurement &&
          (r.valid_when != "pms_status == 4" || r.settle_s != 180))
        return false;
  return true;
}
static_assert(particle_rows_follow_pms(kCores3Semantics));
static_assert(particle_rows_follow_pms(kWaveshareSemantics));
static_assert(row(kWaveshareSemantics, "pm25_batch_candidate_ug_m3").role ==
              modelResult);

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
    broken[12].property = "urn:walkthru-earth:cores3:property:pm1_cf1_ug_m3";
    expect(!well_formed(fields, std::span{broken}),
           "board scoped property is caught");
  }
  {
    auto broken = copy(rows);
    broken[12].property = "urn:walkthru-earth:property:pms_pm1";
    expect(!well_formed(fields, std::span{broken}),
           "sensor name in a property is caught");
  }
  for (const char *variant :
       {"http://vocab.nerc.ac.uk/collection/P07/current/",
        "http://vocab.nerc.ac.uk/collection/P07/current//",
        "http://vocab.nerc.ac.uk/standard_name/",
        "http://vocab.nerc.ac.uk/standard_name//"}) {
    auto broken = copy(rows);
    broken[12].property = variant;
    expect(!well_formed(fields, std::span{broken}),
           "a NERC prefix without a term is caught");
  }
  for (const char *variant :
       {"urn:walkthru-earth:property:m5_bmi_270_temperature",
        "urn:walkthru-earth:property:ltr_553_light",
        "urn:walkthru-earth:property:axp_battery"}) {
    auto broken = copy(rows);
    broken[12].property = variant;
    expect(!well_formed(fields, std::span{broken}),
           "chip or vendor spelling in a property is caught");
  }
  {
    auto broken = copy(rows);
    broken[12].property = "";
    expect(!well_formed(fields, std::span{broken}), "empty property is caught");
  }
  {
    auto broken = copy(rows);
    broken[12].property = broken[13].property;
    expect(!well_formed(fields, std::span{broken}),
           "duplicate property is caught");
  }
  {
    auto broken = copy(rows);
    broken[12].property = "http://vocab.nerc.ac.uk/collection/P07/current/x/";
    expect(!well_formed(fields, std::span{broken}),
           "malformed P07 term is caught");
  }
  {
    auto broken = copy(rows);
    broken[12].property = "urn:walkthru-earth:property:PM1";
    expect(!well_formed(fields, std::span{broken}),
           "non snake case quantity is caught");
  }
  expect(aq::semantics::property_of(std::span{rows}, rows[12].name) ==
             rows[12].property.data(),
         "property_of returns the row property");
  expect(aq::semantics::property_of(std::span{rows}, "not_a_column") == nullptr,
         "property_of returns nullptr for an unknown column");
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
