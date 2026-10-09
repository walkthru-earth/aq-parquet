#pragma once

#include <cstddef>
#include <cstdint>

// Stateless H3 helpers. Persist only the result of coarsen(); derive public
// coordinates from that cell. A cell center is not the original GPS position
// or an administrative country lookup.
namespace aq::location {
using Cell = std::uint64_t;
constexpr int kDefaultResolution = 5;
constexpr std::size_t kCellTextBytes = 16; // 15 lowercase hex digits + NUL

bool valid_cell(Cell cell);
int resolution(Cell cell); // -1 for an invalid cell, including absent cell 0
bool parse_cell(const char *text, Cell &cell);
bool format_cell(Cell cell, char *text, std::size_t size);
// Maximum disclosure resolution, inclusive 0..15. A coarser input is retained
// as-is; increasing this limit cannot reconstruct finer location information.
bool coarsen(Cell input, int maximum_resolution, Cell &published);
bool center_degrees(Cell cell, double &latitude, double &longitude);
bool center_e7(Cell cell, std::int64_t &latitude, std::int64_t &longitude);
bool from_degrees(double latitude, double longitude, int resolution,
                  Cell &cell);
// Assigned ISO 3166-1 alpha-2 code only; country is explicitly owner-declared.
// Empty input means missing to callers, and is not itself a valid country code.
bool valid_country(const char *country);
} // namespace aq::location
