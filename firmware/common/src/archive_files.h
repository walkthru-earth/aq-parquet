#pragma once

#include <cstddef>

// FatFs descriptor budget for the archive card, shared by both boards so the
// mount, the FS_LOCK table and the archive session cannot drift apart.
namespace opensensor::storage {
// Telemetry and codec-test writers.
constexpr std::size_t kWriterFiles = 2;
// One short-lived stream: the finalize check, serial `parquet get` or an OPEN
// replacing a slot's stream.
constexpr std::size_t kTransientFiles = 1;
// Persistent archive read streams: BLE plus each LAN peer
// (ossync::ArchiveSession::kReadHandles).
constexpr std::size_t kArchiveReadHandles = 4;
// Mount max_files, with one spare.
constexpr std::size_t kMaxOpenFiles =
    kWriterFiles + kTransientFiles + kArchiveReadHandles + 1;
// FF_FS_LOCK also counts open directories. The archive walk keeps one open
// per level, depth 0 through 6 (aq_logger_directory.h), next to any open
// files. The serial archive reset keeps up to 9 open, depth 0 through 8
// (aq_logger_archive_reset.h). It fits because the reset first closes the
// writer files and every archive read stream, so the walk has the table to
// itself.
constexpr std::size_t kArchiveWalkDirectories = 7;
constexpr std::size_t kMinFsLock = kMaxOpenFiles + kArchiveWalkDirectories;
} // namespace opensensor::storage
