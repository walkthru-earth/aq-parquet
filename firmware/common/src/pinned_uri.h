#pragma once

// Pinned repository URIs (#89). The footer dictionary URI, the template
// describedby link and the footer template href are raw GitHub URLs at a full
// 40 hex commit, so the bytes at the URL are the bytes the digest names and a
// moving ref such as main can never stand in. These consteval checks cover the
// form a build can see. tools/pinned_uris.py proves with git that the commit
// is on main and holds bytes with the right SHA-256.

#include <cstddef>
#include <string_view>

namespace opensensor::pinned {

inline constexpr std::string_view kRawPrefix =
    "https://raw.githubusercontent.com/walkthru-earth/aq-parquet/";
inline constexpr std::size_t kCommitHexDigits = 40;

// The repository path after the commit, or an empty view when uri is not the
// raw prefix, a lower case 40 hex commit and a slash.
consteval std::string_view path_of(std::string_view uri) {
  if (!uri.starts_with(kRawPrefix))
    return {};
  const std::string_view rest = uri.substr(kRawPrefix.size());
  if (rest.size() <= kCommitHexDigits || rest[kCommitHexDigits] != '/')
    return {};
  for (std::size_t i = 0; i < kCommitHexDigits; ++i) {
    const char c = rest[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return {};
  }
  return rest.substr(kCommitHexDigits + 1);
}

// uri names exactly path at a pinned commit.
consteval bool names(std::string_view uri, std::string_view path) {
  return !path.empty() && path_of(uri) == path;
}

// uri names the published template of dict, under
// docs/shared/templates/<board>/<dict>.json.
consteval bool names_template(std::string_view uri, std::string_view board,
                              std::string_view dict) {
  constexpr std::string_view kTemplates = "docs/shared/templates/";
  std::string_view path = path_of(uri);
  if (!path.starts_with(kTemplates))
    return false;
  path.remove_prefix(kTemplates.size());
  if (!path.starts_with(board) || path.size() <= board.size() ||
      path[board.size()] != '/')
    return false;
  path.remove_prefix(board.size() + 1);
  return path.size() == dict.size() + 5 && path.starts_with(dict) &&
         path.ends_with(".json");
}

} // namespace opensensor::pinned
