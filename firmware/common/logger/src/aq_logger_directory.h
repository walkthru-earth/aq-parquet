#pragma once

#include <cerrno>
#include <sys/stat.h>

namespace aq::logger::detail {

using DirectoryStat = int (*)(const char *, struct stat *);
using DirectoryMkdir = int (*)(const char *, mode_t);

// The caller owns filesystem/bus serialization. FatFs rejects mkdir at its
// mounted root with EINVAL rather than EEXIST; inspect existing paths first.
inline bool ensure_directory(const char *path, DirectoryStat inspect = ::stat,
                             DirectoryMkdir create = ::mkdir) {
  struct stat info{};
  if (inspect(path, &info) == 0) {
    if (S_ISDIR(info.st_mode))
      return true;
    errno = ENOTDIR;
    return false;
  }
  if (errno != ENOENT)
    return false;
  if (create(path, 0700) == 0)
    return true;
  if (errno != EEXIST)
    return false;
  // Do not accept a regular file or a failed lookup after an EEXIST race.
  if (inspect(path, &info) != 0)
    return false;
  if (S_ISDIR(info.st_mode))
    return true;
  errno = ENOTDIR;
  return false;
}

} // namespace aq::logger::detail
