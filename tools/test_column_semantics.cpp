// Host test for the column semantics alignment check (#84). The board tables
// are checked at compile time here, the same way telemetry_contract.h checks
// them in the firmware. Copies with one defect each must fail the check.

#include "../firmware/common/src/column_semantics.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <utility>

namespace {

struct Name {
  const char *name;
};

constexpr Name kCores3Fields[] = {
#define FIELD(name, type, procedure, unit, validity, live, decimals) {#name},
#include "../firmware/esp-idf-cores3/bringup/telemetry_fields.inc"
#undef FIELD
};
constexpr opensensor::semantics::Column kCores3Semantics[] = {
#define SEM(name, ucum, role, cf_standard_name, aggregate, plausible_min,      \
            plausible_max, settle_s, valid_when, property, sensor_vendor,      \
            sensor_model)                                                      \
  {#name,                                                                      \
   ucum,                                                                       \
   opensensor::semantics::Role::role,                                          \
   cf_standard_name,                                                           \
   opensensor::semantics::Aggregate::aggregate,                                \
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
#define FIELD(name, type, procedure, unit, validity, live, decimals) {#name},
#include "../firmware/esp-idf-waveshare-sim7670g/logger/telemetry_fields.inc"
#undef FIELD
};
constexpr opensensor::semantics::Column kWaveshareSemantics[] = {
#define SEM(name, ucum, role, cf_standard_name, aggregate, plausible_min,      \
            plausible_max, settle_s, valid_when, property, sensor_vendor,      \
            sensor_model)                                                      \
  {#name,                                                                      \
   ucum,                                                                       \
   opensensor::semantics::Role::role,                                          \
   cf_standard_name,                                                           \
   opensensor::semantics::Aggregate::aggregate,                                \
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
static_assert(opensensor::semantics::aligned(kCores3Fields, kCores3Semantics));
static_assert(opensensor::semantics::aligned(kWaveshareFields,
                                             kWaveshareSemantics));
static_assert(opensensor::semantics::well_formed(kCores3Fields,
                                                 kCores3Semantics));
static_assert(opensensor::semantics::well_formed(kWaveshareFields,
                                                 kWaveshareSemantics));

constexpr const opensensor::semantics::Column &
row(std::span<const opensensor::semantics::Column> rows,
    std::string_view name) {
  for (const auto &r : rows)
    if (r.name == name)
      return r;
  return rows.front();
}
constexpr bool has(std::span<const opensensor::semantics::Column> rows,
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
  opensensor::semantics::Role role;
  opensensor::semantics::Aggregate aggregate;
  std::uint32_t settle_s;
  bool has_aggregate = true;
};
using enum opensensor::semantics::Role;
using enum opensensor::semantics::Aggregate;
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
constexpr int
kotlin_mismatch(std::span<const opensensor::semantics::Column> rows,
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
particle_rows_follow_pms(std::span<const opensensor::semantics::Column> rows) {
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
std::array<opensensor::semantics::Column, N>
copy(const opensensor::semantics::Column (&rows)[N]) {
  std::array<opensensor::semantics::Column, N> out{};
  for (std::size_t i = 0; i < N; ++i)
    out[i] = rows[i];
  return out;
}

template <typename Fields, std::size_t N>
void check_board(const char *board, const Fields &fields,
                 const opensensor::semantics::Column (&rows)[N]) {
  using opensensor::semantics::aligned;
  using opensensor::semantics::well_formed;
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
  for (std::size_t i = 0; i < N; ++i) {
    if (rows[i].name != "geometry")
      continue;
    auto broken = copy(rows);
    broken[i].ucum = "deg";
    expect(!well_formed(fields, std::span{broken}),
           "a unit on the GEOMETRY column is caught");
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
  expect(opensensor::semantics::property_of(std::span{rows}, rows[12].name) ==
             rows[12].property.data(),
         "property_of returns the row property");
  expect(opensensor::semantics::property_of(std::span{rows}, "not_a_column") ==
             nullptr,
         "property_of returns nullptr for an unknown column");
  {
    auto broken = copy(rows);
    broken[10].sensor_model = "";
    expect(!well_formed(fields, std::span{broken}),
           "vendor without model is caught");
  }
}

// Classes and derived checks (#85) on a small table, since the board
// telemetry_classes.inc rows name board enums this test does not include.
struct Procedure {
  const char *name;
  const char *procedure;
};
constexpr Procedure kMiniFields[] = {{"status", "sensor"},
                                     {"flag", "sensor"},
                                     {"mask", "sensor"},
                                     {"count", "sensor"},
                                     {"model", "fit_v1"}};
using opensensor::semantics::Aggregate;
using opensensor::semantics::Role;
constexpr opensensor::semantics::Column kMini[] = {
    {"status", "{code}", Role::qualityInformation, "", Aggregate::mode, "", "",
     0, "", "urn:walkthru-earth:property:status", "", ""},
    {"flag", "{boolean}", Role::auxiliaryInformation, "", Aggregate::mode, "",
     "", 0, "", "urn:walkthru-earth:property:flag", "", ""},
    {"mask", "{bitmask}", Role::qualityInformation, "", Aggregate::mode, "", "",
     0, "", "urn:walkthru-earth:property:mask", "", ""},
    {"count", "{count}", Role::physicalMeasurement, "", Aggregate::mean, "", "",
     0, "finite", "urn:walkthru-earth:property:count", "", ""},
    {"model", "ug/m3", Role::modelResult, "", Aggregate::mean, "", "", 0,
     "finite", "urn:walkthru-earth:property:model", "", ""},
};
constexpr opensensor::semantics::Class kMiniClasses[] = {
    {"status", 0, "missing", true},
    {"status", 1, "valid", false},
    {"flag", 0, "off", false},
    {"flag", 1, "on", false},
};
constexpr std::string_view kMiniClassless[] = {"mask"};
constexpr std::string_view kMiniSources[] = {"count"};
constexpr opensensor::semantics::Derived kMiniDerived[] = {
    {"model", kMiniSources, "urn:walkthru-earth:method:fit_v1"}};

void check_classes_and_derived() {
  using opensensor::semantics::classes_complete;
  using opensensor::semantics::derived_complete;
  using opensensor::semantics::matches_legend;
  const std::span<const opensensor::semantics::Column> rows{kMini};
  expect(classes_complete(rows, kMiniClasses, kMiniClassless),
         "real classes complete");
  const auto classes = [] {
    std::array<opensensor::semantics::Class, std::size(kMiniClasses)> out{};
    for (std::size_t i = 0; i < out.size(); ++i)
      out[i] = kMiniClasses[i];
    return out;
  };
  expect(!classes_complete(rows, std::span{kMiniClasses}.subspan(2),
                           kMiniClassless),
         "code column without classes is caught");
  expect(!classes_complete(rows, kMiniClasses, {}),
         "bitmask column neither classed nor listed is caught");
  {
    constexpr std::string_view listed[] = {"mask", "status"};
    expect(!classes_complete(rows, kMiniClasses, listed),
           "listed column that has classes is caught");
  }
  {
    constexpr std::string_view listed[] = {"mask", "gone"};
    expect(!classes_complete(rows, kMiniClasses, listed),
           "listed column that does not exist is caught");
  }
  {
    auto broken = classes();
    broken[1].value = 0;
    expect(!classes_complete(rows, broken, kMiniClassless),
           "repeated value is caught");
  }
  {
    auto broken = classes();
    broken[1].name = "missing";
    expect(!classes_complete(rows, broken, kMiniClassless),
           "repeated name is caught");
  }
  {
    auto broken = classes();
    broken[1].nodata = true;
    expect(!classes_complete(rows, broken, kMiniClassless),
           "two nodata values are caught");
  }
  {
    auto broken = classes();
    broken[3].value = 2;
    expect(!classes_complete(rows, broken, kMiniClassless),
           "boolean value outside 0 and 1 is caught");
  }
  expect(
      !classes_complete(rows, std::span{kMiniClasses}.first(3), kMiniClassless),
      "boolean column with only its 0 row is caught");
  {
    const opensensor::semantics::Class one_flag[] = {
        kMiniClasses[0], kMiniClasses[1], kMiniClasses[3]};
    expect(!classes_complete(rows, one_flag, kMiniClassless),
           "boolean column with only its 1 row is caught");
  }
  {
    auto broken = classes();
    broken[2].column = "count";
    expect(!classes_complete(rows, broken, kMiniClassless),
           "class on a quantity column is caught");
  }
  {
    auto broken = classes();
    broken[0].name = "no spaces";
    expect(!classes_complete(rows, broken, kMiniClassless),
           "class name outside the pattern is caught");
  }
  {
    auto broken = classes();
    std::swap(broken[1], broken[2]);
    expect(!classes_complete(rows, broken, kMiniClassless),
           "split column is caught");
  }
  expect(matches_legend(kMiniClasses, "status", "0=missing,1=valid"),
         "legend matches");
  expect(matches_legend(kMiniClasses, "status", "0=missing,1=valid; note"),
         "legend free text after ; is ignored");
  expect(!matches_legend(kMiniClasses, "status", "0=missing,1=ok"),
         "renamed legend entry is caught");
  expect(!matches_legend(kMiniClasses, "status", "0=missing,2=valid"),
         "renumbered legend entry is caught");
  expect(!matches_legend(kMiniClasses, "status", "0=missing,1=valid,2=x"),
         "extra legend entry is caught");
  expect(!matches_legend(kMiniClasses, "status", "0=missing"),
         "missing legend entry is caught");
  expect(!matches_legend(kMiniClasses, "mask", "0=none"),
         "legend for a column without classes is caught");
  expect(!matches_legend(std::span{kMiniClasses}.first(1), "status",
                         "0=missing,1=valid"),
         "class row missing from a legend column is caught");
  {
    using opensensor::semantics::matches_codes;
    const auto name_of = [](std::int64_t code) {
      return code == 0 ? "missing" : code == 1 ? "valid" : "other";
    };
    static constexpr std::int64_t codes[] = {0, 1};
    static constexpr std::int64_t more[] = {0, 1, 2};
    static constexpr std::int64_t fewer[] = {0};
    static constexpr std::int64_t skipped[] = {0, 2};
    expect(matches_codes(kMiniClasses, "status", codes, name_of),
           "codes match");
    expect(!matches_codes(kMiniClasses, "status", more, name_of),
           "code with no class row is caught");
    expect(!matches_codes(kMiniClasses, "status", fewer, name_of),
           "class row for a code never written is caught");
    expect(!matches_codes(kMiniClasses, "status", skipped, name_of),
           "class row with another value is caught");
    expect(!matches_codes(kMiniClasses, "status", skipped,
                          [](std::int64_t code) {
                            return code == 0 ? "missing" : "valid";
                          }),
           "class row with another value and the same name is caught");
    expect(!matches_codes(kMiniClasses, "status", codes,
                          [](std::int64_t) { return "valid"; }),
           "class name other than name_of is caught");
    expect(!matches_codes(kMiniClasses, "mask", fewer, name_of),
           "codes for a column without classes are caught");
  }

  expect(derived_complete(kMiniFields, rows, kMiniDerived),
         "real derived complete");
  expect(!derived_complete(kMiniFields, rows, {}),
         "modelResult without derived is caught");
  {
    constexpr opensensor::semantics::Derived twice[] = {kMiniDerived[0],
                                                        kMiniDerived[0]};
    expect(!derived_complete(kMiniFields, rows, twice),
           "two derived entries for one column are caught");
  }
  {
    constexpr opensensor::semantics::Derived wrong[] = {
        {"count", kMiniSources, "urn:walkthru-earth:method:sensor"}};
    expect(!derived_complete(kMiniFields, rows, wrong),
           "derived on a measurement is caught");
  }
  {
    static constexpr std::string_view sources[] = {"gone"};
    constexpr opensensor::semantics::Derived wrong[] = {
        {"model", sources, "urn:walkthru-earth:method:fit_v1"}};
    expect(!derived_complete(kMiniFields, rows, wrong),
           "unknown source column is caught");
  }
  {
    static constexpr std::string_view sources[] = {"model"};
    constexpr opensensor::semantics::Derived wrong[] = {
        {"model", sources, "urn:walkthru-earth:method:fit_v1"}};
    expect(!derived_complete(kMiniFields, rows, wrong),
           "column derived from itself is caught");
  }
  {
    constexpr opensensor::semantics::Derived wrong[] = {
        {"model", {}, "urn:walkthru-earth:method:fit_v1"}};
    expect(!derived_complete(kMiniFields, rows, wrong),
           "empty source list is caught");
  }
  {
    constexpr opensensor::semantics::Derived wrong[] = {
        {"model", kMiniSources, "urn:walkthru-earth:method:fit_v2"}};
    expect(!derived_complete(kMiniFields, rows, wrong),
           "method that is not the FIELD procedure is caught");
  }
}

} // namespace

int main() {
  check_classes_and_derived();
  expect(opensensor::semantics::is_decimal("-12.5"), "decimal with sign");
  expect(!opensensor::semantics::is_decimal("1."), "trailing point rejected");
  expect(!opensensor::semantics::is_decimal(""), "empty rejected");
  expect(opensensor::semantics::decimal_value("-12.5") == -12.5,
         "decimal value");
  check_board("cores3", kCores3Fields, kCores3Semantics);
  check_board("waveshare-sim7670g", kWaveshareFields, kWaveshareSemantics);
  if (failures != 0)
    return 1;
  std::puts("PASS column semantics align with both FIELD tables, classes and "
            "derived checks catch each defect");
  return 0;
}
