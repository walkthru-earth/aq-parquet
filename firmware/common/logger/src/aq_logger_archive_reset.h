#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace aq::logger::detail {

struct ArchiveResetIo {
#ifdef ESP_PLATFORM
  // These roots are exclusively on the board's mounted FAT volume. IDF 6.1
  // FatFs exposes only file/directory types and has no symbolic-link/lstat
  // implementation (components/fatfs/vfs/vfs_fat.c). Never reuse this default
  // on another filesystem. Host filesystems must inspect links themselves.
  int (*inspect)(const char *, struct stat *) = ::stat;
#else
  int (*inspect)(const char *, struct stat *) = ::lstat;
#endif
  DIR *(*open)(const char *) = ::opendir;
  dirent *(*next)(DIR *) = ::readdir;
  int (*close)(DIR *) = ::closedir;
  int (*remove_file)(const char *) = ::unlink;
  int (*remove_directory)(const char *) = ::rmdir;
};

namespace archive_reset {
constexpr unsigned kMaximumDepth = 8;
constexpr std::uint32_t kMaximumEntries = 65536;
constexpr std::size_t kPathBytes = 512;

// Preflight the complete tree before deleting anything. Only ordinary files
// and directories are allowed; host lstat rejects links, and board FAT cannot
// contain them.
// The caller has stopped every producer and holds exclusive filesystem access.
inline bool visit(const char *path, unsigned depth, bool remove,
                  std::uint32_t &entries, std::uint32_t &files,
                  std::uint32_t &directories, const ArchiveResetIo &io) {
  if (depth > kMaximumDepth || ++entries > kMaximumEntries) {
    errno = EOVERFLOW;
    return false;
  }
  struct stat info{};
  if (io.inspect(path, &info) != 0)
    return depth == 0 && errno == ENOENT;
  if (S_ISREG(info.st_mode)) {
    if (depth == 0) {
      errno = ENOTDIR;
      return false;
    }
    if (!remove)
      return true;
    if (files == std::numeric_limits<std::uint32_t>::max()) {
      errno = EOVERFLOW;
      return false;
    }
    if (io.remove_file(path) != 0)
      return false;
    ++files;
    return true;
  }
  if (!S_ISDIR(info.st_mode)) {
    errno = EINVAL;
    return false;
  }
  DIR *directory = io.open(path);
  if (!directory)
    return false;
  bool ok = true;
  for (;;) {
    errno = 0;
    const dirent *entry = io.next(directory);
    if (!entry) {
      ok = errno == 0;
      break;
    }
    if (std::strcmp(entry->d_name, ".") == 0 ||
        std::strcmp(entry->d_name, "..") == 0)
      continue;
    if (!entry->d_name[0] || std::strchr(entry->d_name, '/')) {
      errno = EINVAL;
      ok = false;
      break;
    }
    char child[kPathBytes];
    const int length =
        std::snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (length < 0 || static_cast<std::size_t>(length) >= sizeof(child)) {
      errno = ENAMETOOLONG;
      ok = false;
      break;
    }
    if (!visit(child, depth + 1, remove, entries, files, directories, io)) {
      ok = false;
      break;
    }
  }
  const int error = errno;
  if (io.close(directory) != 0 && ok)
    return false;
  if (!ok) {
    errno = error;
    return false;
  }
  if (!remove)
    return true;
  if (directories == std::numeric_limits<std::uint32_t>::max()) {
    errno = EOVERFLOW;
    return false;
  }
  if (io.remove_directory(path) != 0)
    return false;
  ++directories;
  return true;
}
} // namespace archive_reset

// Physical owner-authorized maintenance only. Never accept a caller-selected
// path, card root, radio command or a path derived from a file manifest.
// Counters accumulate across the two allowlisted roots, including root dirs.
inline bool erase_archive(const char *root, std::uint32_t &files_removed,
                          std::uint32_t &directories_removed,
                          const ArchiveResetIo &io = {}) {
  if (!root || (std::strcmp(root, "/sd/output") != 0 &&
                std::strcmp(root, "/sd/parquet") != 0)) {
    errno = EINVAL;
    return false;
  }
  std::uint32_t entries = 0;
  if (!archive_reset::visit(root, 0, false, entries, files_removed,
                            directories_removed, io))
    return false;
  entries = 0;
  return archive_reset::visit(root, 0, true, entries, files_removed,
                              directories_removed, io);
}

} // namespace aq::logger::detail
