#include "aq_logger_directory.h"

#include <cassert>
#include <cstring>

namespace {
enum class Scenario {
  MountedRoot,
  ExistingFile,
  Missing,
  Denied,
  RaceDir,
  RaceFile
};
Scenario scenario;
unsigned inspections = 0;
unsigned creations = 0;

int inspect(const char *path, struct stat *info) {
  assert(std::strcmp(path, "/sd") == 0);
  ++inspections;
  if (scenario == Scenario::Denied) {
    errno = EACCES;
    return -1;
  }
  if (scenario == Scenario::Missing ||
      ((scenario == Scenario::RaceDir || scenario == Scenario::RaceFile) &&
       inspections == 1)) {
    errno = ENOENT;
    return -1;
  }
  info->st_mode =
      scenario == Scenario::ExistingFile || scenario == Scenario::RaceFile
          ? S_IFREG
          : S_IFDIR;
  return 0;
}

int create(const char *path, mode_t mode) {
  assert(std::strcmp(path, "/sd") == 0 && mode == 0700);
  ++creations;
  if (scenario == Scenario::MountedRoot) {
    // ESP-IDF FatFs mkdir('/') differs from POSIX mkdir of an existing dir.
    errno = EINVAL;
    return -1;
  }
  if (scenario == Scenario::RaceDir || scenario == Scenario::RaceFile) {
    errno = EEXIST;
    return -1;
  }
  assert(scenario == Scenario::Missing);
  return 0;
}

bool check(Scenario value) {
  scenario = value;
  inspections = creations = 0;
  errno = ERANGE; // Successful stat/mkdir need not clear a prior errno.
  return aq::logger::detail::ensure_directory("/sd", inspect, create);
}
} // namespace

int main() {
  assert(check(Scenario::MountedRoot));
  assert(inspections == 1 && creations == 0);
  assert(!check(Scenario::ExistingFile));
  assert(errno == ENOTDIR && creations == 0);
  assert(check(Scenario::Missing));
  assert(creations == 1);
  assert(!check(Scenario::Denied));
  assert(errno == EACCES && creations == 0);
  assert(check(Scenario::RaceDir));
  assert(inspections == 2 && creations == 1);
  assert(!check(Scenario::RaceFile));
  assert(errno == ENOTDIR && inspections == 2 && creations == 1);
}
