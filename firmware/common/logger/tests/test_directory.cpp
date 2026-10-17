#include "aq_logger_directory.h"

#include <cassert>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

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

struct FakeArchiveIO {
  struct Cursor {
    std::string path;
    unsigned index = 0;
    bool open = false;
  } cursors[2];
  struct dirent entry{};
  std::string fail_open, fail_read, fail_stat, fail_close;
  unsigned opened = 0, closed = 0;

  Cursor *open(const char *path) {
    if (path == fail_open) {
      errno = EIO;
      return nullptr;
    }
    const unsigned index = std::strcmp(path, "/sd/") == 0 ? 0 : 1;
    auto &cursor = cursors[index];
    assert(!cursor.open);
    cursor = {path, 0, true};
    ++opened;
    return &cursor;
  }
  const struct dirent *read(Cursor *cursor) {
    const char *names[2] = {"first.parquet", "day"};
    const bool root = cursor == &cursors[0];
    if (cursor->index == (root ? 2U : 1U)) {
      // The failed SD read happens after a valid file was returned.
      if (cursor->path == fail_read)
        errno = EIO;
      return nullptr;
    }
    std::snprintf(entry.d_name, sizeof(entry.d_name), "%s",
                  root ? names[cursor->index] : "latest.parquet");
    ++cursor->index;
    return &entry;
  }
  int inspect(const char *path, struct stat *info) {
    if (path == fail_stat) {
      errno = EIO;
      return -1;
    }
    info->st_mode = std::strcmp(path, "/sd/day") == 0 ? S_IFDIR : S_IFREG;
    // A successful POSIX call need not clear a previous unrelated errno.
    errno = ERANGE;
    return 0;
  }
  int close(Cursor *cursor) {
    assert(cursor->open);
    cursor->open = false;
    ++closed;
    if (cursor->path == fail_close) {
      errno = EIO;
      return -1;
    }
    return 0;
  }
};

void check_archive_walk() {
  using aq::logger::detail::walk_directory;
  for (unsigned failure = 0; failure < 6; ++failure) {
    FakeArchiveIO io;
    switch (failure) {
    case 1:
      io.fail_read = "/sd/day";
      break;
    case 2:
      io.fail_read = "/sd/";
      break;
    case 3:
      io.fail_open = "/sd/day";
      break;
    case 4:
      io.fail_stat = "/sd/day/latest.parquet";
      break;
    case 5:
      io.fail_close = "/sd/day";
      break;
    }
    std::vector<std::string> files;
    auto visit = [&](const char *name, const struct stat &) {
      files.emplace_back(name);
      return true;
    };
    errno = EACCES;
    const bool complete = walk_directory(io, "/sd", "", 0, visit);
    assert(complete == (failure == 0));
    if (failure)
      assert(errno == EIO);
    assert(io.opened == io.closed); // including recursive error unwinding
    assert(files.size() == (failure == 3 || failure == 4 ? 1U : 2U));
    assert(files[0] == "first.parquet");
    if (files.size() == 2)
      assert(files[1] == "day/latest.parquet");
  }
  FakeArchiveIO io;
  auto visit = [](const char *, const struct stat &) {
    errno = ENAMETOOLONG;
    return false;
  };
  assert(!walk_directory(io, "/sd", "", 0, visit));
  assert(errno == ENAMETOOLONG && io.opened == io.closed);
  assert(!walk_directory(io, "/sd", "", 7, visit));
  assert(errno == EOVERFLOW);
  const std::string long_base(416, 'a');
  assert(!walk_directory(io, long_base.c_str(), "", 0, visit));
  assert(errno == ENAMETOOLONG);
}
// An in-memory tree for the resumable walk. Directories map to their entry
// names; a trailing '/' marks a subdirectory.
struct TreeIO {
  struct Cursor {
    std::string path;
    std::size_t index = 0;
  };
  std::map<std::string, std::vector<std::string>> directories;
  std::vector<std::unique_ptr<Cursor>> cursors;
  struct dirent entry{};
  unsigned opened = 0, closed = 0, reads = 0;

  Cursor *open(const char *path) {
    const auto found = directories.find(path);
    if (found == directories.end()) {
      errno = ENOENT;
      return nullptr;
    }
    ++opened;
    cursors.push_back(std::make_unique<Cursor>(Cursor{path, 0}));
    return cursors.back().get();
  }
  const struct dirent *read(Cursor *cursor) {
    ++reads;
    const auto &names = directories.at(cursor->path);
    if (cursor->index == names.size())
      return nullptr;
    std::string name = names[cursor->index++];
    if (name.back() == '/')
      name.pop_back();
    std::snprintf(entry.d_name, sizeof(entry.d_name), "%s", name.c_str());
    return &entry;
  }
  int inspect(const char *path, struct stat *info) {
    const std::string full(path);
    const auto slash = full.rfind('/');
    const auto &names = directories.at(full.substr(0, slash) +
                                       (slash == 3 ? "/" : ""));
    const std::string leaf = full.substr(slash + 1);
    for (const auto &name : names)
      if (name == leaf || name == leaf + "/") {
        info->st_mode = name.back() == '/' ? S_IFDIR : S_IFREG;
        return 0;
      }
    errno = ENOENT;
    return -1;
  }
  int close(Cursor *) {
    ++closed;
    return 0;
  }
  // "/sd/" holds a day tree with partials and one huge flat directory.
  explicit TreeIO(unsigned huge) {
    directories["/sd/"] = {"a.parquet", "station=x/", "huge/", "z.parquet"};
    directories["/sd/station=x"] = {"year=2026/", "b.partial", ".hidden"};
    directories["/sd/station=x/year=2026"] = {"month=01/", "month=02/"};
    directories["/sd/station=x/year=2026/month=01"] = {"c.parquet", "d.parquet"};
    directories["/sd/station=x/year=2026/month=02"] = {};
    auto &flat = directories["/sd/huge"];
    for (unsigned i = 0; i < huge; ++i)
      flat.push_back("f" + std::to_string(i) + (i % 10 ? ".parquet" : ".partial"));
  }
};

// A walk resumed in bounded steps visits exactly what one unbounded walk
// does, and no step reads more entries than its budget.
void check_resumable_walk() {
  using aq::logger::detail::DirectoryWalk;
  using aq::logger::detail::walk_directory;
  using Status = DirectoryWalk<TreeIO>::Status;
  std::vector<std::string> whole;
  {
    TreeIO io(10000);
    auto visit = [&](const char *name, const struct stat &) {
      whole.emplace_back(name);
      return true;
    };
    assert(walk_directory(io, "/sd", "", 0, visit));
    assert(io.opened == io.closed && io.opened == 6);
  }
  assert(whole.size() == 4 + 1 + 10000);
  assert(whole[0] == "a.parquet" &&
         whole[1] == "station=x/year=2026/month=01/c.parquet" &&
         whole.back() == "z.parquet");
  for (const unsigned budget : {1U, 7U, 32U, 4096U}) {
    TreeIO io(10000);
    std::vector<std::string> stepped;
    auto visit = [&](const char *name, const struct stat &) {
      stepped.emplace_back(name);
      return true;
    };
    DirectoryWalk<TreeIO> walk;
    assert(walk.begin(io, "/sd"));
    unsigned steps = 0;
    Status status;
    do {
      const unsigned before = io.reads;
      status = walk.step(budget, visit);
      assert(io.reads - before <= budget);
      ++steps;
    } while (status == Status::More);
    assert(status == Status::Done && !walk.active());
    assert(stepped == whole && io.opened == io.closed);
    assert(steps >= io.reads / budget);
  }
  // A visitor that refuses stops the walk inside the step and closes every
  // open directory. Nothing is read afterwards.
  {
    TreeIO io(10000);
    unsigned visits = 0;
    auto visit = [&](const char *, const struct stat &) { return ++visits < 3; };
    DirectoryWalk<TreeIO> walk;
    assert(walk.begin(io, "/sd"));
    assert(walk.step(100000, visit) == Status::Stopped);
    assert(visits == 3 && !walk.active() && io.opened == io.closed);
    const unsigned reads = io.reads;
    assert(walk.step(100000, visit) == Status::Done && io.reads == reads);
  }
  // cancel() mid walk, and a destroyed walk, close what is open.
  {
    TreeIO io(100);
    auto visit = [](const char *, const struct stat &) { return true; };
    {
      DirectoryWalk<TreeIO> walk;
      assert(walk.begin(io, "/sd"));
      assert(walk.step(3, visit) == Status::More && walk.active());
      assert(io.opened > io.closed);
    }
    assert(io.opened == io.closed);
    DirectoryWalk<TreeIO> walk;
    assert(walk.begin(io, "/sd"));
    assert(walk.step(5, visit) == Status::More);
    assert(walk.cancel() && !walk.active() && io.opened == io.closed);
    assert(!walk.begin(io, "/missing") && errno == ENOENT);
  }
}
void check_quarantine_names() {
  using aq::logger::detail::quarantine_target;
  char directory[416], path[416];
  assert(quarantine_target(
      "/sd/output",
      "station=abc/year=2026/month=10/day=11/data_1200_b1_42-3.partial", "b2",
      7, directory, sizeof(directory), path, sizeof(path)));
  assert(std::strcmp(directory, "/sd/output/quarantine/station=abc/"
                                "year=2026_month=10_day=11") == 0);
  assert(std::strcmp(path, "/sd/output/quarantine/station=abc/"
                           "year=2026_month=10_day=11/"
                           "data_1200_b1_42-3-b2-00000007.partial") == 0);
  assert(quarantine_target("/sd/output",
                           "station=abc/unsynced/boot=b1/data_unsynced_b1_0-0."
                           "partial",
                           "b1", 0, directory, sizeof(directory), path,
                           sizeof(path)));
  assert(std::strcmp(path, "/sd/output/quarantine/station=abc/unsynced_boot=b1/"
                           "data_unsynced_b1_0-0-b1-00000000.partial") == 0);
  assert(quarantine_target("/sd/output",
                           "benchmarks/boot=b1/lz4_raw_b1_0-1."
                           "partial",
                           "b1", 1, directory, sizeof(directory), path,
                           sizeof(path)));
  assert(std::strcmp(directory,
                     "/sd/output/quarantine/no-station/benchmarks_boot=b1") ==
         0);
  assert(quarantine_target("/sd/output", "stray-0.partial", "b1", 2, directory,
                           sizeof(directory), path, sizeof(path)));
  assert(std::strcmp(path, "/sd/output/quarantine/no-station/root/"
                           "stray-0-b1-00000002.partial") == 0);
  // Rejections: not a partial, unsafe segments, absolute or too long.
  for (const char *bad :
       {"station=a/x.parquet", "station=a/../x.partial", "station=a//x.partial",
        "/abs/x.partial", "./x.partial", ".partial"}) {
    errno = 0;
    assert(!quarantine_target("/sd/output", bad, "b1", 0, directory,
                              sizeof(directory), path, sizeof(path)));
  }
  assert(!quarantine_target("/sd/output", "station=a/x-0.partial", "b1", 0,
                            directory, 24, path, sizeof(path)));
  assert(errno == ENAMETOOLONG);
  assert(!quarantine_target("/sd/output", "station=a/x-0.partial", "b1", 0,
                            directory, sizeof(directory), path, 40));
  assert(errno == ENAMETOOLONG);
  const std::string long_station =
      "station=" + std::string(120, 's') + "/x-0.partial";
  assert(!quarantine_target("/sd/output", long_station.c_str(), "b1", 0,
                            directory, sizeof(directory), path, sizeof(path)));
  assert(errno == ENAMETOOLONG);
}

struct PosixQuarantineIO {
  bool make_directories(const char *path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return !error;
  }
  bool exists(const char *path) { return ::access(path, F_OK) == 0; }
  int rename(const char *from, const char *to) { return ::rename(from, to); }
};

// A write failure quarantines its own .partial in the same boot, with
// provenance, and never replaces an existing quarantined fragment.
void check_same_boot_quarantine() {
  namespace fs = std::filesystem;
  char temporary[] = "/tmp/aq-quarantine-XXXXXX";
  assert(::mkdtemp(temporary));
  const fs::path root = fs::path(temporary) / "output";
  const char *relative =
      "station=abc/year=2026/month=10/day=11/data_1200_b1_42-3.partial";
  fs::create_directories((root / relative).parent_path());
  {
    std::ofstream(root / relative) << "partial bytes";
  }
  PosixQuarantineIO io;
  char moved[416];
  assert(aq::logger::detail::quarantine_partial(io, root.c_str(), relative,
                                                "b1", 0, moved, sizeof(moved)));
  assert(!fs::exists(root / relative));
  assert(fs::exists(root / "station=abc/year=2026/month=10/day=11"));
  const fs::path expected = root / "quarantine/station=abc/"
                                   "year=2026_month=10_day=11/"
                                   "data_1200_b1_42-3-b1-00000000.partial";
  assert(moved == expected.string() && fs::file_size(expected) == 13);
  // Same counter again must refuse to overwrite the earlier fragment.
  {
    std::ofstream(root / relative) << "second";
  }
  assert(!aq::logger::detail::quarantine_partial(
      io, root.c_str(), relative, "b1", 0, moved, sizeof(moved)));
  assert(errno == EEXIST && fs::exists(root / relative));
  assert(fs::file_size(expected) == 13);
  assert(aq::logger::detail::quarantine_partial(io, root.c_str(), relative,
                                                "b1", 1, moved, sizeof(moved)));
  assert(!fs::exists(root / relative));
  // A missing source fails and creates nothing beyond the directory.
  assert(!aq::logger::detail::quarantine_partial(
      io, root.c_str(), relative, "b1", 2, moved, sizeof(moved)));
  fs::remove_all(temporary);
}

struct FakeListing {
  std::vector<std::string> names;
  bool fail_open = false, fail_read = false, fail_close = false;
  unsigned index = 0;
  bool opened = false;
  struct dirent entry{};
  FakeListing *open(const char *) {
    if (fail_open) {
      errno = EIO;
      return nullptr;
    }
    opened = true;
    index = 0;
    return this;
  }
  const struct dirent *read(FakeListing *) {
    if (index == names.size()) {
      if (fail_read)
        errno = EIO;
      return nullptr;
    }
    std::snprintf(entry.d_name, sizeof(entry.d_name), "%s",
                  names[index++].c_str());
    return &entry;
  }
  int close(FakeListing *) {
    opened = false;
    return fail_close ? -1 : 0;
  }
};

void check_crosslink_guard() {
  using aq::logger::detail::crosslink_pair;
  using aq::logger::detail::crosslink_suspect;
  assert(crosslink_pair("data_1200_b1_42-3.partial",
                        "data_1200_b1_42-131-3.parquet"));
  assert(crosslink_pair("data_unsynced_b1_0-12.partial",
                        "data_unsynced_b1_0-5-12.parquet"));
  // Different attempt, different stem, missing last or not a number.
  assert(!crosslink_pair("data_1200_b1_42-3.partial",
                         "data_1200_b1_42-131-4.parquet"));
  assert(!crosslink_pair("data_1200_b1_42-3.partial",
                         "data_1200_b1_43-131-3.parquet"));
  assert(!crosslink_pair("data_1200_b1_42-3.partial",
                         "data_1200_b1_42--3.parquet"));
  assert(!crosslink_pair("data_1200_b1_42-3.partial",
                         "data_1200_b1_42-x1-3.parquet"));
  assert(!crosslink_pair("data_1200_b1_42-3.partial",
                         "data_1200_b1_42-131-13.parquet"));
  assert(!crosslink_pair("data_1200_b1_42-3.partial", "other.parquet"));
  assert(!crosslink_pair("nodash.partial", "nodash-1-2.parquet"));
  assert(!crosslink_pair("x-.partial", "x-1-.parquet"));

  FakeListing listing;
  listing.names = {"data_1200_b1_42-131-3.parquet", "data_1200_b1_42-3.partial",
                   "data_1215_b1_132-4.partial"};
  assert(crosslink_suspect(listing, "/sd/day", "data_1200_b1_42-3.partial"));
  assert(!listing.opened);
  assert(!crosslink_suspect(listing, "/sd/day", "data_1215_b1_132-4.partial"));
  // An unreadable directory is treated as suspect so nothing moves.
  listing.fail_read = true;
  assert(crosslink_suspect(listing, "/sd/day", "data_1215_b1_132-4.partial"));
  listing.fail_read = false;
  listing.fail_open = true;
  assert(crosslink_suspect(listing, "/sd/day", "data_1215_b1_132-4.partial"));
  listing.fail_open = false;
  listing.fail_close = true;
  assert(crosslink_suspect(listing, "/sd/day", "data_1215_b1_132-4.partial"));
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
  check_archive_walk();
  check_resumable_walk();
  check_quarantine_names();
  check_same_boot_quarantine();
  check_crosslink_guard();
}
