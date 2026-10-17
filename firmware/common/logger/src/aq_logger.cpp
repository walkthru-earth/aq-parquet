#include "aq_logger.h"
#include "aq_logger_archive_reset.h"
#include "aq_logger_directory.h"
#include "aq_logger_live.h"
#include "aq_logger_location.h"
#include "aq_logger_provision.h"
#include "aq_logger_rotation.h"
#include "aq_logger_status.h"
#include "aq_logger_watchdog.h"
#include "aq_logger_watchdog_apply.h"
#include "aq_logger_work_queue.h"
#include "crc_cache.h"
#include "debug_log.h"
#include "device_config.h"
#include "lz4_codec.h"
#include "parquet_writer.h"
#include "storage_governor.h"
#include "wifi_link.h"
#include <abort_signal.h>
#include <archive_files.h>
#include <archive_sync.h>
#include <control_sync.h>
#include <sync_codec.h>
#include <sync_service.h>
#include <time_sync.h>
#include <utc_clock.h>

#include "aq_console.h"
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <new>
#include <numeric>
#include <sys/stat.h>
#include <unistd.h>

namespace aqlogger {
namespace {
using namespace telemetry;
using Sample = LocationSample<Row>;
Config config;
Hooks hooks;
std::size_t field_count = 0;
std::size_t sequence = 0, event_time_utc_us = 0, clock_epoch = 0;
std::size_t monotonic_field = 0;
constexpr std::int32_t kClockNone = aq::utc::None;
constexpr std::int32_t kClockHost = aq::utc::Host;
constexpr std::int32_t kClockRtc = aq::utc::Rtc;
using aq::sync::crc32_update;
using aq::sync::get_i64;
using aq::sync::get_u16;
using aq::sync::get_u32;
using aq::sync::put_i64;
using aq::sync::put_u16;
using aq::sync::put_u32;
using aq::utc::days_from_civil;
constexpr std::size_t kMaxRows = 90;
constexpr std::int64_t kSampleUs = 10000000;
const char *kDirectory = "/sd/output";
const char *kBenchDirectory = "/sd/bench";
// Footer key/value budget. finalize_file fills a fixed array in this order.
constexpr std::size_t kFixedMetadata = 22;
constexpr std::size_t kLocationMetadata =
    std::tuple_size<decltype(LocationMetadata::entries)>::value;
constexpr std::size_t kAnchorMetadata = 3;
constexpr std::size_t kMaxConfigMetadata = 32;
constexpr std::size_t kFooterMetadata = 64;
static_assert(kFixedMetadata + kLocationMetadata + kAnchorMetadata +
                      kMaxConfigMetadata <=
                  kFooterMetadata,
              "footer metadata array is too small");
// One Parquet file in progress: created as `.partial`, grown by one row group
// per completed RAM batch (each fsynced), finalized with the footer at the
// window boundary, at kMaxRowGroups, or on an explicit flush. Rows already in
// a row group are on the card; only the RAM batch is lost on reset.
struct OutputFile {
  FILE *file = nullptr;
  Writer writer;
  Workspace workspace{};
  char stem[384]{}; // <partition>/<prefix>_<boot>_<first>
  char partial[416]{};
  char *staging = nullptr;    // stdio buffer, see WriterState
  const char *root = nullptr; // kDirectory or kBenchDirectory
  bool benchmark = false;
  bool dated = false; // false: unsynced tree
  Codec codec = Codec::Uncompressed;
  config::LocationSettings location{};
  std::int32_t epoch = 0;
  std::int64_t window = 0; // UTC seconds / rotation, when dated
  std::int64_t first = 0, last = 0;
  std::uint32_t rows = 0;
  std::uint32_t attempt = 0;
  std::int64_t opened_us = 0;
  std::uint64_t writer_us = 0, codec_us = 0, sync_us = 0;
  bool open() const { return file != nullptr; }
};
constexpr std::size_t kCrcCacheEntries = 64;
constexpr std::size_t kCrcScratchBytes = 4096;
static_assert(aqsync::ArchiveSession::kReadHandles <=
                  aq::storage::kArchiveReadHandles,
              "mount max_files must cover every archive read handle");
struct WriterState {
  Sample rows[kMaxRows];
  Column columns[telemetry::kMaxColumns];
  OutputFile telemetry;
  // codec-test copies never touch the telemetry file. The benchmark file is
  // allocated from PSRAM only while codec-test runs and is null otherwise.
  OutputFile *benchmark = nullptr;
  Lz4Workspace lz4{};
  // Archive reads. Finalize-time CRCs for OPEN, the CRC read block and one
  // stdio buffer per persistent archive read handle. Worker only.
  aq::storage::CrcCache<kCrcCacheEntries> crc_cache;
  std::uint8_t crc_scratch[kCrcScratchBytes]{};
  char read_buffers[aqsync::ArchiveSession::kReadHandles]
                   [aqsync::ArchiveSession::kReadBufferBytes]{};
};
// Telemetry stdio staging stays in internal RAM, distinct from the PSRAM row
// buffer. codec-test staging is PSRAM, allocated only while the test runs.
constexpr std::size_t kStagingBytes = 4096;
char staging_telemetry[kStagingBytes];
struct Command {
  enum class Source : std::uint8_t { Serial, Control, Network };
  Source source = Source::Serial;
  char text[2 * ble::kMaxControlBytes + 32]{};
  std::int64_t received_mono_us = 0;
  ble::ControlRequest control{};
  lan::TimeAnchor network_time{};
};
WriterState *writer_state = nullptr;
QueueHandle_t samples = nullptr;
QueueHandle_t commands = nullptr;
SemaphoreHandle_t spi_mutex = nullptr;
SemaphoreHandle_t worker_wakeup = nullptr;
aqsync::ServiceConfig sync_config{};
std::atomic<std::uint32_t> dropped{0}, errors{0}, finalized{0}, buffered{0};
std::atomic<std::uint32_t> write_us{0}, sync_us{0}, queue_peak{0};
std::atomic<std::uint32_t> total_kib{0}, used_kib{0}, rotation_seconds{900};
// Worker state mirrored for the BLE `status` document, which any task may
// build.
std::atomic<std::uint32_t> partials_seen{0}, partials_quarantined{0};
std::atomic<std::uint64_t> quarantine_bytes{0};
// Rows/row groups already on the card in the open .partial (footer pending).
std::atomic<std::uint32_t> open_rows{0}, open_groups{0};
std::atomic<bool> worker_failed{false}, storage_ok{false};
// Storage worker only. Mirrored to sd_state for STATUS.
storage::Governor governor;
std::atomic<std::uint8_t> sd_state{
    static_cast<std::uint8_t>(storage::State::Retry)};
std::uint32_t quarantine_counter = 0; // unique per boot
// Advertising bit `new_files`: set when a file finalizes, cleared when a LIST
// has been answered on any link (that phone now knows; a phone that then fails
// its download is covered by its periodic run).
// docs/shared/ble-sync-protocol.md.
std::atomic<bool> unlisted_files{false};
std::int64_t boot_hi = 0, boot_lo = 0, device = 0, next_sample_us = 0;
std::int64_t sample_sequence = 0;
std::atomic<std::int64_t> missed_deadlines{0};
// Worker liveness: the loop and every long walk stamp this; poll warns when
// it stops moving for 5 s, and the watchdog supervisor resets the chip when
// it stops for CONFIG_AQ_LOGGER_WORKER_WDT_TIMEOUT_S. begin() seeds it before
// the worker starts, so a worker that never stamps still ages out.
std::atomic<std::int64_t> worker_heartbeat_us{0};
#if CONFIG_AQ_LOGGER_WORKER_WDT
// Survives the task-watchdog panic reset, lost on power-on.
RTC_NOINIT_ATTR watchdog::Guard stall_guard;
std::atomic<esp_task_wdt_user_handle_t> worker_wdt{nullptr};
#endif
// The single heartbeat stamp. It also feeds the worker's task-watchdog user.
void stamp_heartbeat() {
  worker_heartbeat_us = esp_timer_get_time();
#if CONFIG_AQ_LOGGER_WORKER_WDT
  if (auto *user = worker_wdt.load())
    esp_task_wdt_reset_user(user);
#endif
}
TaskHandle_t worker_task = nullptr;
char boot_text[33]{};
char device_text[13]{};
char station_text[37]{};
portMUX_TYPE clock_mutex = portMUX_INITIALIZER_UNLOCKED;
std::int64_t anchor_mono_us = 0, anchor_utc_ns = 0;
std::int32_t clock_generation = 0;
std::int32_t clock_source = kClockNone;
// A host sync asks the main task to copy the anchor through the optional
// board RTC callback; the storage worker never performs board RTC IO.
std::atomic<bool> rtc_write_pending{false};
std::atomic<std::int64_t> rtc_write_requested_us{0};
std::atomic<std::int32_t> rtc_state{0}; // 0 unread, 1 seeded, 2 unusable
bool mounted = false;
bool accepting = false;
std::atomic<bool> archive_reset{false};
std::atomic<Codec> selected_codec{Codec::Uncompressed};
const char *codec_name(Codec codec) {
  return codec == Codec::Lz4Raw ? "LZ4_RAW" : "UNCOMPRESSED";
}

class BusLock {
public:
  BusLock() { lock_bus(); }
  ~BusLock() { unlock_bus(); }
};

// Writer output. Each block that reaches the file is confirmed progress, so it
// stamps the heartbeat after the write and never before. A write that never
// returns still trips the watchdog, a slow one that keeps moving does not.
bool sink(void *context, const std::uint8_t *data, std::size_t size) {
  BusLock lock;
  FILE *file = static_cast<FILE *>(context);
  std::size_t written = 0;
  while (written < size) {
    const std::size_t count =
        std::fwrite(data + written, 1, size - written, file);
    if (!count) {
      // Preserve the first useful errno when stdio reports only its error bit.
      if (!errno && std::ferror(file))
        errno = EIO;
      return false;
    }
    written += count;
    stamp_heartbeat();
  }
  return true;
}

// Cache key of a finalized file (crc_cache.h) under the bus lock.
bool crc_key(const char *path, aq::storage::CrcKey &key) {
  BusLock lock;
  return aq::storage::crc_stat_key(path, key);
}

// Structural completion check and CRC-32 of `file`, an open stream. PyArrow +
// DuckDB perform conformance. With use_cache, an entry matching path, size
// and mtime replaces the full read, and a computed CRC is remembered.
bool check_file(const char *path, FILE *file, aqsync::FileCheck &check,
                bool use_cache) {
  std::uint8_t magic[4]{}, tail[8]{};
  bool ok = false;
  {
    BusLock lock;
    if (std::fseek(file, 0, SEEK_END) == 0) {
      const long length = std::ftell(file);
      if (length >= 12 && length <= 1048576) {
        check.size = static_cast<std::uint32_t>(length);
        ok = std::fseek(file, 0, SEEK_SET) == 0 &&
             std::fread(magic, 1, 4, file) == 4 &&
             std::fseek(file, -8, SEEK_END) == 0 &&
             std::fread(tail, 1, 8, file) == 8;
        ok = ok && aq::sync::parquet_complete(check.size, magic, tail) &&
             std::fseek(file, 0, SEEK_SET) == 0;
      }
    }
  }
  if (!ok)
    return false;
  aq::storage::CrcKey key;
  const bool keyed = use_cache && crc_key(path, key) && key.size == check.size;
  auto &cache = writer_state->crc_cache;
  check.cached = keyed && cache.lookup(key, check.crc);
  if (check.cached)
    return true;
  std::uint32_t crc = 0xffffffffU, read_bytes = 0;
  while (read_bytes < check.size) {
    std::size_t count;
    {
      BusLock lock;
      count = std::fread(writer_state->crc_scratch, 1, kCrcScratchBytes, file);
    }
    if (!count)
      return false;
    crc = crc32_update(crc, writer_state->crc_scratch, count);
    read_bytes += count;
    stamp_heartbeat(); // a 1 MiB CRC pass is progress, not a stall
  }
  if (read_bytes != check.size)
    return false;
  check.crc = crc ^ 0xffffffffU;
  if (keyed)
    cache.insert(key, check.crc);
  return true;
}

bool finalized_file(const char *path, std::uint32_t &size, std::uint32_t &crc) {
  FILE *file = nullptr;
  {
    BusLock lock;
    file = std::fopen(path, "rb");
  }
  if (!file)
    return false;
  aqsync::FileCheck check;
  bool ok = check_file(path, file, check, false);
  {
    BusLock lock;
    ok = std::fclose(file) == 0 && ok;
  }
  size = check.size;
  crc = check.crc;
  return ok;
}

// Called once a file is finalized under its ready name, so the first OPEN
// skips the full CRC read.
void remember_crc(const char *path, std::uint32_t size, std::uint32_t crc) {
  aq::storage::CrcKey key;
  if (crc_key(path, key) && key.size == size)
    writer_state->crc_cache.insert(key, crc);
}

void prepare_columns() {
  for (std::size_t i = 0; i < field_count; ++i) {
    const auto &field = config.fields[i];
    const auto logical =
        field.type == PhysicalType::Int64 &&
                std::strcmp(field.unit, "us_since_unix_epoch") == 0
            ? LogicalType::TimestampMicrosUtc
            : LogicalType::None;
    writer_state->columns[i] = Column{
        field.name, field.type, &writer_state->rows[0].data[i], sizeof(Sample),
        &writer_state->rows[0].valid[i], sizeof(Sample), logical,
        // 1-based dictionary position, see
        // aqlogger::Config::fields.
        static_cast<std::int32_t>(i + 1)};
  }
}

bool station_identity() {
  nvs_handle_t settings;
  if (nvs_open("parquet", NVS_READWRITE, &settings) != ESP_OK)
    return false;
  std::size_t length = sizeof(station_text);
  const esp_err_t found =
      nvs_get_str(settings, "station", station_text, &length);
  if (found == ESP_ERR_NVS_NOT_FOUND) {
    std::uint8_t id[16];
    esp_fill_random(id, sizeof(id));
    id[6] = (id[6] & 0x0f) | 0x40;
    id[8] = (id[8] & 0x3f) | 0x80;
    std::snprintf(
        station_text, sizeof(station_text),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7], id[8], id[9],
        id[10], id[11], id[12], id[13], id[14], id[15]);
    if (nvs_set_str(settings, "station", station_text) != ESP_OK ||
        nvs_commit(settings) != ESP_OK) {
      nvs_close(settings);
      return false;
    }
  } else if (found != ESP_OK) {
    nvs_close(settings);
    return false;
  }
  nvs_close(settings);
  if (std::strlen(station_text) != 36)
    return false;
  for (std::size_t i = 0; i < 36; ++i) {
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? station_text[i] != '-'
             : !((station_text[i] >= '0' && station_text[i] <= '9') ||
                 (station_text[i] >= 'a' && station_text[i] <= 'f')))
      return false;
  }
  return true;
}

bool make_directories(const char *path) {
  char copy[416];
  if (std::strlen(path) >= sizeof(copy))
    return false;
  std::strcpy(copy, path);
  // The board already mounted its filesystem; create directories only.
  for (char *part = copy + 1;; ++part) {
    if (*part != '/' && *part != '\0')
      continue;
    const char saved = *part;
    *part = '\0';
    bool ok;
    {
      BusLock lock;
      ok = aq::logger::detail::ensure_directory(copy);
    }
    *part = saved;
    if (!ok)
      return false;
    if (!saved)
      return true;
  }
}

// Rows per row group for the current rotation window.
// 600 s -> 60-row groups, one per file; 900 s -> 90 x 1; 1800 s -> 90 x 2;
// 3600 s -> 90 x 4. The RAM batch (loss window) never exceeds kMaxRows.
std::size_t rows_per_group() {
  const std::size_t rows = rotation_seconds.load() / 10;
  return rows < kMaxRows ? rows : kMaxRows;
}

// Dated rows rotate on UTC, unsynced rows on monotonic time of this boot.
std::int64_t window_index(const Sample &row) {
  const auto rotation = static_cast<std::int64_t>(rotation_seconds.load());
  return row.valid[event_time_utc_us]
             ? rotation_window(row.data[event_time_utc_us] / 1000000, rotation)
             : rotation_window(row.data[monotonic_field] / 1000000, rotation);
}

// Whether `row` may join the file/batch that `reference` started: same clock
// epoch, and the same UTC window when dated (never across midnight or a
// clock correction).
bool same_window(const Sample &row, std::int32_t epoch, bool dated,
                 std::int64_t window) {
  const bool row_dated = row.valid[event_time_utc_us] != 0;
  return same_rotation_window(static_cast<std::int32_t>(row.data[clock_epoch]),
                              row_dated, window_index(row), epoch, dated,
                              window);
}

struct ArchiveDirectoryIO {
  static DIR *open(const char *path) {
    BusLock lock;
    return ::opendir(path);
  }
  static const struct dirent *read(DIR *directory) {
    stamp_heartbeat(); // archive walks are progress, not a stall
    BusLock lock;
    errno = 0;
    return ::readdir(directory);
  }
  static int inspect(const char *path, struct stat *info) {
    BusLock lock;
    return ::stat(path, info);
  }
  static int close(DIR *directory) {
    BusLock lock;
    return ::closedir(directory);
  }
};

// Adds the quarantine moves, one bus lock per call.
struct StorageIO : ArchiveDirectoryIO {
  static bool make_directories(const char *path) {
    return aqlogger::make_directories(path);
  }
  static bool exists(const char *path) {
    BusLock lock;
    return ::access(path, F_OK) == 0;
  }
  static int rename(const char *from, const char *to) {
    BusLock lock;
    return ::rename(from, to);
  }
};

// Moves the .partial at `relative` below `root` into provenance quarantine,
// unless its finalized sibling exists (possible FAT crosslink after a power
// cut inside rename). Suspects stay untouched. Nothing is ever deleted.
bool quarantine_one(const char *root, const char *relative,
                    const char *origin) {
  const char *base = std::strrchr(relative, '/');
  base = base ? base + 1 : relative;
  char directory[416];
  const int length =
      base == relative
          ? std::snprintf(directory, sizeof(directory), "%s", root)
          : std::snprintf(directory, sizeof(directory), "%s/%.*s", root,
                          static_cast<int>(base - relative - 1), relative);
  if (length < 0 || static_cast<std::size_t>(length) >= sizeof(directory)) {
    ++errors;
    aqlog.printf("PARQUET ERROR operation=quarantine reason=path-too-long\n");
    return false;
  }
  StorageIO io;
  if (aq::logger::detail::crosslink_suspect(io, directory, base)) {
    aqlog.printf("PARQUET RECOVERY crosslink-suspect file=%s action=none\n",
                 relative);
    return false;
  }
  char source[416];
  struct stat info{};
  std::snprintf(source, sizeof(source), "%s/%s", root, relative);
  if (io.inspect(source, &info) != 0)
    info.st_size = 0;
  char destination[416];
  const bool ok = aq::logger::detail::quarantine_partial(
      io, root, relative, boot_text, quarantine_counter++, destination,
      sizeof(destination));
  if (!ok) {
    ++errors;
    aqlog.printf("PARQUET ERROR operation=quarantine file=%s errno=%d\n",
                 relative, errno);
    return false;
  }
  aqlog.printf("PARQUET QUARANTINE source=%s target=%s bytes=%llu origin=%s\n",
               relative, destination + std::strlen(root) + 1,
               static_cast<unsigned long long>(info.st_size), origin);
  return true;
}

void close_failed(OutputFile &target, const char *operation,
                  const char *reason) {
  ++errors;
  aqlog.printf("PARQUET ERROR operation=%s file=%s reason=%s errno=%d "
               "rows_on_card=%lu\n",
               operation, target.stem, reason ? reason : "io-or-validation",
               errno, static_cast<unsigned long>(target.rows));
  if (target.file) {
    bool closed;
    {
      BusLock lock;
      closed = std::fclose(target.file) == 0;
    }
    if (!closed) {
      ++errors;
      aqlog.printf("PARQUET ERROR operation=close file=%s errno=%d\n",
                   target.stem, errno);
    }
  }
  target.file = nullptr;
  if (!target.benchmark)
    open_rows = open_groups = 0;
  // The .partial is retained, never repaired. Move it out of the active tree
  // now, so the next file and LIST never see it beside live data.
  char relative[416];
  const int length =
      std::snprintf(relative, sizeof(relative), "%s-%lu.partial", target.stem,
                    static_cast<unsigned long>(target.attempt));
  if (target.root && length > 0 &&
      static_cast<std::size_t>(length) < sizeof(relative) &&
      quarantine_one(target.root, relative, "same-boot") &&
      target.root == kDirectory) {
    if (partials_quarantined.load() != UINT32_MAX)
      ++partials_quarantined;
  }
}

// Creates `<partition>/<prefix>_<boot>_<first>-<attempt>.partial` from the
// first buffered row and starts the writer.
bool create_file(OutputFile &target, Codec codec, bool benchmark) {
  static std::uint32_t attempt = 0;
  const Sample &first_row = writer_state->rows[0];
  char partition[160], prefix[24];
  int partition_length;
  target.dated = false;
  target.root = benchmark ? kBenchDirectory : kDirectory;
  target.window = window_index(first_row);
  if (benchmark) {
    partition_length =
        std::snprintf(partition, sizeof(partition), "boot=%s", boot_text);
    std::strcpy(prefix, codec == Codec::Lz4Raw ? "lz4_raw" : "uncompressed");
  } else if (first_row.valid[event_time_utc_us]) {
    const std::int64_t seconds = first_row.data[event_time_utc_us] / 1000000;
    const std::time_t window =
        (seconds / rotation_seconds.load()) * rotation_seconds.load();
    std::tm utc{};
    if (!::gmtime_r(&window, &utc)) {
      ++errors;
      return false;
    }
    partition_length =
        std::snprintf(partition, sizeof(partition),
                      "station=%s/year=%04d/month=%02d/day=%02d", station_text,
                      utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
    std::snprintf(prefix, sizeof(prefix), "data_%02d%02d", utc.tm_hour,
                  utc.tm_min);
    target.dated = true;
  } else {
    partition_length =
        std::snprintf(partition, sizeof(partition),
                      "station=%s/unsynced/boot=%s", station_text, boot_text);
    std::strcpy(prefix, "data_unsynced");
  }
  char directory[192];
  const int directory_length = std::snprintf(directory, sizeof(directory),
                                             "%s/%s", target.root, partition);
  if (partition_length < 0 ||
      static_cast<std::size_t>(partition_length) >= sizeof(partition) ||
      directory_length < 0 ||
      static_cast<std::size_t>(directory_length) >= sizeof(directory)) {
    ++errors;
    aqlog.println("PARQUET ERROR operation=create reason=path-too-long");
    return false;
  }
  if (!make_directories(directory)) {
    ++errors;
    aqlog.println("PARQUET ERROR operation=mkdir");
    return false;
  }
  target.benchmark = benchmark;
  target.codec = codec;
  target.location = first_row.location;
  target.epoch = static_cast<std::int32_t>(first_row.data[clock_epoch]);
  target.first = target.last = first_row.data[sequence];
  target.rows = 0;
  target.writer_us = target.codec_us = target.sync_us = 0;
  target.attempt = attempt++;
  target.opened_us = esp_timer_get_time();
  const int stem_length = std::snprintf(
      target.stem, sizeof(target.stem), "%s/%s_%s_%lld", partition, prefix,
      boot_text, static_cast<long long>(target.first));
  const int partial_length = std::snprintf(
      target.partial, sizeof(target.partial), "%s/%s-%lu.partial", target.root,
      target.stem, static_cast<unsigned long>(target.attempt));
  if (stem_length < 0 ||
      static_cast<std::size_t>(stem_length) >= sizeof(target.stem) ||
      partial_length < 0 ||
      static_cast<std::size_t>(partial_length) >= sizeof(target.partial)) {
    ++errors;
    aqlog.println("PARQUET ERROR operation=create reason=path-too-long");
    return false;
  }
  {
    BusLock lock;
    const int fd = ::open(target.partial, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
      target.file = ::fdopen(fd, "wb");
      if (!target.file)
        ::close(fd);
    }
  }
  if (!target.file) {
    ++errors;
    aqlog.printf("PARQUET ERROR operation=create errno=%d file=%s\n", errno,
                 target.stem);
    return false;
  }
  std::setvbuf(target.file, target.staging, _IOFBF, kStagingBytes);
  // The writer copies this configuration; the LZ4 buffers/state it points to
  // live in writer_state for the whole session.
  const auto compression = writer_state->lz4.configuration();
  const auto result = target.writer.begin(
      sink, target.file, target.workspace, writer_state->columns, field_count,
      codec == Codec::Lz4Raw ? &compression : nullptr);
  if (!result.ok) {
    close_failed(target, "begin", result.error);
    return false;
  }
  return true;
}

bool sync_file(OutputFile &target) {
  const auto started = esp_timer_get_time();
  bool ok;
  {
    BusLock lock;
    ok = std::fflush(target.file) == 0 && ::fsync(::fileno(target.file)) == 0;
  }
  target.sync_us += esp_timer_get_time() - started;
  sync_us = static_cast<std::uint32_t>(esp_timer_get_time() - started);
  if (ok)
    stamp_heartbeat(); // a completed fsync is progress
  return ok;
}

// Appends the RAM batch as one row group and makes it durable.
bool append_group(OutputFile &target, std::size_t count) {
  writer_state->lz4.codec_us = 0;
  const auto started = esp_timer_get_time();
  const auto result = target.writer.row_group(count, sequence);
  const auto elapsed = esp_timer_get_time() - started;
  target.writer_us += elapsed;
  target.codec_us += writer_state->lz4.codec_us;
  if (!result.ok) {
    close_failed(target, "row-group", result.error);
    return false;
  }
  // Encoding after the last sink write is progress too.
  stamp_heartbeat();
  if (!sync_file(target)) {
    close_failed(target, "row-group-sync", nullptr);
    return false;
  }
  target.last = writer_state->rows[count - 1].data[sequence];
  target.rows += count;
  if (!target.benchmark) {
    open_rows = target.rows;
    open_groups = target.writer.row_groups();
  }
  aqlog.printf("PARQUET GROUP file=%s ordinal=%u rows=%u first=%lld last=%lld "
               "bytes=%llu writer_us=%lld codec=%s codec_us=%llu\n",
               target.stem, unsigned(target.writer.row_groups() - 1),
               unsigned(count), static_cast<long long>(target.first),
               static_cast<long long>(target.last),
               static_cast<unsigned long long>(target.writer.bytes_written()),
               static_cast<long long>(elapsed), codec_name(target.codec),
               static_cast<unsigned long long>(writer_state->lz4.codec_us));
  return true;
}

// Writes the footer, syncs, closes, checks the structure and renames to
// `<stem>-<last>-<attempt>.parquet`.
bool finalize_file(OutputFile &target) {
  char interval_text[12], groups_text[12];
  char anchor_mono_text[24], anchor_utc_text[24], anchor_source_text[12];
  std::int64_t footer_anchor_mono, footer_anchor_utc;
  std::int32_t footer_anchor_source;
  portENTER_CRITICAL(&clock_mutex);
  footer_anchor_mono = anchor_mono_us;
  footer_anchor_utc = anchor_utc_ns;
  footer_anchor_source = clock_source;
  portEXIT_CRITICAL(&clock_mutex);
  std::snprintf(anchor_mono_text, sizeof(anchor_mono_text), "%lld",
                static_cast<long long>(footer_anchor_mono));
  std::snprintf(anchor_utc_text, sizeof(anchor_utc_text), "%lld",
                static_cast<long long>(footer_anchor_utc));
  std::snprintf(anchor_source_text, sizeof(anchor_source_text), "%ld",
                static_cast<long>(footer_anchor_source));
  std::snprintf(interval_text, sizeof(interval_text), "%lu",
                static_cast<unsigned long>(rotation_seconds.load()));
  std::snprintf(groups_text, sizeof(groups_text), "%u",
                unsigned(target.writer.row_groups()));
  const LocationMetadata location_metadata(target.location);
  KeyValue metadata[kFooterMetadata] = {
      {"schema_version", config.schema_name},
      {"device_id", device_text},
      {"station_id", station_text},
      {"boot_id", boot_text},
      {"firmware", config.firmware},
      {"dictionary_version", config.dictionary_version},
      {"dictionary_uri", config.dictionary_uri},
      {"dictionary_sha256", config.dictionary_sha256},
      {"acquisition_config_id", config.configuration_id},
      {"acquisition_config", config.configuration},
      {"deployment_id", "unknown"},
      {"calibration_id", "unknown"},
      {"time_semantics", config.time_semantics},
      {"rotation_interval_s", interval_text},
      {"row_groups", groups_text},
      {"row_group_rows_max", "90"},
      {"compression", codec_name(target.codec)},
      {"purpose",
       target.benchmark ? "codec-comparison-duplicate-rows" : "telemetry"},
      {"board", config.board},
      {"sample_interval_ms", "10000"},
      {"clock", "0=unsynchronized/null,1=host estimate,2=restored from RTC "
                "(earlier external estimate, whole seconds),3=network SNTP; "
                "partitions UTC"},
      {"durability",
       "RAM batch; unfinished rows lost on reset; each row group "
       "fsynced; footer at finalization; completed files retained"}};
  std::size_t metadata_count = kFixedMetadata;
  static_assert(sizeof(metadata) / sizeof(metadata[0]) == kFooterMetadata);
  for (const auto &entry : location_metadata.entries)
    metadata[metadata_count++] = entry;
  // Evidence belongs to this file's boot, regardless of the rows' original
  // clock status. Hosts may derive corrected partitions without editing raw
  // UTC or treating a later reboot's clock as an anchor for this boot.
  if (footer_anchor_source != kClockNone) {
    metadata[metadata_count++] = {"reconciliation_anchor_mono_us",
                                  anchor_mono_text};
    metadata[metadata_count++] = {"reconciliation_anchor_utc_ns",
                                  anchor_utc_text};
    metadata[metadata_count++] = {"reconciliation_anchor_source",
                                  anchor_source_text};
  }
  for (std::size_t i = 0; i < config.metadata_count; ++i)
    metadata[metadata_count++] = config.metadata[i];
  const auto started = esp_timer_get_time();
  const auto result = target.writer.finish(metadata, metadata_count,
                                           config.firmware, config.created_by);
  target.writer_us += esp_timer_get_time() - started;
  if (!result.ok) {
    close_failed(target, "finish", result.error);
    return false;
  }
  bool ok = sync_file(target);
  {
    BusLock lock;
    ok = std::fclose(target.file) == 0 && ok;
  }
  target.file = nullptr;
  if (!target.benchmark)
    open_rows = open_groups = 0;
  char ready[416];
  std::snprintf(ready, sizeof(ready), "%s/%s-%lld-%lu.parquet", target.root,
                target.stem, static_cast<long long>(target.last),
                static_cast<unsigned long>(target.attempt));
  std::uint32_t size = 0, crc = 0;
  if (ok)
    ok = finalized_file(target.partial, size, crc) &&
         size == result.bytes_written;
  if (ok) {
    BusLock lock;
    ok = ::access(ready, F_OK) != 0 && ::rename(target.partial, ready) == 0;
    used_kib =
        (hooks.storage_used_bytes ? hooks.storage_used_bytes(hooks.context)
                                  : 0) /
        1024;
  }
  write_us =
      static_cast<std::uint32_t>(esp_timer_get_time() - target.opened_us);
  if (!ok) {
    close_failed(target, "finalize", "io-or-validation");
    return false;
  }
  ++finalized;
  unlisted_files = true;
  if (!target.benchmark)
    remember_crc(ready, size, crc);
  aqlog.printf(
      "PARQUET READY name=%s-%lld-%lu.parquet rows=%u row_groups=%u "
      "first=%lld last=%lld bytes=%lu crc32=%08lx write_us=%lu sync_us=%llu "
      "heap_free=%lu psram_free=%lu stack_free=%u codec=%s codec_us=%llu "
      "writer_us=%llu codec_workspace_bytes=%u heap_min=%lu "
      "dma_free=%lu dma_largest=%lu dma_min=%lu\n",
      target.stem, static_cast<long long>(target.last),
      static_cast<unsigned long>(target.attempt), unsigned(target.rows),
      unsigned(target.writer.row_groups()),
      static_cast<long long>(target.first), static_cast<long long>(target.last),
      static_cast<unsigned long>(size), static_cast<unsigned long>(crc),
      static_cast<unsigned long>(write_us.load()),
      static_cast<unsigned long long>(target.sync_us),
      static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
      static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
      unsigned(uxTaskGetStackHighWaterMark(nullptr)), codec_name(target.codec),
      static_cast<unsigned long long>(target.codec_us),
      static_cast<unsigned long long>(target.writer_us),
      unsigned(sizeof(Lz4Workspace)),
      static_cast<unsigned long>(
          heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
      static_cast<unsigned long>(
          heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)),
      static_cast<unsigned long>(heap_caps_get_largest_free_block(
          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)),
      static_cast<unsigned long>(heap_caps_get_minimum_free_size(
          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)));
  return true;
}

// Appends the RAM batch (if any) to the telemetry file, opening it when
// needed, and finalizes the file when `finalize` is set or the file is full.
// On success `count` is zero. Failure leaves the .partial and returns false.
bool commit(std::size_t &count, bool finalize) {
  OutputFile &target = writer_state->telemetry;
  if (count) {
    if (!target.open() && !create_file(target, selected_codec, false))
      return false;
    if (!append_group(target, count))
      return false;
    count = 0;
    buffered = 0;
  }
  if (target.open() &&
      finalize_after_group(finalize, target.writer.row_groups(), kMaxRowGroups))
    return finalize_file(target);
  return true;
}

// A one-row-group diagnostic copy of the RAM batch outside station trees;
// the rows stay buffered for normal telemetry rotation.
bool write_benchmark(std::size_t count, Codec codec) {
  if (!writer_state->benchmark)
    return false;
  OutputFile &target = *writer_state->benchmark;
  if (!count || target.open())
    return false;
  return create_file(target, codec, true) && append_group(target, count) &&
         finalize_file(target);
}

bool safe_name(const char *name) { return aq::sync::safe_parquet_name(name); }

// One finalized-file entry from a listing. `name` already carries the export
// prefix ("legacy-parquet/" for the pre-Hive directory).
using FileEmitter = void (*)(void *context, const char *name,
                             std::uint32_t bytes);

void emit_file_serial(void *, const char *name, std::uint32_t bytes) {
  aqlog.printf("PARQUET FILE name=%s bytes=%lu\n", name,
               static_cast<unsigned long>(bytes));
}

bool list_directory(unsigned &partials, FileEmitter emit, void *context,
                    const char *base = kDirectory,
                    const char *export_prefix = "") {
  ArchiveDirectoryIO io;
  auto visit = [&](const char *name, const struct stat &info) {
    if (S_ISREG(info.st_mode) && safe_name(name)) {
      if (info.st_size < 0 ||
          static_cast<std::uint64_t>(info.st_size) > UINT32_MAX) {
        errno = EOVERFLOW;
        return false;
      }
      char exported[400];
      const int length = std::snprintf(exported, sizeof(exported), "%s%s",
                                       export_prefix, name);
      if (length < 0 || static_cast<std::size_t>(length) >= sizeof(exported)) {
        errno = ENAMETOOLONG;
        return false;
      }
      emit(context, exported, static_cast<std::uint32_t>(info.st_size));
    } else if (std::strstr(name, ".partial") && partials != UINT32_MAX) {
      ++partials;
    }
    return true;
  };
  return aq::logger::detail::walk_directory(io, base, "", 0, visit);
}

bool partial_name(const char *name) {
  const std::size_t length = std::strlen(name);
  return length > 8 && std::strcmp(name + length - 8, ".partial") == 0;
}

// Move interrupted files out of active station trees without deleting or
// pretending to repair them. Each lands under output/quarantine/ in a
// directory that keeps its station and partition provenance.
void quarantine_directory(const char *relative, unsigned depth,
                          unsigned &quarantined) {
  if (depth > 6)
    return;
  char directory_path[416];
  std::snprintf(directory_path, sizeof(directory_path), "%s/%s", kDirectory,
                relative);
  DIR *directory;
  {
    BusLock lock;
    directory = ::opendir(directory_path);
  }
  if (!directory)
    return;
  for (;;) {
    stamp_heartbeat();
    const struct dirent *entry;
    {
      BusLock lock;
      entry = ::readdir(directory);
    }
    if (!entry)
      break;
    if (entry->d_name[0] == '.')
      continue;
    char name[384], path[416];
    const int length = std::snprintf(name, sizeof(name), "%s%s%s", relative,
                                     *relative ? "/" : "", entry->d_name);
    if (length < 0 || std::size_t(length) >= sizeof(name) ||
        std::strncmp(name, "quarantine/", 11) == 0)
      continue;
    std::snprintf(path, sizeof(path), "%s/%s", kDirectory, name);
    struct stat info{};
    {
      BusLock lock;
      if (::stat(path, &info) != 0)
        continue;
    }
    if (S_ISDIR(info.st_mode)) {
      quarantine_directory(name, depth + 1, quarantined);
      continue;
    }
    if (!partial_name(name))
      continue;
    if (quarantine_one(kDirectory, name, "boot"))
      ++quarantined;
  }
  {
    BusLock lock;
    ::closedir(directory);
  }
}

// Recursive, so provenance subdirectories and the flat pre-#5 layout count.
void count_quarantine(unsigned &count, std::uint64_t &bytes) {
  char root[64];
  std::snprintf(root, sizeof(root), "%s/quarantine", kDirectory);
  ArchiveDirectoryIO io;
  auto visit = [&](const char *name, const struct stat &info) {
    if (!S_ISREG(info.st_mode) || !partial_name(name))
      return true;
    if (count != UINT32_MAX)
      ++count;
    const std::uint64_t file_bytes = static_cast<std::uint64_t>(info.st_size);
    bytes = UINT64_MAX - bytes < file_bytes ? UINT64_MAX : bytes + file_bytes;
    return true;
  };
  if (!aq::logger::detail::walk_directory(io, root, "", 0, visit) &&
      errno != ENOENT) {
    ++errors;
    aqlog.printf("PARQUET ERROR operation=quarantine-count errno=%d\n", errno);
  }
}

unsigned quarantine_partials() {
  char directory[64];
  std::snprintf(directory, sizeof(directory), "%s/quarantine", kDirectory);
  if (!make_directories(directory)) {
    ++errors;
    aqlog.printf("PARQUET ERROR operation=quarantine-mkdir errno=%d\n", errno);
    return 0;
  }
  unsigned quarantined = 0;
  quarantine_directory("", 0, quarantined);
  unsigned total = 0;
  std::uint64_t bytes = 0;
  count_quarantine(total, bytes);
  partials_quarantined = total;
  quarantine_bytes = bytes;
  return quarantined;
}

// Walk every configured archive tree; only a complete scan replaces counters.
aqsync::ListResult list_all(FileEmitter emit, void *context) {
  aqsync::ListResult result;
  result.complete = list_directory(result.partials, emit, context);
  if (result.complete && config.legacy_directory) {
    struct stat legacy{};
    int stat_result;
    int stat_error;
    {
      BusLock lock;
      stat_result = ::stat(config.legacy_directory, &legacy);
      stat_error = errno;
    }
    if (stat_result == 0) {
      if (S_ISDIR(legacy.st_mode)) {
        result.complete =
            list_directory(result.partials, emit, context,
                           config.legacy_directory, "legacy-parquet/");
      } else {
        errno = ENOTDIR;
        result.complete = false;
      }
    } else if (stat_error != ENOENT) {
      errno = stat_error;
      result.complete = false;
    }
  }
  if (result.complete) {
    partials_seen = result.partials;
  } else {
    ++errors;
    aqlog.printf("PARQUET ERROR operation=list errno=%d\n", errno);
  }
  return result;
}

void list_files(bool emit_serial = true) {
  std::uint32_t count = 0;
  const FileEmitter count_file = [](void *context, const char *,
                                    std::uint32_t) {
    auto &files = *static_cast<std::uint32_t *>(context);
    if (files != UINT32_MAX)
      ++files;
  };
  const aqsync::ListResult result =
      list_all(emit_serial ? emit_file_serial : count_file,
               emit_serial ? nullptr : &count);
  if (!result.complete)
    return;
  const unsigned partials = result.partials;
  if (!emit_serial) {
    // Startup must not wait for a USB reader to drain an archive-sized dump.
    // Enumeration/stats still run on this worker with the same bus ownership.
    aqlog.printf("PARQUET SCAN files=%lu retained_partials=%u "
                 "quarantine_files=%lu quarantine_bytes=%llu\n",
                 static_cast<unsigned long>(count), partials,
                 static_cast<unsigned long>(partials_quarantined.load()),
                 static_cast<unsigned long long>(quarantine_bytes.load()));
    return;
  }
  aqlog.printf("PARQUET PARTIAL retained=%u quarantine_files=%lu "
               "quarantine_bytes=%llu recovery=host-only\n",
               partials,
               static_cast<unsigned long>(partials_quarantined.load()),
               static_cast<unsigned long long>(quarantine_bytes.load()));
  aqlog.println("PARQUET LIST END");
}

bool resolve_path(const char *name, char *path, std::size_t size) {
  if (!safe_name(name))
    return false;
  const int written =
      std::strncmp(name, "legacy-parquet/", 15) == 0
          ? (config.legacy_directory
                 ? std::snprintf(path, size, "%s/%.369s",
                                 config.legacy_directory, name + 15)
                 : -1)
          : std::snprintf(path, size, "%s/%.384s", kDirectory, name);
  return written >= 0 && static_cast<std::size_t>(written) < size;
}

void send_file(const char *name) {
  char path[416];
  if (!resolve_path(name, path, sizeof(path))) {
    aqlog.println("PARQUET ERROR operation=get reason=invalid-name");
    return;
  }
  FILE *file;
  {
    BusLock lock;
    file = std::fopen(path, "rb");
  }
  if (!file) {
    aqlog.println("PARQUET ERROR operation=get reason=open");
    return;
  }
  aqsync::FileCheck check;
  bool rewound = check_file(path, file, check, true);
  if (rewound) {
    BusLock lock;
    rewound = std::fseek(file, 0, SEEK_SET) == 0;
  }
  if (!rewound) {
    {
      BusLock lock;
      std::fclose(file);
    }
    aqlog.println("PARQUET ERROR operation=get reason=invalid-file");
    return;
  }
  const std::uint32_t size = check.size, expected_crc = check.crc;
  aqlog.printf("PARQUET DATA BEGIN name=%s bytes=%lu\n", name,
               static_cast<unsigned long>(size));
  std::uint8_t block[256];
  char hex[513];
  constexpr char digits[] = "0123456789abcdef";
  std::uint32_t offset = 0;
  while (offset < size) {
    std::size_t count;
    {
      BusLock lock;
      count = std::fread(block, 1, sizeof(block), file);
    }
    if (!count)
      break;
    for (std::size_t i = 0; i < count; ++i) {
      hex[2 * i] = digits[block[i] >> 4];
      hex[2 * i + 1] = digits[block[i] & 15];
    }
    hex[2 * count] = '\0';
    aqlog.printf("PARQUET DATA offset=%lu hex=%s\n",
                 static_cast<unsigned long>(offset), hex);
    offset += count;
    // A 1 MiB file is about 3 minutes of hex at 115200 baud, longer than the
    // stall timeout. Each sent block is progress, not a stall.
    stamp_heartbeat();
    vTaskDelay(pdMS_TO_TICKS(1) > 0 ? pdMS_TO_TICKS(1) : 1);
  }
  {
    BusLock lock;
    std::fclose(file);
  }
  aqlog.printf("PARQUET DATA END name=%s bytes=%lu crc32=%08lx\n", name,
               static_cast<unsigned long>(offset),
               static_cast<unsigned long>(expected_crc));
}

const char *clock_source_name(std::int32_t source) {
  return aq::utc::source_name(source);
}

// Shared by `parquet time`, the BLE/LAN SET_TIME op and the boot-time RTC
// seed. False for a bad epoch (before 2020 or after 2100, which also rejects
// an RTC that was never written). Initial UTC, RTC upgrades and corrections
// larger than two seconds start a new epoch; small refreshes keep the current
// file. Captured rows keep their timestamps and exact anchors. `skew_ns`
// receives new minus old estimate of the same monotonic instant, i.e. how far
// the previous clock (RTC or earlier host) had drifted from this host.
bool set_clock(std::int64_t seconds, std::int64_t mono, std::int32_t source,
               std::int64_t *skew_ns = nullptr,
               std::int32_t *previous = nullptr,
               std::uint32_t subsecond_us = 0) {
  if (!aq::utc::supported_anchor(seconds, subsecond_us))
    return false;
  const std::int64_t utc_ns = aq::utc::anchor_ns(seconds, subsecond_us);
  portENTER_CRITICAL(&clock_mutex);
  const auto previous_source = clock_generation ? clock_source : kClockNone;
  const auto skew =
      clock_generation
          ? utc_ns - aq::utc::estimate_ns(mono, anchor_mono_us, anchor_utc_ns)
          : 0;
  if (previous)
    *previous = previous_source;
  if (skew_ns)
    *skew_ns = skew;
  anchor_mono_us = mono;
  anchor_utc_ns = utc_ns;
  clock_source = source;
  if (aq::utc::starts_new_epoch(previous_source, source, skew))
    ++clock_generation;
  portEXIT_CRITICAL(&clock_mutex);
  if (source == kClockHost || source == aq::utc::Network) {
    rtc_write_requested_us = mono;
    rtc_write_pending = true;
  }
  return true;
}

// Reports a host sync on serial: the anchor line the host tools parse, then
// the drift of whatever clock was in use before (RTC-restored or an earlier
// host value), which is the only clock-accuracy evidence this device has.
void report_host_clock(const char *transport, std::int64_t seconds,
                       std::int64_t mono, std::int32_t previous,
                       std::int64_t skew_ns) {
  aqlog.printf("PARQUET TIME epoch_s=%lld source=host monotonic_us=%lld\n",
               static_cast<long long>(seconds), static_cast<long long>(mono));
  if (previous != kClockNone)
    aqlog.printf("PARQUET CLOCK transport=%s previous=%s skew_ms=%lld\n",
                 transport, clock_source_name(previous),
                 static_cast<long long>(skew_ns / 1000000));
}

// The board RTC hook rejects invalid or unset data. RTC values are UTC,
// because this runtime only writes anchors previously supplied by the host.
void seed_clock_from_rtc() {
  std::int64_t seconds = 0;
  const bool read_ok = hooks.rtc_read && hooks.rtc_read(seconds, hooks.context);
  const auto mono = esp_timer_get_time();
  if (!read_ok || !set_clock(seconds, mono, kClockRtc)) {
    rtc_state = 2;
    aqlog.println("PARQUET RTC state=unusable-or-absent");
    return;
  }
  rtc_state = 1;
  aqlog.printf("PARQUET TIME epoch_s=%lld source=rtc monotonic_us=%lld\n",
               static_cast<long long>(seconds), static_cast<long long>(mono));
}

// Main-task half of a host sync: copies the anchor into the RTC. The chip
// stores whole seconds, so the write waits for the anchor's next second
// boundary (the 20 ms loop makes a 60 ms window reliable) and gives up
// waiting after 5 s so a slow loop cannot postpone it forever.
void service_rtc_write(std::int64_t now) {
  if (!rtc_write_pending.load())
    return;
  std::int64_t utc_anchor, mono_anchor;
  portENTER_CRITICAL(&clock_mutex);
  utc_anchor = anchor_utc_ns;
  mono_anchor = anchor_mono_us;
  portEXIT_CRITICAL(&clock_mutex);
  const std::int64_t utc_ns =
      aq::utc::estimate_ns(now, mono_anchor, utc_anchor);
  const std::int64_t fraction_ns = utc_ns % 1000000000;
  const bool overdue = now - rtc_write_requested_us.load() > 5000000LL;
  if (fraction_ns >= 60000000LL && !overdue)
    return;
  rtc_write_pending = false;
  if (!hooks.rtc_write) {
    aqlog.println("PARQUET RTC write=skipped reason=absent");
    return;
  }
  const auto seconds = (utc_ns + 500000000) / 1000000000;
  const bool ok = hooks.rtc_write(seconds, hooks.context);
  rtc_state = ok ? 1 : 2;
  aqlog.printf("PARQUET RTC write=%s epoch_s=%lld late_ms=%lld%s\n",
               ok ? "ok" : "readback-mismatch", static_cast<long long>(seconds),
               static_cast<long long>(fraction_ns / 1000000),
               overdue ? " overdue=true" : "");
}

// ---- BLE `status` document and control handlers
// (docs/shared/ble-sync-protocol.md)

std::size_t build_status_json(char *out, std::size_t size) {
  std::int32_t generation, source;
  std::int64_t now, mono_anchor, utc_anchor;
  portENTER_CRITICAL(&clock_mutex);
  now = esp_timer_get_time();
  generation = clock_generation;
  source = clock_source;
  mono_anchor = anchor_mono_us;
  utc_anchor = anchor_utc_ns;
  portEXIT_CRITICAL(&clock_mutex);
  StatusSnapshot view;
  view.uptime_seconds = static_cast<std::uint32_t>(now / 1000000);
  view.interval_seconds = rotation_seconds.load();
  view.buffered = buffered.load();
  view.finalized = finalized.load();
  view.dropped = dropped.load();
  view.errors = errors.load();
  view.missed = missed_deadlines.load();
  view.failed = worker_failed.load();
  view.codec = selected_codec.load();
  view.utc = generation != 0;
  view.generation = generation;
  view.clock_source =
      static_cast<std::uint8_t>(generation ? source : kClockNone);
  view.rtc_state = static_cast<std::uint8_t>(rtc_state.load());
  if (generation) {
    view.utc_ms = aq::utc::estimate_ns(now, mono_anchor, utc_anchor) / 1000000;
    view.clock_age_seconds = (now - mono_anchor) / 1000000;
  }
  view.storage_ok = storage_ok.load();
  view.sd_state = static_cast<storage::State>(sd_state.load());
  view.total_kib = total_kib.load();
  view.used_kib = used_kib.load();
  view.heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  view.partials = partials_seen.load();
  view.quarantined = partials_quarantined.load();
  view.quarantine_bytes = quarantine_bytes.load();
  view.open_rows = open_rows.load();
  view.open_groups = open_groups.load();
  const auto disconnects = ble::disconnect_stats();
  view.disconnects = static_cast<std::uint16_t>(
      std::min<std::uint32_t>(disconnects.count, UINT16_MAX));
  view.last_disconnect = static_cast<std::uint8_t>(disconnects.last);
  return format_status(view, out, size);
}

// The ten advertising bytes are derived from the same state as `status`; the
// BLE side only issues an HCI command when flags or the counter changed.
void publish_advert() {
  std::int32_t generation, source;
  portENTER_CRITICAL(&clock_mutex);
  generation = clock_generation;
  source = clock_source;
  portEXIT_CRITICAL(&clock_mutex);
  ble::AdvertState advert;
  advert.finalized = finalized.load();
  if (!generation)
    advert.flags |= ble::kAdvNoUtc;
  else if (source == kClockRtc)
    advert.flags |= ble::kAdvClockRestored;
  if (unlisted_files.load())
    advert.flags |= ble::kAdvNewFiles;
  if (storage_ok.load())
    advert.flags |= ble::kAdvSd;
  if (lan::status().mdns)
    advert.flags |= ble::kAdvLan;
  if (worker_failed.load())
    advert.flags |= ble::kAdvFail;
  ble::publish_advert(advert);
}

void publish_status() {
  char json[ble::kMaxJson + 16];
  const std::size_t length = build_status_json(json, sizeof(json));
  if (length && length <= ble::kMaxJson) {
    ble::publish_status(json, length);
    lan::publish_status(json, length);
  }
  publish_advert();
}

// Publishes the `live` snapshot of the row that was just queued. The board's
// field table decides which keys appear; see aq_logger_live.h.
void publish_live(const Sample &row) {
  char json[ble::kMaxJson + 1];
  const auto used =
      format_live(config.fields, field_count, row, json, sizeof(json));
  if (used && used <= ble::kMaxJson) {
    ble::publish_live(json, used);
    lan::publish_live(json, used);
  }
}

// The request being executed; handlers answer on its link, peer and generation.
const ble::ControlRequest *current_request = nullptr;

std::uint32_t link_generation(ble::Link link, std::uint8_t peer = 0) {
  return link == ble::Link::Lan ? lan::connection_generation(peer)
                                : ble::connection_generation();
}
std::uint16_t link_payload_max() {
  if (current_request && current_request->link == ble::Link::Lan)
    return lan::payload_max(current_request->peer);
  // GATT attribute values are at most 512 bytes; Android's stack silently
  // discards larger notifications while macOS accepts them (measured
  // 2026-09-17: every 514-byte CHUNK vanished on a OnePlus, the short final
  // chunk arrived). So a BLE frame never exceeds 512 even at MTU 517.
  const std::uint16_t raw = ble::payload_max();
  return raw > ble::kMaxFrame ? ble::kMaxFrame : raw;
}
void service_samples_while_waiting();
// Between ring-space waits: the worker is alive and keeps taking samples. A
// data frame stops waiting once an ABORT arrived; a terminal ERROR does not,
// and the slow peer rule bounds that wait.
bool error_wait_step(void *) {
  stamp_heartbeat();
  service_samples_while_waiting();
  return true;
}
bool data_wait_step(void *context) {
  error_wait_step(context);
  return !(current_request && aqsync::abort::pending(*current_request));
}
bool lan_deliver(const std::uint8_t *frame, std::size_t length,
                 std::uint8_t peer, std::uint32_t generation) {
  return lan::deliver(frame, length, peer, generation, data_wait_step, nullptr);
}
bool lan_error(ble::Op op, ble::Error code, const char *detail,
               std::uint8_t peer, std::uint32_t generation) {
  std::uint8_t frame[lan::kErrorFrameBytes]{};
  const std::size_t length = lan::encode_error(frame, op, code, detail);
  return lan::deliver(frame, length, peer, generation, error_wait_step,
                      nullptr);
}
bool respond(const std::uint8_t *frame, std::size_t length) {
  // A long LIST or READ is progress, not a stall: stamp the heartbeat per
  // frame so the stall detector only fires when sending truly stops.
  stamp_heartbeat();
  return current_request && current_request->link == ble::Link::Lan
             ? lan_deliver(frame, length, current_request->peer,
                           current_request->link_generation)
             : ble::deliver(frame, length,
                            current_request ? current_request->link_generation
                                            : 0,
                            data_wait_step, nullptr);
}
bool respond_error(ble::Op op, ble::Error code, const char *detail) {
  return current_request && current_request->link == ble::Link::Lan
             ? lan_error(op, code, detail, current_request->peer,
                         current_request->link_generation)
             : ble::deliver_error(
                   op, code, detail,
                   current_request ? current_request->link_generation : 0,
                   error_wait_step, nullptr);
}
// Archive policy stays with the board worker; file-transfer sessions and wire
// frames are shared across boards and BLE/LAN transports.
aqsync::ArchiveSession archive_session(
    {nullptr,
     [](void *, aqsync::FileEmitter emit, void *context) {
       return list_all(emit, context);
     },
     [](void *, const char *name, char *path, std::size_t size) {
       return resolve_path(name, path, size);
     },
     [](void *, const char *path, FILE *file, aqsync::FileCheck &check) {
       return check_file(path, file, check, true);
     },
     [](void *) { return total_kib.load(); },
     [](void *) { return used_kib.load(); },
     [](void *) {
       unlisted_files = false;
       publish_advert();
     },
     [](void *) { lock_bus(); }, [](void *) { unlock_bus(); },
     [](void *, const char *line) { aqlog.record_only().print(line); },
     [](void *, std::size_t slot) -> void * {
       return writer_state->read_buffers[slot];
     },
     [](void *) { return std::int64_t(esp_timer_get_time()); }},
    {nullptr,
     [](void *, ble::Link link, std::uint8_t peer) {
       return link_generation(link, peer);
     },
     [](void *, ble::Link link, std::uint8_t peer) {
       return link == ble::Link::Lan ? lan::payload_max(peer)
                                     : ble::payload_max();
     },
     [](void *, ble::Link link, std::uint8_t peer, std::uint32_t generation,
        const std::uint8_t *frame, std::size_t length) {
       return link == ble::Link::Lan
                  ? lan_deliver(frame, length, peer, generation)
                  : ble::deliver(frame, length, generation, data_wait_step,
                                 nullptr);
     },
     [](void *, ble::Link link, std::uint8_t peer, std::uint32_t generation,
        ble::Op op, ble::Error code, const char *detail) {
       return link == ble::Link::Lan
                  ? lan_error(op, code, detail, peer, generation)
                  : ble::deliver_error(op, code, detail, generation,
                                       error_wait_step, nullptr);
     },
     [](void *) { stamp_heartbeat(); }});

const aqsync::ControlReplies control_replies{
    nullptr, [](void *, ble::Link) { return link_payload_max(); },
    [](void *, ble::Link, const std::uint8_t *frame, std::size_t length) {
      return respond(frame, length);
    },
    [](void *, ble::Link, ble::Op op, ble::Error code, const char *detail) {
      return respond_error(op, code, detail);
    }};

std::int64_t uptime_seconds() { return esp_timer_get_time() / 1000000; }

// Fresh free-space reading. False when the volume cannot answer.
// A first free-cluster count can scan the whole FAT, so it is stamped on both
// sides and bounded only by the watchdog timeout.
bool measure_free(std::uint64_t &free_bytes) {
  if (!hooks.storage_free_bytes)
    return false;
  stamp_heartbeat();
  bool ok;
  {
    BusLock lock;
    ok = hooks.storage_free_bytes(free_bytes, hooks.context);
  }
  stamp_heartbeat();
  return ok;
}

// Capacity and allocation unit right after a mount. Configures the governor
// thresholds and returns false when the volume cannot report free space.
bool measure_volume() {
  std::uint64_t total = 0, used = 0;
  std::uint32_t cluster = 0;
  stamp_heartbeat();
  {
    BusLock lock;
    total = hooks.storage_total_bytes ? hooks.storage_total_bytes(hooks.context)
                                      : 0;
    used =
        hooks.storage_used_bytes ? hooks.storage_used_bytes(hooks.context) : 0;
    cluster = hooks.storage_cluster_bytes
                  ? hooks.storage_cluster_bytes(hooks.context)
                  : 0;
  }
  stamp_heartbeat();
  total_kib = static_cast<std::uint32_t>(total / 1024);
  used_kib = static_cast<std::uint32_t>(used / 1024);
  std::uint64_t free_bytes = 0;
  const bool ok = total != 0 && measure_free(free_bytes);
  aqlog.printf(
      "PARQUET STORAGE mount cluster_bytes=%lu total_kib=%llu "
      "free_kib=%llu reserve_kib=%llu high_kib=%llu\n",
      static_cast<unsigned long>(cluster),
      static_cast<unsigned long long>(total / 1024),
      static_cast<unsigned long long>(free_bytes / 1024),
      static_cast<unsigned long long>(storage::thresholds(total).reserve /
                                      1024),
      static_cast<unsigned long long>(storage::thresholds(total).high / 1024));
  governor.configure(total);
  if (ok)
    governor.observe_free(free_bytes, uptime_seconds());
  return ok;
}

// Mirrors the governor into STATUS and the log. Entering Full finalizes the
// open telemetry file so its rows are readable while logging is paused; the
// footer is a few KiB and comes out of the hard reserve.
void publish_storage(storage::State previous) {
  auto current = governor.state();
  if (current == storage::State::Full && previous != current &&
      writer_state->telemetry.open() &&
      !finalize_file(writer_state->telemetry)) {
    governor.write_failed(uptime_seconds());
    current = governor.state();
  }
  sd_state = static_cast<std::uint8_t>(current);
  worker_failed = storage::alarm(current); // fail means "not writing"
  if (current == previous)
    return;
  const auto free_kib =
      static_cast<unsigned long long>(governor.free_bytes() / 1024);
  aqlog.printf("PARQUET STORAGE state=%s previous=%s free_kib=%llu "
               "retry_attempts=%lu\n",
               storage::state_name(current), storage::state_name(previous),
               free_kib, static_cast<unsigned long>(governor.retry_attempts()));
  if (current == storage::State::Full)
    aqlog.printf("PARQUET ALARM storage=full free_kib=%llu action=paused "
                 "deleted=0\n",
                 free_kib);
  publish_status();
}

// Every telemetry write passes here so the governor sees each result. A
// failure never retries in place; the probe schedule owns recovery.
bool governed_commit(std::size_t &count, bool finalize) {
  const auto previous = governor.state();
  bool ok = commit(count, finalize);
  std::uint64_t free_bytes = 0;
  if (ok && measure_free(free_bytes)) {
    governor.observe_free(free_bytes, uptime_seconds());
  } else {
    ok = false;
    governor.write_failed(uptime_seconds());
  }
  publish_storage(previous);
  return ok;
}

// The codec-test file and its stdio staging are borrowed from PSRAM only for
// that command, so the resident workspace carries no benchmark OutputFile.
bool alloc_benchmark() {
  constexpr auto caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  void *file = heap_caps_calloc(1, sizeof(OutputFile), caps);
  auto *staging = static_cast<char *>(heap_caps_malloc(kStagingBytes, caps));
  if (!file || !staging) {
    heap_caps_free(file);
    heap_caps_free(staging);
    return false;
  }
  writer_state->benchmark = new (file) OutputFile{};
  writer_state->benchmark->staging = staging;
  return true;
}

void free_benchmark() {
  OutputFile *target = writer_state->benchmark;
  if (!target)
    return;
  writer_state->benchmark = nullptr;
  // Normally already closed by finalize_file or close_failed. A leftover
  // handle is closed under the bus lock like every other SD call.
  if (target->file) {
    BusLock lock;
    std::fclose(target->file);
  }
  heap_caps_free(target->staging);
  target->~OutputFile();
  heap_caps_free(target);
}

// No file may be open across a remount. probe_storage releases the archive
// session's read streams; their handles reopen by path on the next READ.
bool files_closed() {
  return !writer_state->telemetry.open() &&
         (!writer_state->benchmark || !writer_state->benchmark->open());
}

// Remount (when the board allows it) and re-measure. Clears Retry only when
// the card answers and, with rows waiting, a write succeeds again.
void probe_storage(std::size_t &count, bool &storage_ready) {
  const auto previous = governor.state();
  const auto now = uptime_seconds();
  bool mounted_again = false;
  bool ok = storage_ready || hooks.storage_remount;
  // An open file is finalized (or closed and quarantined by close_failed on
  // error) so no FILE* outlives the mount.
  if (ok && hooks.storage_remount && writer_state->telemetry.open())
    finalize_file(writer_state->telemetry);
  if (ok && hooks.storage_remount && files_closed()) {
    // A remount may follow an external card edit, so cached CRCs go too.
    archive_session.remounted();
    writer_state->crc_cache.clear();
    stamp_heartbeat();
    {
      BusLock lock;
      ok = hooks.storage_remount(hooks.context) &&
           aq::logger::detail::ensure_directory(kDirectory);
    }
    stamp_heartbeat();
    mounted_again = ok;
  }
  std::uint64_t free_bytes = 0;
  ok = ok && (!mounted_again || measure_volume()) && measure_free(free_bytes);
  if (ok && !storage_ready) {
    storage_ready = true;
    storage_ok = true;
  }
  // Partials left by the pulled card move before any new file is created,
  // so the probe write below can never quarantine its own open file.
  if (ok && mounted_again &&
      storage::remount_scans_partials(previous, free_bytes,
                                      governor.limits())) {
    const unsigned quarantined = quarantine_partials();
    aqlog.printf("PARQUET RECOVERY quarantined=%u repaired=0 deleted=0 "
                 "origin=remount\n",
                 quarantined);
  }
  // Waiting rows are the real write test. Committed while still in Retry so
  // a card that mounts but cannot write keeps its growing backoff.
  if (ok && count && free_bytes >= governor.limits().reserve)
    ok = commit(count, false) && measure_free(free_bytes);
  if (ok)
    governor.probe_succeeded(free_bytes, now);
  else
    governor.probe_failed(now);
  publish_storage(previous);
}

// Runs due probes and the 60 s free-space recheck while not Ready. A Full
// card is remounted so space freed on a PC becomes visible.
void service_storage(std::size_t &count, bool &storage_ready) {
  const auto now = uptime_seconds();
  if (governor.probe_due(now) && (storage_ready || hooks.storage_remount)) {
    probe_storage(count, storage_ready);
    return;
  }
  if (!governor.recheck_due(now))
    return;
  const auto previous = governor.state();
  if (previous == storage::State::Full && hooks.storage_remount &&
      files_closed()) {
    probe_storage(count, storage_ready);
    return;
  }
  std::uint64_t free_bytes = 0;
  if (measure_free(free_bytes))
    governor.observe_free(free_bytes, now);
  else
    governor.write_failed(now);
  publish_storage(previous);
}

// Explicit owner actions retry at once instead of waiting for the backoff.
bool storage_writable(std::size_t &count, bool &storage_ready) {
  governor.request_probe();
  service_storage(count, storage_ready);
  return storage_ready && governor.writable();
}

struct WorkerState {
  std::size_t &count;
  bool &storage_ready;
};
// Set while the worker executes a control request.
WorkerState *active_state = nullptr;

// Never mix clock epochs or UTC windows (including midnight) in a file: the
// open file, or else the RAM batch, is the reference.
bool fits_batch(const Sample &row, std::size_t count) {
  const OutputFile &open = writer_state->telemetry;
  if (open.open())
    return same_window(row, open.epoch, open.dated, open.window) &&
           config::same_location(row.location, open.location);
  if (!count)
    return true;
  const auto &previous = writer_state->rows[0];
  return same_window(row, previous.data[clock_epoch],
                     previous.valid[event_time_utc_us] != 0,
                     window_index(previous)) &&
         config::same_location(row.location, previous.location);
}

// While a LAN reply waits for ring space, move queued samples into the RAM
// batch so the sample queue does not overflow. Only appends that need no card
// write happen here: the bus may be held by the transfer, so anything else
// stays queued for the main loop.
void service_samples_while_waiting() {
  // Mirrors the main loop: without storage or a remount hook rows are dropped
  // there, and a row the batch cannot take needs a write.
  if (!active_state || (!active_state->storage_ready && !hooks.storage_remount))
    return;
  static Sample row;
  auto &count = active_state->count;
  while (xQueuePeek(samples, &row, 0) == pdTRUE) {
    if (count + 1 >= rows_per_group() || count + 1 >= kMaxRows ||
        !fits_batch(row, count))
      return;
    xQueueReceive(samples, &row, 0);
    writer_state->rows[count++] = row;
    buffered = count;
  }
}

void handle_control_request(const ble::ControlRequest &request,
                            WorkerState &state) {
  archive_session.reconcile();
  if (!request.length || request.length > sizeof(request.bytes) ||
      (request.link == ble::Link::Lan && request.peer >= ble::kMaxLanClients) ||
      (request.link == ble::Link::Ble && request.peer != 0) ||
      request.link_generation != link_generation(request.link, request.peer))
    return;
  if (archive_session.handle(request, state.storage_ready) ||
      aqsync::handle_common_control(request, control_replies))
    return;
  const std::size_t body_length = request.length - 1;
  switch (request.bytes[0]) {
  case ble::kOpSetTime: {
    aq::sync::TimeRequest time;
    const auto decoded =
        aq::sync::decode_time_request(request.bytes + 1, body_length, time);
    if (decoded != aq::sync::TimeRequestResult::Ok) {
      respond_error(ble::kOpSetTime,
                    decoded == aq::sync::TimeRequestResult::Malformed
                        ? ble::kErrMalformed
                        : ble::kErrInvalidEpoch,
                    nullptr);
      return;
    }
    const auto seconds = time.seconds;
    std::int64_t skew_ns = 0;
    std::int32_t previous = kClockNone;
    if (!set_clock(seconds, request.received_mono_us, kClockHost, &skew_ns,
                   &previous, time.subsecond_us)) {
      respond_error(ble::kOpSetTime, ble::kErrInvalidEpoch, nullptr);
      return;
    }
    report_host_clock(request.link == ble::Link::Ble ? "ble" : "lan", seconds,
                      request.received_mono_us, previous, skew_ns);
    // Acceptance never flushes an unfinished interval. A genuine clock/UTC
    // transition closes the previous file when its next sample arrives.
    std::uint8_t frame[21];
    frame[0] = ble::kFrameTimeSet;
    const auto length =
        aq::sync::encode_time_ack(frame + 1, time, request.received_mono_us);
    respond(frame, length + 1);
    publish_status();
    return;
  }
  case ble::kOpFlush: {
    if (!state.count && !writer_state->telemetry.open()) {
      respond_error(ble::kOpFlush, ble::kErrNothingToFlush, nullptr);
      return;
    }
    if (!storage_writable(state.count, state.storage_ready)) {
      respond_error(ble::kOpFlush, ble::kErrStorage,
                    storage::state_name(governor.state()));
      return;
    }
    // Rows reported: RAM batch plus row groups already on the card.
    const auto rows =
        static_cast<std::uint16_t>(state.count + open_rows.load());
    if (!governed_commit(state.count, true)) {
      respond_error(ble::kOpFlush, ble::kErrStorage, "write");
      return;
    }
    std::uint8_t frame[7];
    frame[0] = ble::kFrameFlushed;
    put_u16(frame + 1, rows);
    put_u32(frame + 3, finalized.load());
    respond(frame, sizeof(frame));
    publish_status();
    return;
  }
  case ble::kOpStatus:
    publish_status();
    return;
  case ble::kOpReboot: {
    // Never lose the RAM batch to a reboot the owner asked for.
    if ((state.count || writer_state->telemetry.open()) &&
        state.storage_ready && governor.writable())
      governed_commit(state.count, true);
    // Rows taken while REBOOTING waits for ring space would die with the
    // restart, so leave them in the sample queue.
    active_state = nullptr;
    constexpr std::uint16_t kDelayMs = 500;
    std::uint8_t frame[3];
    frame[0] = ble::kFrameRebooting;
    put_u16(frame + 1, kDelayMs);
    respond(frame, sizeof(frame));
    aqlog.printf("PARQUET REBOOT source=%s delay_ms=%u\n",
                 request.link == ble::Link::Lan ? "lan" : "ble",
                 unsigned(kDelayMs));
    // LAN sessions hear BYE reboot before their sockets vanish.
    lan::drop_session(ble::DisconnectReason::Reboot);
    aq::console::flush();
    vTaskDelay(pdMS_TO_TICKS(kDelayMs));
    // After the delay REBOOTING has left; a BLE peer gets a recorded close too.
    ble::disconnect_and_wait(ble::DisconnectReason::Reboot, 1000);
    esp_restart();
    return;
  }
  default:
    respond_error(static_cast<ble::Op>(request.bytes[0]), ble::kErrUnknownOp,
                  nullptr);
  }
}

void storage_worker(void *) {
  // Startup scans below are covered from the first instruction.
  stamp_heartbeat();
  bool storage_ready;
  {
    BusLock lock;
    storage_ready = mounted && aq::logger::detail::ensure_directory(kDirectory);
  }
  storage_ok = storage_ready;
  if (storage_ready) {
    if (!measure_volume())
      governor.write_failed(uptime_seconds());
    publish_storage(storage::State::Retry);
    const unsigned quarantined = quarantine_partials();
    aqlog.printf("PARQUET RECOVERY quarantined=%u repaired=0 deleted=0\n",
                 quarantined);
    list_files(false);
  } else {
    ++errors;
    aqlog.printf("PARQUET ERROR operation=mount-or-directory "
                 "samples_will_be_dropped=true remount=%s\n",
                 hooks.storage_remount ? "retry" : "none");
    if (hooks.storage_remount)
      governor.write_failed(uptime_seconds());
    sd_state = static_cast<std::uint8_t>(governor.state());
    worker_failed = true; // fail means "not writing"
    publish_status();
  }
  std::size_t count = 0;
  Sample row;
  Command command;
  for (;;) {
    stamp_heartbeat();
    // Sample priority is retained without sleeping before every control
    // request. Successful producers signal after copying to a queue.
    if (xQueueReceive(samples, &row, 0) == pdTRUE) {
      if ((!storage_ready && !hooks.storage_remount) || count == kMaxRows) {
        ++dropped;
      } else {
        // A batch from another window can only leave RAM through a write.
        // While the card cannot take it, the new row is the one dropped.
        if (!fits_batch(row, count) &&
            (!storage_ready || !governor.writable() ||
             !governed_commit(count, true))) {
          ++dropped;
          continue;
        }
        writer_state->rows[count++] = row;
        buffered = count;
        if (count >= rows_per_group() && storage_ready && governor.writable())
          governed_commit(count, false);
      }
    }
    service_storage(count, storage_ready);
    // A recheck can resume without a probe write. A full batch would
    // otherwise drop every later sample. A failed commit leaves writable().
    if (count >= rows_per_group() && storage_ready && governor.writable())
      governed_commit(count, false);
    if (xQueueReceive(commands, &command, 0) != pdTRUE) {
      // Drain pending samples before sleeping. Queue-then-signal also covers
      // an enqueue racing this check: the signal remains pending until taken.
      work::wait_if_idle(samples, worker_wakeup);
      continue;
    }
    if (command.source == Command::Source::Network) {
      const auto &anchor = command.network_time;
      if (set_clock(anchor.seconds, anchor.monotonic_us, aq::utc::Network,
                    nullptr, nullptr, anchor.subsecond_us)) {
        aqlog.printf(
            "PARQUET TIME epoch_s=%lld source=network monotonic_us=%lld\n",
            static_cast<long long>(anchor.seconds),
            static_cast<long long>(anchor.monotonic_us));
        publish_status();
      }
      continue;
    }
    if (command.source == Command::Source::Control) {
      WorkerState state{count, storage_ready};
      current_request = &command.control;
      active_state = &state;
      handle_control_request(command.control, state);
      active_state = nullptr;
      current_request = nullptr;
      continue;
    }
    if (std::strcmp(command.text, "parquet erase-archive CONFIRM") == 0) {
      // Physical UART only: radio controls cannot reach this dispatcher.
      // Pause acquisition before closing files or deleting the archive. Stay
      // paused even on failure so a reset cannot be undone by a queued command.
      if (!storage_ready) {
        aq::console::printf("PARQUET ERASE_ARCHIVE ok=0 reason=no-storage\n");
        continue;
      }
      archive_reset = true;
      archive_session.close();
      writer_state->crc_cache.clear();
      std::uint32_t files_removed = 0, directories_removed = 0;
      bool erased;
      {
        BusLock lock;
        bool closed = true;
        for (auto *target :
             {&writer_state->telemetry, writer_state->benchmark}) {
          if (target && target->file) {
            closed &= std::fclose(target->file) == 0;
            target->file = nullptr;
          }
        }
        // Only the configured archive roots. The benchmark root is not
        // archive data and stays untouched. Every visited entry stamps the
        // heartbeat, so a large archive is not mistaken for a stall.
        aq::logger::detail::ArchiveResetIo io;
        io.progress = stamp_heartbeat;
        erased =
            closed &&
            aq::logger::detail::erase_archive(
                kDirectory, kDirectory, config.legacy_directory, files_removed,
                directories_removed, io) &&
            (!config.legacy_directory ||
             aq::logger::detail::erase_archive(
                 config.legacy_directory, kDirectory, config.legacy_directory,
                 files_removed, directories_removed, io));
      }
      count = 0;
      buffered = open_rows = open_groups = 0;
      storage_ok = false;
      worker_failed = !erased;
      xQueueReset(samples);
      xQueueReset(commands);
      if (erased) {
        partials_seen = partials_quarantined = 0;
        quarantine_bytes = 0;
        unlisted_files = false;
      } else
        ++errors;
      aq::console::printf("PARQUET ERASE_ARCHIVE ok=%u files=%lu "
                          "directories=%lu reboot_required=1\n",
                          erased ? 1U : 0U,
                          static_cast<unsigned long>(files_removed),
                          static_cast<unsigned long>(directories_removed));
      publish_status();
      for (;;) {
        stamp_heartbeat();
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    } else if (std::strcmp(command.text, "parquet owner-pin") == 0) {
      // Physical serial only. Bypass aqlog so the PIN never enters LOG_TAIL.
      // Control requests use a separate opcode dispatcher above and cannot
      // reach this command. No PIN is printed at boot.
      const auto settings = ::config::get();
      if (settings.pair == ::config::PairMode::Fixed)
        aq::console::printf("AQ OWNER_PIN %06lu\n",
                            static_cast<unsigned long>(settings.pin));
      else
        aq::console::printf(
            "AQ OWNER_PIN ERROR mode=%s reason=use-pairing-display\n",
            ::config::pair_name(settings.pair));
    } else if (std::strcmp(command.text, "parquet wifi-profile") == 0) {
      // Credential-bearing output never passes through the log ring.
      char profile[256];
      const auto length =
          format_wifi_profile(::config::get(), profile, sizeof(profile));
      if (length)
        aq::console::write(reinterpret_cast<const std::uint8_t *>(profile),
                           length);
      else
        aqlog.println("AQ WIFI_PROFILE ERROR reason=capacity");
    } else if (std::strncmp(command.text, "parquet config-hex ", 19) == 0) {
      const auto result = apply_config_hex(command.text + 19);
      if (result.ok)
        aq::console::printf("AQ CONFIG ok=1 reboot_required=%u\n",
                            result.reboot_required ? 1U : 0U);
      else
        aqlog.printf("AQ CONFIG ok=0 key=%s\n", result.bad_key);
      std::memset(command.text, 0, sizeof(command.text));
    } else if (std::strcmp(command.text, "parquet status") == 0) {
      aqlog.printf(
          "PARQUET STATUS interval_s=%lu buffered=%u open_rows=%lu "
          "open_groups=%lu finalized=%lu "
          "dropped=%lu errors=%lu queue_peak=%lu failed=%s sd_state=%s "
          "sd_free_kib=%llu station=%s "
          "codec=%s schema=%s config=%s deployment=unknown "
          "calibration=unknown\n",
          static_cast<unsigned long>(rotation_seconds.load()), unsigned(count),
          static_cast<unsigned long>(open_rows.load()),
          static_cast<unsigned long>(open_groups.load()),
          static_cast<unsigned long>(finalized.load()),
          static_cast<unsigned long>(dropped.load()),
          static_cast<unsigned long>(errors.load()),
          static_cast<unsigned long>(queue_peak.load()),
          worker_failed.load() ? "true" : "false",
          storage::state_name(governor.state()),
          static_cast<unsigned long long>(governor.free_bytes() / 1024),
          station_text, codec_name(selected_codec), config.schema_name,
          config.configuration_id);
    } else if (std::strcmp(command.text, "parquet schema") == 0) {
      aqlog.printf("PARQUET SCHEMA BEGIN schema=%s columns=%u sha256=%s\n",
                   config.schema_name, unsigned(field_count),
                   config.dictionary_sha256);
      for (std::size_t i = 0; i < field_count; ++i) {
        const auto &field = config.fields[i];
        aqlog.printf("PARQUET FIELD name=%s type=%u procedure=%s unit=%s "
                     "validity=%s property=%s\n",
                     field.name, unsigned(field.type), field.procedure,
                     field.unit, field.validity, field.property_uri);
        vTaskDelay(pdMS_TO_TICKS(1) > 0 ? pdMS_TO_TICKS(1) : 1);
      }
      aqlog.println("PARQUET SCHEMA END");
    } else if (std::strcmp(command.text, "parquet codec-test") == 0) {
      // Diagnostic copies only spend space a Ready card does not need.
      if (!count || !storage_writable(count, storage_ready) ||
          governor.state() != storage::State::Ready) {
        aqlog.printf("PARQUET ERROR operation=codec-test reason=%s\n",
                     count ? storage::state_name(governor.state()) : "empty");
      } else if (!alloc_benchmark()) {
        ++errors;
        aqlog.printf("PARQUET ERROR operation=allocate mem=psram bytes=%u "
                     "purpose=codec-test\n",
                     unsigned(sizeof(OutputFile) + kStagingBytes));
      } else {
        const auto previous = governor.state();
        const bool ok = write_benchmark(count, Codec::Uncompressed) &&
                        write_benchmark(count, Codec::Lz4Raw);
        free_benchmark();
        // A failed diagnostic copy does not pause telemetry; the next
        // telemetry write is the card test.
        std::uint64_t free_bytes = 0;
        if (measure_free(free_bytes))
          governor.observe_free(free_bytes, uptime_seconds());
        publish_storage(previous);
        // Keep the original batch for normal telemetry rotation. Benchmark
        // copies live under the bench root, outside the synced archive.
        if (ok)
          aqlog.printf(
              "PARQUET BENCH END rows=%u retained_for_telemetry=true\n",
              unsigned(count));
      }
    } else if (std::strcmp(command.text, "parquet codec none") == 0 ||
               std::strcmp(command.text, "parquet codec lz4") == 0) {
      // A file holds one codec: finish the open one before switching.
      if ((count || writer_state->telemetry.open()) &&
          (!storage_writable(count, storage_ready) ||
           !governed_commit(count, true))) {
        aqlog.printf("PARQUET ERROR operation=codec reason=storage-%s\n",
                     storage::state_name(governor.state()));
        continue;
      }
      selected_codec =
          command.text[14] == 'l' ? Codec::Lz4Raw : Codec::Uncompressed;
      aqlog.printf("PARQUET CONFIG codec=%s persistent=false\n",
                   codec_name(selected_codec));
    } else if (std::strcmp(command.text, "parquet flush") == 0) {
      if (!count && !writer_state->telemetry.open())
        aqlog.println("PARQUET ERROR operation=flush reason=empty");
      else if (!storage_writable(count, storage_ready))
        aqlog.printf("PARQUET ERROR operation=flush reason=storage-%s\n",
                     storage::state_name(governor.state()));
      else
        governed_commit(count, true);
    } else if (std::strncmp(command.text, "parquet interval ", 17) == 0 &&
               (std::strcmp(command.text + 17, "600") == 0 ||
                std::strcmp(command.text + 17, "900") == 0 ||
                std::strcmp(command.text + 17, "1800") == 0 ||
                std::strcmp(command.text + 17, "3600") == 0)) {
      // The window defines the file: finish the open one first.
      if ((count || writer_state->telemetry.open()) &&
          (!storage_writable(count, storage_ready) ||
           !governed_commit(count, true))) {
        aqlog.printf("PARQUET ERROR operation=interval reason=storage-%s\n",
                     storage::state_name(governor.state()));
        continue;
      }
      rotation_seconds = static_cast<std::uint32_t>(
          std::strtoul(command.text + 17, nullptr, 10));
      aqlog.printf("PARQUET CONFIG interval_s=%lu persistent=false\n",
                   static_cast<unsigned long>(rotation_seconds.load()));
    } else if (std::strncmp(command.text, "parquet time ", 13) == 0) {
      char *end = nullptr;
      const auto seconds = std::strtoll(command.text + 13, &end, 10);
      const auto mono = command.received_mono_us;
      std::int64_t skew_ns = 0;
      std::int32_t previous = kClockNone;
      if (!end || *end ||
          !set_clock(seconds, mono, kClockHost, &skew_ns, &previous)) {
        aqlog.println("PARQUET ERROR operation=time reason=invalid-epoch");
        continue;
      }
      report_host_clock("serial", seconds, mono, previous, skew_ns);
      publish_status();
    } else if (std::strcmp(command.text, "parquet list") == 0)
      list_files();
    else if (std::strncmp(command.text, "parquet get ", 12) == 0)
      send_file(command.text + 12);
    else
      aqlog.println("PARQUET ERROR operation=command reason=unknown-command");
  }
}

void collect(std::int64_t now, std::int64_t scheduled) {
  Sample row{};
  row.location = ::config::get().location;
  if (hooks.collect)
    hooks.collect(row, now, scheduled, hooks.context);
  const std::array<std::size_t, 4> location_fields = {
      field_index("h3_cell_id"), field_index("h3_resolution"),
      field_index("h3_center_lat_e7"), field_index("h3_center_lon_e7")};
  if (std::all_of(location_fields.begin(), location_fields.end(),
                  [](std::size_t index) { return index < field_count; }))
    apply_location(row, row.location, location_fields);
  integer(row, "schema_version", config.schema_version);
  counter(row, "device_id", device);
  counter(row, "boot_id_hi", boot_hi);
  counter(row, "boot_id_lo", boot_lo);
  counter(row, "sequence", sample_sequence++);
  counter(row, "monotonic_us", now);
  counter(row, "scheduled_us", scheduled);
  counter(row, "sample_jitter_us", now - scheduled);
  std::int64_t utc_anchor, mono_anchor;
  std::int32_t generation, source;
  portENTER_CRITICAL(&clock_mutex);
  utc_anchor = anchor_utc_ns;
  mono_anchor = anchor_mono_us;
  generation = clock_generation;
  source = clock_source;
  portEXIT_CRITICAL(&clock_mutex);
  integer(row, "clock_status", generation ? source : kClockNone);
  integer(row, "clock_epoch", generation);
  if (generation) {
    counter(row, "event_time_utc_us",
            aq::utc::estimate_us(now, mono_anchor, utc_anchor));
    counter(row, "clock_anchor_mono_us", mono_anchor);
    counter(row, "clock_anchor_utc_us", aq::utc::ns_to_us(utc_anchor));
  }
  counter(row, "heap_free_bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  counter(row, "heap_min_free_bytes",
          heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  counter(row, "psram_free_bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  if (total_kib.load()) { // also set by a later remount
    counter(row, "sd_total_bytes", std::int64_t(total_kib.load()) * 1024);
    counter(row, "sd_used_bytes", std::int64_t(used_kib.load()) * 1024);
  }
  counter(row, "rows_dropped", dropped.load());
  counter(row, "sample_deadlines_missed", missed_deadlines.load());
  counter(row, "storage_errors", errors.load());
  counter(row, "files_finalized", finalized.load());
  counter(row, "last_write_us", write_us.load());
  counter(row, "last_sync_us", sync_us.load());
  integer(row, "queue_high_water", queue_peak.load());
  counter(row, "collection_completed_mono_us", esp_timer_get_time());
  if (!work::enqueue(samples, &row, worker_wakeup))
    ++dropped;
  const auto depth =
      static_cast<std::uint32_t>(uxQueueMessagesWaiting(samples));
  if (depth > queue_peak.load())
    queue_peak = depth;
  publish_live(row);
  publish_status();
  aqlog.printf("PARQUET ROW sequence=%lld monotonic_us=%lld jitter_us=%lld "
               "buffered=%lu dropped=%lu\n",
               static_cast<long long>(row.data[sequence]),
               static_cast<long long>(now),
               static_cast<long long>(now - scheduled),
               static_cast<unsigned long>(buffered.load()),
               static_cast<unsigned long>(dropped.load()));
}
// Hourly, and once a minute after begin: stack high water marks (bytes left)
// for every app task and the heap minimums, so stacks are sized from
// measurements. Called on the board task, which reports itself by name.
void report_runtime(std::int64_t now) {
  static std::int64_t next_report_us = 0;
  if (!next_report_us)
    next_report_us = now + 60LL * 1000000LL;
  if (now < next_report_us)
    return;
  next_report_us = now + 3600LL * 1000000LL;
  const auto stack_free = [](const char *name) {
    TaskHandle_t task = xTaskGetHandle(name);
    return task ? long(uxTaskGetStackHighWaterMark(task)) : -1L;
  };
  aqlog.printf(
      "DIAG tasks %s=%u parquet-sd=%ld aq-lan=%ld aq_ble_error=%ld "
      "nimble_host=%ld tcpip=%ld uptime_s=%lld\n",
      pcTaskGetName(nullptr), unsigned(uxTaskGetStackHighWaterMark(nullptr)),
      worker_task ? long(uxTaskGetStackHighWaterMark(worker_task)) : -1L,
      stack_free("aq-lan"), stack_free("aq_ble_error"),
      stack_free("nimble_host"), stack_free("tcpip"),
      static_cast<long long>(now / 1000000));
  constexpr auto kInternal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  constexpr auto kDma = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
  aqlog.printf(
      "DIAG heap internal_free=%lu internal_min=%lu internal_largest=%lu "
      "dma_free=%lu dma_min=%lu dma_largest=%lu psram_free=%lu "
      "psram_min=%lu\n",
      static_cast<unsigned long>(heap_caps_get_free_size(kInternal)),
      static_cast<unsigned long>(heap_caps_get_minimum_free_size(kInternal)),
      static_cast<unsigned long>(heap_caps_get_largest_free_block(kInternal)),
      static_cast<unsigned long>(heap_caps_get_free_size(kDma)),
      static_cast<unsigned long>(heap_caps_get_minimum_free_size(kDma)),
      static_cast<unsigned long>(heap_caps_get_largest_free_block(kDma)),
      static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
      static_cast<unsigned long>(
          heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)));
}
} // namespace

std::size_t field_index(const char *name) {
  if (!name)
    return field_count;
  for (std::size_t i = 0; i < field_count; ++i)
    if (std::strcmp(config.fields[i].name, name) == 0)
      return i;
  return field_count;
}
void integer(Row &row, const char *name, std::int32_t value) {
  const auto index = field_index(name);
  if (index < field_count &&
      config.fields[index].type == telemetry::PhysicalType::Int32)
    row.integer(index, value);
}
void counter(Row &row, const char *name, std::int64_t value) {
  const auto index = field_index(name);
  if (index < field_count &&
      config.fields[index].type == telemetry::PhysicalType::Int64)
    row.counter(index, value);
}
void number(Row &row, const char *name, float value) {
  const auto index = field_index(name);
  if (index < field_count &&
      config.fields[index].type == telemetry::PhysicalType::Float)
    row.number(index, value);
}

void lock_bus() {
  if (hooks.lock_bus)
    hooks.lock_bus(hooks.context);
  else if (spi_mutex)
    xSemaphoreTake(spi_mutex, portMAX_DELAY);
}
void unlock_bus() {
  if (hooks.unlock_bus)
    hooks.unlock_bus(hooks.context);
  else if (spi_mutex)
    xSemaphoreGive(spi_mutex);
}

// True when `path` is `root` or lies below it.
bool path_within(const char *path, const char *root) {
  const std::size_t length = std::strlen(root);
  return std::strncmp(path, root, length) == 0 &&
         (path[length] == '\0' || path[length] == '/');
}

#if CONFIG_AQ_LOGGER_WORKER_WDT
// Watchdog state shared by begin(), poll() and the supervisor timer. The
// supervisor state belongs to start_worker_watchdog() until the timer starts
// and to the timer after that.
std::atomic<bool> wdt_armed{false};
std::atomic<bool> wdt_cleared{false};
std::atomic<bool> wdt_recovered{false};
// The guard's count as the supervisor last left it. The guard itself belongs
// to the supervisor timer, so poll() reads this copy instead.
std::atomic<std::uint32_t> wdt_consecutive{0};
watchdog::Supervisor wdt_supervisor;

// The task watchdog has one global config shared with the idle tasks and any
// other user. A trip switches only its panic flag, and a recovery puts back
// the config the IDF started with.
esp_task_wdt_config_t task_wdt_config(bool panic) {
  std::uint32_t idle_core_mask = 0;
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
  idle_core_mask |= 1U;
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
  idle_core_mask |= 2U;
#endif
  return {CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000U, idle_core_mask, panic};
}
#ifdef CONFIG_ESP_TASK_WDT_PANIC
constexpr bool kTaskWdtPanic = true;
#else
constexpr bool kTaskWdtPanic = false;
#endif

// The worker's task watchdog user, as watchdog::apply() and the start steps
// in aq_logger_watchdog_apply.h use it.
struct IdfTaskWdt {
  bool add_user() {
    esp_task_wdt_user_handle_t user = nullptr;
    if (esp_task_wdt_add_user("parquet-sd", &user) != ESP_OK)
      return false;
    worker_wdt = user;
    return true;
  }
  bool reset_user() {
    return esp_task_wdt_reset_user(worker_wdt.load()) == ESP_OK;
  }
  bool reconfigure(bool panic) {
    const auto config = task_wdt_config(panic);
    return esp_task_wdt_reconfigure(&config) == ESP_OK;
  }
  // Unregisters the user so nothing is left armed without a feeder. A worker
  // stamp racing this sees a cleared handle or one the IDF no longer lists.
  void delete_user() {
    if (auto *user = worker_wdt.exchange(nullptr))
      esp_task_wdt_delete_user(user);
  }
};

void supervise_worker(void *);

struct IdfSupervisorTimer {
  esp_timer_handle_t handle = nullptr;
  bool create() {
    static const esp_timer_create_args_t args{supervise_worker, nullptr,
                                              ESP_TIMER_TASK, "aq-wdt", true};
    return esp_timer_create(&args, &handle) == ESP_OK;
  }
  bool start() {
    return esp_timer_start_periodic(handle, 1000000ULL) == ESP_OK;
  }
  void destroy() {
    esp_timer_delete(handle);
    handle = nullptr;
  }
};

// Runs every second in the esp_timer task. The task watchdog has one global
// period (CONFIG_ESP_TASK_WDT_TIMEOUT_S) shared with the idle tasks, so this
// feeds the worker's user on its behalf while the heartbeat is younger than
// the logger timeout. Long single SD calls between stamps are therefore safe.
// Once the heartbeat expires it stops feeding, records the pending stall in
// RTC memory and switches the task watchdog to panic, so the next period
// names "parquet-sd" and resets with a core dump. A worker that moves again
// first is recovered. The pending stall is withdrawn and the original config
// comes back, so a later stall trips again. watchdog::supervise() decides and
// watchdog::tick() applies it.
void supervise_worker(void *) {
  IdfTaskWdt twdt;
  const auto result = watchdog::tick(
      wdt_supervisor, stall_guard, twdt, esp_timer_get_time(),
      worker_heartbeat_us.load(), CONFIG_AQ_LOGGER_WORKER_WDT_TIMEOUT_S,
      CONFIG_AQ_LOGGER_WORKER_WDT_HEALTHY_S, kTaskWdtPanic);
  wdt_consecutive = stall_guard.consecutive;
  if (result.cleared)
    wdt_cleared = true;
  if (result.recovered)
    wdt_recovered = true;
  wdt_armed = wdt_supervisor.armed;
}

const char *watchdog_mode_name() { return wdt_armed.load() ? "armed" : "warn"; }

void start_worker_watchdog() {
  IdfTaskWdt twdt;
  IdfSupervisorTimer timer;
  auto boot = watchdog::prepare(wdt_supervisor, stall_guard, twdt, timer,
                                esp_reset_reason() == ESP_RST_TASK_WDT,
                                esp_timer_get_time());
  // Published before the timer runs. After launch() the guard belongs to it.
  const std::uint32_t consecutive = stall_guard.consecutive;
  wdt_consecutive = consecutive;
  wdt_armed = boot.armed;
  if (boot.stall_reset)
    aqlog.printf("PARQUET ERROR operation=worker-watchdog-reset "
                 "consecutive=%lu\n",
                 static_cast<unsigned long>(consecutive));
  watchdog::launch(boot, wdt_supervisor, twdt, timer);
  if (!boot.supervised)
    wdt_armed = false;
  aqlog.printf("PARQUET WATCHDOG mode=%s timeout_s=%u consecutive=%lu "
               "max_consecutive=%lu healthy_s=%u supervised=%u\n",
               boot.armed ? "armed" : "warn",
               unsigned(CONFIG_AQ_LOGGER_WORKER_WDT_TIMEOUT_S),
               static_cast<unsigned long>(consecutive),
               static_cast<unsigned long>(watchdog::kMaxConsecutive),
               unsigned(CONFIG_AQ_LOGGER_WORKER_WDT_HEALTHY_S),
               boot.supervised ? 1U : 0U);
}
#else
const char *watchdog_mode_name() { return "off"; }
#endif

bool begin(const Config &configuration, const Hooks &board_hooks,
           bool sd_mounted) {
  if (writer_state || accepting || !configuration.fields ||
      configuration.column_count == 0 ||
      configuration.column_count > telemetry::kMaxColumns ||
      configuration.metadata_count > kMaxConfigMetadata ||
      (configuration.metadata_count && !configuration.metadata) ||
      bool(board_hooks.lock_bus) != bool(board_hooks.unlock_bus) ||
      ((sd_mounted || board_hooks.storage_remount) &&
       (!board_hooks.storage_total_bytes || !board_hooks.storage_used_bytes ||
        !board_hooks.storage_free_bytes)))
    return false;
  config = configuration;
  hooks = board_hooks;
  field_count = config.column_count;
  if (!config.schema_name || !config.firmware || !config.dictionary_version ||
      !config.dictionary_uri || !config.dictionary_sha256 ||
      !config.configuration_id || !config.configuration ||
      !config.time_semantics || !config.board || !config.output_directory ||
      config.output_directory[0] != '/' ||
      std::strlen(config.output_directory) > 31 ||
      !config.benchmark_directory || config.benchmark_directory[0] != '/' ||
      std::strlen(config.benchmark_directory) > 31 ||
      path_within(config.benchmark_directory, config.output_directory) ||
      path_within(config.output_directory, config.benchmark_directory))
    return false;
  for (std::size_t i = 0; i < field_count; ++i) {
    const auto &field = config.fields[i];
    if (!field.name || !field.unit || !field.procedure || !field.validity ||
        !field.property_uri)
      return false;
    for (std::size_t j = 0; j < i; ++j)
      if (std::strcmp(field.name, config.fields[j].name) == 0)
        return false;
  }
  if (!valid_live_keys(config.fields, field_count))
    return false;
  sequence = field_index("sequence");
  event_time_utc_us = field_index("event_time_utc_us");
  clock_epoch = field_index("clock_epoch");
  const auto monotonic = field_index("monotonic_us");
  if (sequence == field_count || event_time_utc_us == field_count ||
      clock_epoch == field_count || monotonic == field_count ||
      config.fields[sequence].type != telemetry::PhysicalType::Int64 ||
      config.fields[event_time_utc_us].type != telemetry::PhysicalType::Int64 ||
      config.fields[monotonic].type != telemetry::PhysicalType::Int64 ||
      config.fields[clock_epoch].type != telemetry::PhysicalType::Int32)
    return false;
  monotonic_field = monotonic;
  kDirectory = config.output_directory;
  kBenchDirectory = config.benchmark_directory;
  mounted = sd_mounted;
  if (!station_identity()) {
    aqlog.println("PARQUET ERROR operation=station-identity logging=false");
    return false;
  }
  std::uint8_t mac[6]{};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    aqlog.println("PARQUET ERROR operation=identity");
    return false;
  }
  device = std::accumulate(std::begin(mac), std::end(mac), std::int64_t{0},
                           [](std::int64_t value, std::uint8_t byte) {
                             return (value << 8) | byte;
                           });
  std::snprintf(device_text, sizeof(device_text), "%012llx",
                static_cast<unsigned long long>(device));
  std::uint32_t random[4];
  esp_fill_random(random, sizeof(random));
  std::memcpy(&boot_hi, random, 8);
  std::memcpy(&boot_lo, random + 2, 8);
  std::snprintf(boot_text, sizeof(boot_text), "%016llx%016llx",
                static_cast<unsigned long long>(boot_hi),
                static_cast<unsigned long long>(boot_lo));
  spi_mutex = xSemaphoreCreateMutex();
  sync_config = config.sync ? *config.sync : aqsync::ServiceConfig{};
  samples = xQueueCreate(sync_config.sample_queue_depth, sizeof(Sample));
  commands = xQueueCreate(sync_config.command_queue_depth, sizeof(Command));
  // Created before the worker: wakeups do not depend on task-handle
  // publication.
  worker_wakeup = xSemaphoreCreateBinary();
  writer_state = static_cast<WriterState *>(heap_caps_calloc(
      1, sizeof(WriterState), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!spi_mutex || !samples || !commands || !worker_wakeup || !writer_state) {
    if (!writer_state)
      aqlog.printf("PARQUET ERROR operation=allocate mem=psram bytes=%u "
                   "purpose=writer-state logging=false\n",
                   unsigned(sizeof(WriterState)));
    else
      aqlog.println("PARQUET ERROR operation=allocate logging=false");
    return false;
  }
  new (writer_state) WriterState{};
  writer_state->telemetry.staging = staging_telemetry;
  prepare_columns();
  // Before the worker starts, so the first row of the boot is already dated
  // when the RTC holds an earlier host sync.
  seed_clock_from_rtc();
  next_sample_us = esp_timer_get_time() + kSampleUs;
  // Seed the heartbeat so a worker that blocks before its first stamp ages
  // out like any other stall, for the worker-stall warning and the watchdog.
  worker_heartbeat_us = esp_timer_get_time();
  if (xTaskCreate(storage_worker, "parquet-sd",
                  static_cast<std::uint32_t>(config.worker_stack_bytes),
                  nullptr, config.worker_priority, &worker_task) != pdPASS) {
    aqlog.println("PARQUET ERROR operation=task logging=false");
    return false;
  }
#if CONFIG_AQ_LOGGER_WORKER_WDT
  start_worker_watchdog();
#else
  aqlog.println("PARQUET WATCHDOG mode=off");
#endif
  accepting = true;
  // The CRC-32 check value through the ROM routine OPEN uses on the device.
  const auto *check_bytes = reinterpret_cast<const std::uint8_t *>("123456789");
  const char *crc_check =
      (crc32_update(0xffffffffU, check_bytes, 9) ^ 0xffffffffU) == 0xcbf43926U
          ? "ok"
          : "bad";
  aqlog.printf(
      "PARQUET BEGIN schema=%s columns=%u sample_s=10 "
      "interval_s=900 max_rows=90 max_row_groups=%u psram_workspace_bytes=%u "
      "row_bytes=%u boot=%s station=%s "
      "codec=UNCOMPRESSED crc32_check=%s\n",
      config.schema_name, unsigned(field_count), unsigned(kMaxRowGroups),
      unsigned(sizeof(WriterState)), unsigned(sizeof(Sample)), boot_text,
      station_text, crc_check);
  return true;
}

void poll() {
  if (!accepting || archive_reset.load())
    return;
  const auto now = esp_timer_get_time();
  // Only an actual SNTP callback provides network time. Never treat an
  // arbitrary system clock or mere Wi-Fi association as UTC evidence.
  static Command network_command{};
  static bool network_pending = false;
  if (!network_pending)
    network_pending = lan::take_time_anchor(network_command.network_time);
  if (network_pending) {
    network_command.source = Command::Source::Network;
    if (work::enqueue(commands, &network_command, worker_wakeup))
      network_pending = false;
  }
  {
    static std::int64_t last_stall_warning_us = 0;
    const auto heartbeat = worker_heartbeat_us.load();
    // Warn only. The reset decision belongs to the watchdog supervisor.
    if (work::check_stall(now, heartbeat, last_stall_warning_us, 0) ==
        work::StallAction::Warn) {
      const char *state_name = "?";
      if (worker_task) {
        switch (eTaskGetState(worker_task)) {
        case eRunning:
          state_name = "running";
          break;
        case eReady:
          state_name = "ready";
          break;
        case eBlocked:
          state_name = "blocked";
          break;
        case eSuspended:
          state_name = "suspended";
          break;
        case eDeleted:
          state_name = "deleted";
          break;
        default:
          break;
        }
      }
      const auto elapsed_s =
          static_cast<long long>((now - heartbeat) / 1000000);
      const unsigned stack_free =
          worker_task ? unsigned(uxTaskGetStackHighWaterMark(worker_task)) : 0U;
      aqlog.printf("PARQUET ERROR operation=worker-stall seconds=%lld "
                   "state=%s stack_free=%u watchdog=%s\n",
                   elapsed_s, state_name, stack_free, watchdog_mode_name());
    }
  }
#if CONFIG_AQ_LOGGER_WORKER_WDT
  if (wdt_recovered.exchange(false))
    aqlog.printf("PARQUET WATCHDOG mode=%s consecutive=%lu reason=recovered\n",
                 watchdog_mode_name(),
                 static_cast<unsigned long>(wdt_consecutive.load()));
  if (wdt_cleared.exchange(false))
    aqlog.printf("PARQUET WATCHDOG mode=%s consecutive=0 reason=healthy\n",
                 watchdog_mode_name());
#endif
  report_runtime(now);
  if (now >= next_sample_us) {
    const auto skipped = (now - next_sample_us) / kSampleUs;
    missed_deadlines += skipped;
    sample_sequence += skipped;
    next_sample_us += skipped * kSampleUs;
    collect(now, next_sample_us);
    next_sample_us += kSampleUs;
  }
  service_rtc_write(esp_timer_get_time());
  static Command input{};
  static std::size_t length = 0;
  static bool overflow = false;
  input.source = Command::Source::Serial;
  for (unsigned limit = 0; limit < 128; ++limit) {
    const int byte = aq::console::read();
    if (byte < 0)
      break;
    if (byte == '\r')
      continue;
    if (byte == '\n') {
      input.text[length] = '\0';
      input.received_mono_us = esp_timer_get_time();
      if (overflow || !work::enqueue(commands, &input, worker_wakeup))
        aqlog.println(
            "PARQUET ERROR operation=command reason=too-long-or-busy");
      length = 0;
      overflow = false;
    } else if (byte >= 32 && byte <= 126) {
      if (length + 1 < sizeof(input.text))
        input.text[length++] = static_cast<char>(byte);
      else
        overflow = true;
    }
  }
}
bool start_links(bool display_detected) {
  if (!accepting)
    return false;
  static const ble::Identity identity{
      station_text,       device_text,           boot_text,
      config.schema_name, unsigned(field_count), config.dictionary_sha256,
      config.firmware};
  return aqsync::begin(identity, enqueue_request, display_detected, sync_config)
      .ble_started;
}

bool enqueue_request(const ble::ControlRequest &request) {
  if (archive_reset.load() || !commands || !worker_wakeup ||
      request.length == 0 || request.length > sizeof(request.bytes))
    return false;
  // BLE and LAN invoke this concurrently; xQueueSend copies this local value.
  Command command{};
  command.source = Command::Source::Control;
  command.received_mono_us = request.received_mono_us;
  command.control = request;
  return work::enqueue(commands, &command, worker_wakeup);
}

const char *station_text_id() { return station_text; }
const char *device_text_id() { return device_text; }
const char *firmware_text_id() { return config.firmware; }

ClockView clock_view() {
  ClockView view{};
  std::int64_t utc_anchor, mono_anchor;
  std::int32_t generation, source;
  portENTER_CRITICAL(&clock_mutex);
  utc_anchor = anchor_utc_ns;
  mono_anchor = anchor_mono_us;
  generation = clock_generation;
  source = clock_source;
  portEXIT_CRITICAL(&clock_mutex);
  view.epoch = generation;
  view.source = generation ? source : kClockNone;
  view.source_name = clock_source_name(view.source);
  view.rtc_state = rtc_state.load();
  if (generation)
    view.utc_ns = utc_anchor + (esp_timer_get_time() - mono_anchor) * 1000;
  return view;
}
} // namespace aqlogger
