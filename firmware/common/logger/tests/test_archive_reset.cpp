#include "aq_logger_archive_reset.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
namespace fs = std::filesystem;
fs::path card;
std::string mapped(const char *path) {
  assert(std::strncmp(path, "/sd/", 4) == 0);
  return (card / (path + 4)).string();
}
int inspect(const char *path, struct stat *info) {
  return ::lstat(mapped(path).c_str(), info);
}
DIR *open(const char *path) { return ::opendir(mapped(path).c_str()); }
int remove_file(const char *path) { return ::unlink(mapped(path).c_str()); }
int remove_directory(const char *path) { return ::rmdir(mapped(path).c_str()); }
const aq::logger::detail::ArchiveResetIo io{
    inspect, open, ::readdir, ::closedir, remove_file, remove_directory};
void write(const fs::path &path) {
  fs::create_directories(path.parent_path());
  std::ofstream stream(path);
  stream << "retained evidence";
  assert(stream.good());
}
dirent *read_failure(DIR *) {
  errno = EIO;
  return nullptr;
}
int delete_failure(const char *) {
  errno = EACCES;
  return -1;
}
} // namespace

int main() {
  char temporary[] = "/tmp/aq-archive-reset-XXXXXX";
  assert(::mkdtemp(temporary));
  card = temporary;
  using aq::logger::detail::erase_archive;
  std::uint32_t files = 0, directories = 0;
  for (const char *path : {"/sd", "/sd/", "/sd/output/", "/sd/output/../",
                           "/sd/other", "output"}) {
    assert(!erase_archive(path, files, directories, io));
    assert(errno == EINVAL && files == 0 && directories == 0);
  }
  assert(!erase_archive(nullptr, files, directories, io));
  assert(erase_archive("/sd/output", files, directories, io));
  assert(files == 0 && directories == 0);
  write(card / "output");
  assert(!erase_archive("/sd/output", files, directories, io));
  assert(errno == ENOTDIR && fs::is_regular_file(card / "output"));
  fs::remove(card / "output");

  write(card / "output/retained.partial");
  auto broken_io = io;
  broken_io.next = read_failure;
  assert(!erase_archive("/sd/output", files, directories, broken_io));
  assert(errno == EIO && files == 0 && directories == 0);
  assert(fs::exists(card / "output/retained.partial"));
  broken_io = io;
  broken_io.remove_file = delete_failure;
  assert(!erase_archive("/sd/output", files, directories, broken_io));
  assert(errno == EACCES && files == 0 && directories == 0);
  assert(fs::exists(card / "output/retained.partial"));
  fs::remove_all(card / "output");

  write(card / "unrelated.txt");
  write(card / "output/station=old/year=2026/month=10/day=10/part.parquet");
  write(card / "output/quarantine/interrupted.partial");
  write(card / "output/boot-0.partial");
  write(card / "parquet/legacy.parquet");
  fs::create_symlink(card / "unrelated.txt", card / "output/escape");
  assert(!erase_archive("/sd/output", files, directories, io));
  assert(files == 0 && directories == 0);
  assert(fs::exists(card / "output/boot-0.partial"));
  fs::remove(card / "output/escape");
  fs::create_directory_symlink(card, card / "output/escape");
  assert(!erase_archive("/sd/output", files, directories, io));
  assert(files == 0 && directories == 0);
  fs::remove(card / "output/escape");

  // Reject excessive depth in preflight, without partially deleting siblings.
  fs::path deep = card / "output";
  for (unsigned i = 0; i < 9; ++i)
    deep /= "deep";
  fs::create_directories(deep);
  assert(!erase_archive("/sd/output", files, directories, io));
  assert(errno == EOVERFLOW && files == 0 && directories == 0);
  fs::remove_all(card / "output/deep");
  assert(erase_archive("/sd/output", files, directories, io));
  assert(files == 3 && directories == 6);
  assert(!fs::exists(card / "output"));
  assert(fs::exists(card / "parquet/legacy.parquet"));
  assert(erase_archive("/sd/parquet", files, directories, io));
  assert(files == 4 && directories == 7);
  assert(fs::exists(card / "unrelated.txt"));
  assert(erase_archive("/sd/output", files, directories, io));
  assert(files == 4 && directories == 7);

  fs::create_directory_symlink(card, card / "output");
  assert(!erase_archive("/sd/output", files, directories, io));
  assert(fs::exists(card / "unrelated.txt"));
  fs::remove_all(card);
}
