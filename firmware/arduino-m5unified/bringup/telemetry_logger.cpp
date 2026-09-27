#include "telemetry_logger.h"
#include "debug_log.h"
#include "device_config.h"
#include "ltr553.h"
#include "lz4_codec.h"
#include "parquet_writer.h"
#include "telemetry_contract.h"
#include "wifi_link.h"

#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

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

namespace telemetry {
namespace {
using namespace contract;
constexpr std::size_t kMaxRows = 90;
constexpr std::int64_t kSampleUs = 10000000;
constexpr const char *kDirectory = "/sd/output";
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
  char *staging = nullptr; // internal-RAM stdio buffer
  bool benchmark = false;
  bool dated = false; // false: unsynced tree
  Codec codec = Codec::Uncompressed;
  std::int32_t epoch = 0;
  std::int64_t window = 0; // UTC seconds / rotation, when dated
  std::int64_t first = 0, last = 0;
  std::uint32_t rows = 0;
  std::uint32_t attempt = 0;
  std::int64_t opened_us = 0;
  std::uint64_t writer_us = 0, codec_us = 0, sync_us = 0;
  bool open() const { return file != nullptr; }
};
struct WriterState {
  Sample rows[kMaxRows];
  Column columns[field_count];
  OutputFile telemetry;
  OutputFile benchmark; // codec-test copies never touch the telemetry file
  Lz4Workspace lz4{};
};
// stdio staging stays in internal RAM, distinct from the PSRAM row buffer.
char staging_telemetry[4096];
char staging_benchmark[4096];
struct Command {
  enum class Source : std::uint8_t { Serial, Control };
  Source source = Source::Serial;
  char text[448]{};
  std::int64_t received_mono_us = 0;
  ble::ControlRequest control{};
};
WriterState *writer_state = nullptr;
QueueHandle_t samples = nullptr;
QueueHandle_t commands = nullptr;
SemaphoreHandle_t spi_mutex = nullptr;
Ltr553 light_sensor;
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
// Advertising bit `new_files`: set when a file finalizes, cleared when a LIST
// has been answered on any link (that phone now knows; a phone that then fails
// its download is covered by its periodic run).
// docs/shared/ble-sync-protocol.md.
std::atomic<bool> unlisted_files{false};
std::int64_t boot_hi = 0, boot_lo = 0, device = 0, next_sample_us = 0;
std::int64_t sample_sequence = 0;
std::atomic<std::int64_t> missed_deadlines{0};
// Worker liveness: the loop stamps this every pass; poll_logger warns when it
// stops moving, so a wedged worker is visible on serial instead of silent.
std::atomic<std::int64_t> worker_heartbeat_us{0};
TaskHandle_t worker_task = nullptr;
char boot_text[33]{};
char device_text[13]{};
char station_text[37]{};
portMUX_TYPE clock_mutex = portMUX_INITIALIZER_UNLOCKED;
std::int64_t anchor_mono_us = 0, anchor_utc_ns = 0;
std::int32_t clock_generation = 0;
std::int32_t clock_source = kClockNone;
// A host sync asks the main task (the only task that talks to the internal
// I2C bus after startup) to copy the anchor into the BM8563 RTC.
std::atomic<bool> rtc_write_pending{false};
std::atomic<std::int64_t> rtc_write_requested_us{0};
std::atomic<std::int32_t> rtc_state{0}; // 0 unread, 1 seeded, 2 unusable
bool mounted = false;
bool accepting = false;
Codec selected_codec = Codec::Uncompressed;
const char *codec_name(Codec codec) {
  return codec == Codec::Lz4Raw ? "LZ4_RAW" : "UNCOMPRESSED";
}

class BusLock {
public:
  BusLock() { lock_display(); }
  ~BusLock() { unlock_display(); }
};

std::uint32_t crc_update(std::uint32_t crc, const std::uint8_t *data,
                         std::size_t size) {
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return crc;
}

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
  }
  return true;
}

bool finalized_file(const char *path, std::uint32_t &size, std::uint32_t &crc) {
  // This is a structural completion check. PyArrow + DuckDB perform
  // conformance.
  FILE *file = nullptr;
  {
    BusLock lock;
    file = std::fopen(path, "rb");
  }
  if (!file)
    return false;
  std::uint8_t block[512];
  std::uint8_t magic[4]{}, tail[8]{};
  bool ok = false;
  {
    BusLock lock;
    if (std::fseek(file, 0, SEEK_END) == 0) {
      const long length = std::ftell(file);
      if (length >= 12 && length <= 1048576) {
        size = static_cast<std::uint32_t>(length);
        ok = std::fseek(file, 0, SEEK_SET) == 0 &&
             std::fread(magic, 1, 4, file) == 4 &&
             std::fseek(file, -8, SEEK_END) == 0 &&
             std::fread(tail, 1, 8, file) == 8;
        const std::uint32_t footer = tail[0] | (std::uint32_t(tail[1]) << 8) |
                                     (std::uint32_t(tail[2]) << 16) |
                                     (std::uint32_t(tail[3]) << 24);
        ok = ok && std::memcmp(magic, "PAR1", 4) == 0 &&
             std::memcmp(tail + 4, "PAR1", 4) == 0 && footer > 0 &&
             footer <= size - 12 && std::fseek(file, 0, SEEK_SET) == 0;
      }
    }
  }
  crc = 0xffffffffU;
  std::uint32_t read_bytes = 0;
  while (ok && read_bytes < size) {
    std::size_t count;
    {
      BusLock lock;
      count = std::fread(block, 1, sizeof(block), file);
    }
    if (!count) {
      ok = false;
      break;
    }
    crc = crc_update(crc, block, count);
    read_bytes += count;
  }
  {
    BusLock lock;
    ok = std::fclose(file) == 0 && ok;
  }
  crc ^= 0xffffffffU;
  return ok && read_bytes == size;
}

void prepare_columns() {
  contract::prepare_columns(writer_state->columns, writer_state->rows);
}

bool station_identity() {
  Preferences settings;
  if (!settings.begin("parquet", false))
    return false;
  if (settings.isKey("station")) {
    settings.getString("station", station_text, sizeof(station_text));
  } else {
    std::uint8_t id[16];
    esp_fill_random(id, sizeof(id));
    id[6] = (id[6] & 0x0f) | 0x40;
    id[8] = (id[8] & 0x3f) | 0x80;
    std::snprintf(
        station_text, sizeof(station_text),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7], id[8], id[9],
        id[10], id[11], id[12], id[13], id[14], id[15]);
    if (settings.putString("station", station_text) != 36) {
      settings.end();
      return false;
    }
  }
  settings.end();
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
  // /sd is the already mounted filesystem; create only descendants.
  for (char *part = copy + 4;; ++part) {
    if (*part != '/' && *part != '\0')
      continue;
    const char saved = *part;
    *part = '\0';
    bool ok;
    {
      BusLock lock;
      ok = ::mkdir(copy, 0700) == 0 || errno == EEXIST;
    }
    *part = saved;
    if (!ok)
      return false;
    if (!saved)
      return true;
  }
}

// Rows per row group and row groups per file for the current rotation window.
// 600 s -> 60-row groups, one per file; 900 s -> 90 x 1; 1800 s -> 90 x 2;
// 3600 s -> 90 x 4. The RAM batch (loss window) never exceeds kMaxRows.
std::size_t rows_per_group() {
  const std::size_t rows = rotation_seconds.load() / 10;
  return rows < kMaxRows ? rows : kMaxRows;
}
std::size_t groups_per_file() {
  const std::size_t groups = rotation_seconds.load() / 900;
  return groups ? (groups < kMaxRowGroups ? groups : kMaxRowGroups) : 1;
}

std::int64_t window_index(const Sample &row) {
  return row.data[event_time_utc_ns] / 1000000000 /
         static_cast<std::int64_t>(rotation_seconds.load());
}

// Whether `row` may join the file/batch that `reference` started: same clock
// epoch, and the same UTC window when dated (never across midnight or a
// clock correction).
bool same_window(const Sample &row, std::int32_t epoch, bool dated,
                 std::int64_t window) {
  if (row.data[clock_epoch] != epoch)
    return false;
  const bool row_dated = row.valid[event_time_utc_ns] != 0;
  if (row_dated != dated)
    return false;
  return !dated || window_index(row) == window;
}

void close_failed(OutputFile &target, const char *operation,
                  const char *reason) {
  ++errors;
  aqlog.printf("PARQUET ERROR operation=%s file=%s reason=%s errno=%d "
               "rows_on_card=%lu\n",
               operation, target.stem, reason ? reason : "io-or-validation",
               errno, static_cast<unsigned long>(target.rows));
  if (target.file) {
    BusLock lock;
    std::fclose(target.file); // the .partial is retained, never repaired
  }
  target.file = nullptr;
  if (!target.benchmark)
    open_rows = open_groups = 0;
}

// Creates `<partition>/<prefix>_<boot>_<first>-<attempt>.partial` from the
// first buffered row and starts the writer.
bool create_file(OutputFile &target, Codec codec, bool benchmark) {
  static std::uint32_t attempt = 0;
  const Sample &first_row = writer_state->rows[0];
  char partition[160], prefix[24];
  target.dated = false;
  if (benchmark) {
    std::snprintf(partition, sizeof(partition), "benchmarks/boot=%s",
                  boot_text);
    std::strcpy(prefix, codec == Codec::Lz4Raw ? "lz4_raw" : "uncompressed");
  } else if (first_row.valid[event_time_utc_ns]) {
    const std::int64_t seconds = first_row.data[event_time_utc_ns] / 1000000000;
    const std::time_t window =
        (seconds / rotation_seconds.load()) * rotation_seconds.load();
    std::tm utc{};
    if (!::gmtime_r(&window, &utc)) {
      ++errors;
      return false;
    }
    std::snprintf(partition, sizeof(partition),
                  "station=%s/year=%04d/month=%02d/day=%02d", station_text,
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
    std::snprintf(prefix, sizeof(prefix), "data_%02d%02d", utc.tm_hour,
                  utc.tm_min);
    target.dated = true;
    target.window = window_index(first_row);
  } else {
    std::snprintf(partition, sizeof(partition), "station=%s/unsynced/boot=%s",
                  station_text, boot_text);
    std::strcpy(prefix, "data_unsynced");
  }
  char directory[192];
  std::snprintf(directory, sizeof(directory), "%s/%s", kDirectory, partition);
  if (!make_directories(directory)) {
    ++errors;
    aqlog.println("PARQUET ERROR operation=mkdir");
    return false;
  }
  target.benchmark = benchmark;
  target.codec = codec;
  target.epoch = static_cast<std::int32_t>(first_row.data[clock_epoch]);
  target.first = target.last = first_row.data[sequence];
  target.rows = 0;
  target.writer_us = target.codec_us = target.sync_us = 0;
  target.attempt = attempt++;
  target.opened_us = esp_timer_get_time();
  std::snprintf(target.stem, sizeof(target.stem), "%s/%s_%s_%lld", partition,
                prefix, boot_text, static_cast<long long>(target.first));
  std::snprintf(target.partial, sizeof(target.partial), "%s/%s-%lu.partial",
                kDirectory, target.stem,
                static_cast<unsigned long>(target.attempt));
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
  std::setvbuf(target.file, target.staging, _IOFBF, 4096);
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
  std::snprintf(interval_text, sizeof(interval_text), "%lu",
                static_cast<unsigned long>(rotation_seconds.load()));
  std::snprintf(groups_text, sizeof(groups_text), "%u",
                unsigned(target.writer.row_groups()));
  const KeyValue metadata[] = {
      {"schema_version", kSchemaName},
      {"device_id", device_text},
      {"station_id", station_text},
      {"boot_id", boot_text},
      {"firmware", kFirmware},
      {"dictionary_version", kDictionaryVersion},
      {"dictionary_uri", kDictionaryUri},
      {"dictionary_sha256", kDictionarySha256},
      {"acquisition_config_id", kConfigurationId},
      {"acquisition_config", kConfiguration},
      {"deployment_id", kUnknown},
      {"calibration_id", kUnknown},
      {"time_semantics", kTimeSemantics},
      {"rotation_interval_s", interval_text},
      {"row_groups", groups_text},
      {"row_group_rows_max", "90"},
      {"compression", codec_name(target.codec)},
      {"purpose",
       target.benchmark ? "codec-comparison-duplicate-rows" : "telemetry"},
      {"board", "CoreS3 ESP32-S3 rev0.2"},
      {"sample_interval_ms", "10000"},
      {"clock", "0=unsynchronized/null,1=host estimate,2=restored from RTC "
                "(earlier host estimate, whole seconds); partitions UTC"},
      {"pms_status", "0=missing,1=warming,2=stale,3=sensor-error,4=valid"},
      {"light_status",
       "0=unavailable,1=not-fresh,2=invalid,3=valid,4=io-error"},
      {"proximity_status",
       "0=unavailable,1=not-fresh,2=saturated,3=valid,4=io-error"},
      {"magnetic_units", "M5Unified BMI270 auxiliary raw counts; uncalibrated"},
      {"unavailable", "SHT20 address collision; battery current unsupported; "
                      "camera/audio not sampled"},
      {"durability", "RAM batch; unfinished rows lost on reset; each row group "
                     "fsynced; footer at finalization; completed files "
                     "retained"}};
  const auto started = esp_timer_get_time();
  const auto result = target.writer.finish(
      metadata, sizeof(metadata) / sizeof(metadata[0]), kFirmware);
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
  std::snprintf(ready, sizeof(ready), "%s/%s-%lld-%lu.parquet", kDirectory,
                target.stem, static_cast<long long>(target.last),
                static_cast<unsigned long>(target.attempt));
  std::uint32_t size = 0, crc = 0;
  if (ok)
    ok = finalized_file(target.partial, size, crc) &&
         size == result.bytes_written;
  if (ok) {
    BusLock lock;
    ok = ::access(ready, F_OK) != 0 && ::rename(target.partial, ready) == 0;
    used_kib = SD.usedBytes() / 1024;
  }
  write_us =
      static_cast<std::uint32_t>(esp_timer_get_time() - target.opened_us);
  if (!ok) {
    close_failed(target, "finalize", "io-or-validation");
    return false;
  }
  ++finalized;
  unlisted_files = true;
  aqlog.printf(
      "PARQUET READY name=%s-%lld-%lu.parquet rows=%u row_groups=%u "
      "first=%lld last=%lld bytes=%lu crc32=%08lx write_us=%lu sync_us=%llu "
      "heap_free=%lu psram_free=%lu stack_free=%u codec=%s codec_us=%llu "
      "writer_us=%llu codec_workspace_bytes=%u heap_min=%lu\n",
      target.stem, static_cast<long long>(target.last),
      static_cast<unsigned long>(target.attempt), unsigned(target.rows),
      unsigned(target.writer.row_groups()),
      static_cast<long long>(target.first), static_cast<long long>(target.last),
      static_cast<unsigned long>(size), static_cast<unsigned long>(crc),
      static_cast<unsigned long>(write_us.load()),
      static_cast<unsigned long long>(target.sync_us),
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(ESP.getFreePsram()),
      unsigned(uxTaskGetStackHighWaterMark(nullptr)), codec_name(target.codec),
      static_cast<unsigned long long>(target.codec_us),
      static_cast<unsigned long long>(target.writer_us),
      unsigned(sizeof(Lz4Workspace)),
      static_cast<unsigned long>(ESP.getMinFreeHeap()));
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
      (finalize || target.writer.row_groups() >= groups_per_file() ||
       target.writer.row_groups() >= kMaxRowGroups))
    return finalize_file(target);
  return true;
}

// Flush everything: RAM batch into a row group, then the footer.
bool write_batch(std::size_t &count) { return commit(count, true); }

// A one-row-group diagnostic copy of the RAM batch outside station trees;
// the rows stay buffered for normal telemetry rotation.
bool write_benchmark(std::size_t count, Codec codec) {
  OutputFile &target = writer_state->benchmark;
  if (!count || target.open())
    return false;
  return create_file(target, codec, true) && append_group(target, count) &&
         finalize_file(target);
}

bool safe_name(const char *name) {
  const std::size_t length = std::strlen(name);
  if (length < 9 || length > 384 || name[0] == '/' ||
      std::strcmp(name + length - 8, ".parquet") != 0)
    return false;
  for (std::size_t i = 0; i < length; ++i)
    if (!((name[i] >= '0' && name[i] <= '9') ||
          (name[i] >= 'a' && name[i] <= 'z') || name[i] == '-' ||
          name[i] == '.' || name[i] == '_' || name[i] == '=' || name[i] == '/'))
      return false;
  return std::strstr(name, "..") == nullptr &&
         std::strstr(name, "//") == nullptr;
}

// One finalized-file entry from a listing. `name` already carries the export
// prefix ("legacy-parquet/" for the pre-Hive directory).
using FileEmitter = void (*)(void *context, const char *name,
                             std::uint32_t bytes);

void emit_file_serial(void *, const char *name, std::uint32_t bytes) {
  aqlog.printf("PARQUET FILE name=%s bytes=%lu\n", name,
               static_cast<unsigned long>(bytes));
}

void list_directory(const char *relative, unsigned depth, unsigned &partials,
                    FileEmitter emit, void *context,
                    const char *base = kDirectory,
                    const char *export_prefix = "") {
  if (depth > 6)
    return;
  char directory_path[416];
  std::snprintf(directory_path, sizeof(directory_path), "%s/%s", base,
                relative);
  DIR *directory;
  {
    BusLock lock;
    directory = ::opendir(directory_path);
  }
  if (!directory) {
    aqlog.println("PARQUET ERROR operation=list");
    return;
  }
  for (;;) {
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
    if (length < 0 || std::size_t(length) >= sizeof(name))
      continue;
    std::snprintf(path, sizeof(path), "%s/%s", base, name);
    struct stat info{};
    int stat_result;
    {
      BusLock lock;
      stat_result = ::stat(path, &info);
    }
    if (stat_result != 0)
      continue;
    if (S_ISDIR(info.st_mode))
      list_directory(name, depth + 1, partials, emit, context, base,
                     export_prefix);
    else if (safe_name(name)) {
      char exported[400];
      std::snprintf(exported, sizeof(exported), "%s%s", export_prefix, name);
      emit(context, exported, static_cast<std::uint32_t>(info.st_size));
    } else if (std::strstr(name, ".partial"))
      ++partials;
  }
  {
    BusLock lock;
    ::closedir(directory);
  }
}

bool partial_name(const char *name) {
  const std::size_t length = std::strlen(name);
  return length > 8 && std::strcmp(name + length - 8, ".partial") == 0;
}

// Move interrupted files out of active station/benchmark trees without
// deleting or pretending to repair them. The relative path is flattened into
// a unique boot-scoped name under output/quarantine/.
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
    char quarantine[416];
    std::snprintf(quarantine, sizeof(quarantine),
                  "%s/quarantine/%s-%08lx.partial", kDirectory, boot_text,
                  static_cast<unsigned long>(quarantined));
    bool ok;
    {
      BusLock lock;
      ok = ::rename(path, quarantine) == 0;
    }
    if (ok) {
      ++quarantined;
      aqlog.printf("PARQUET QUARANTINE source=%s bytes=%llu\n", name,
                   static_cast<unsigned long long>(info.st_size));
    } else {
      ++errors;
      aqlog.printf("PARQUET ERROR operation=quarantine file=%s errno=%d\n",
                   name, errno);
    }
  }
  {
    BusLock lock;
    ::closedir(directory);
  }
}

void count_quarantine(unsigned &count, std::uint64_t &bytes) {
  DIR *directory;
  {
    BusLock lock;
    directory = ::opendir("/sd/output/quarantine");
  }
  if (!directory)
    return;
  for (;;) {
    const struct dirent *entry;
    {
      BusLock lock;
      entry = ::readdir(directory);
    }
    if (!entry)
      break;
    if (entry->d_name[0] == '.' || !partial_name(entry->d_name))
      continue;
    char path[416];
    std::snprintf(path, sizeof(path), "/sd/output/quarantine/%s",
                  entry->d_name);
    struct stat info{};
    {
      BusLock lock;
      if (::stat(path, &info) != 0)
        continue;
    }
    if (!S_ISREG(info.st_mode))
      continue;
    if (count != UINT32_MAX)
      ++count;
    const std::uint64_t file_bytes = static_cast<std::uint64_t>(info.st_size);
    bytes = UINT64_MAX - bytes < file_bytes ? UINT64_MAX : bytes + file_bytes;
  }
  {
    BusLock lock;
    ::closedir(directory);
  }
}

unsigned quarantine_partials() {
  const char *directory = "/sd/output/quarantine";
  if (!make_directories(directory)) {
    ++errors;
    aqlog.println("PARQUET ERROR operation=quarantine-mkdir");
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

// Walks the output tree and the legacy directory; returns retained partials.
unsigned list_all(FileEmitter emit, void *context) {
  unsigned partials = 0;
  list_directory("", 0, partials, emit, context);
  struct stat legacy{};
  bool has_legacy;
  {
    BusLock lock;
    has_legacy = ::stat("/sd/parquet", &legacy) == 0 && S_ISDIR(legacy.st_mode);
  }
  if (has_legacy)
    list_directory("", 0, partials, emit, context, "/sd/parquet",
                   "legacy-parquet/");
  partials_seen = partials;
  return partials;
}

void list_files() {
  const unsigned partials = list_all(emit_file_serial, nullptr);
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
  if (std::strncmp(name, "legacy-parquet/", 15) == 0)
    std::snprintf(path, size, "/sd/parquet/%.369s", name + 15);
  else
    std::snprintf(path, size, "%s/%.384s", kDirectory, name);
  return true;
}

void send_file(const char *name) {
  char path[416];
  if (!resolve_path(name, path, sizeof(path))) {
    aqlog.println("PARQUET ERROR operation=get reason=invalid-name");
    return;
  }
  std::uint32_t size = 0, expected_crc = 0;
  if (!finalized_file(path, size, expected_crc)) {
    aqlog.println("PARQUET ERROR operation=get reason=invalid-file");
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
    delay(1);
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
  switch (source) {
  case kClockHost:
    return "host";
  case kClockRtc:
    return "rtc";
  default:
    return "none";
  }
}

// Shared by `parquet time`, the BLE/LAN SET_TIME op and the boot-time RTC
// seed. False for a bad epoch (before 2020 or after 2100, which also rejects
// an RTC that was never written). Each call starts a new clock epoch; rows
// already captured keep theirs. When an anchor already existed, `skew_ns`
// receives new minus old estimate of the same monotonic instant, i.e. how far
// the previous clock (RTC or earlier host) had drifted from this host.
bool set_clock(std::int64_t seconds, std::int64_t mono, std::int32_t source,
               std::int64_t *skew_ns = nullptr,
               std::int32_t *previous = nullptr) {
  if (seconds < 1577836800LL || seconds > 4102444800LL)
    return false;
  const std::int64_t utc_ns = seconds * 1000000000;
  portENTER_CRITICAL(&clock_mutex);
  if (previous)
    *previous = clock_generation ? clock_source : kClockNone;
  if (skew_ns)
    *skew_ns = clock_generation
                   ? utc_ns - (anchor_utc_ns + (mono - anchor_mono_us) * 1000)
                   : 0;
  anchor_mono_us = mono;
  anchor_utc_ns = utc_ns;
  clock_source = source;
  ++clock_generation;
  portEXIT_CRITICAL(&clock_mutex);
  if (source == kClockHost) {
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

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// days_from_civil); avoids mktime/timegm and the process TZ entirely.
std::int64_t days_from_civil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// Boot-time seed from the BM8563: used only when the chip reports no
// voltage-low event since it was last written and the calendar decodes. The
// chip keeps UTC (never local time), because this device writes it from a UTC
// anchor and the Hive path contract is UTC.
void seed_clock_from_rtc() {
  if (!M5.Rtc.isEnabled()) {
    rtc_state = 2;
    aqlog.println("PARQUET RTC state=absent");
    return;
  }
  if (M5.Rtc.getVoltLow()) {
    rtc_state = 2;
    aqlog.println("PARQUET RTC state=unusable reason=voltage-low");
    return;
  }
  m5::rtc_datetime_t rtc{};
  if (!M5.Rtc.getDateTime(&rtc)) {
    rtc_state = 2;
    aqlog.println("PARQUET RTC state=unusable reason=invalid-calendar");
    return;
  }
  const std::int64_t seconds =
      days_from_civil(rtc.date.year, static_cast<unsigned>(rtc.date.month),
                      static_cast<unsigned>(rtc.date.date)) *
          86400 +
      rtc.time.hours * 3600 + rtc.time.minutes * 60 + rtc.time.seconds;
  const auto mono = esp_timer_get_time();
  if (!set_clock(seconds, mono, kClockRtc)) {
    rtc_state = 2;
    aqlog.printf("PARQUET RTC state=unusable reason=out-of-range "
                 "date=%04d-%02d-%02d\n",
                 rtc.date.year, rtc.date.month, rtc.date.date);
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
  const std::int64_t utc_ns = utc_anchor + (now - mono_anchor) * 1000;
  const std::int64_t fraction_ns = utc_ns % 1000000000;
  const bool overdue = now - rtc_write_requested_us.load() > 5000000LL;
  if (fraction_ns >= 60000000LL && !overdue)
    return;
  rtc_write_pending = false;
  if (!M5.Rtc.isEnabled()) {
    aqlog.println("PARQUET RTC write=skipped reason=absent");
    return;
  }
  // Round to the nearest second when writing late.
  const std::time_t seconds =
      static_cast<std::time_t>((utc_ns + 500000000) / 1000000000);
  std::tm utc{};
  if (!::gmtime_r(&seconds, &utc)) {
    aqlog.println("PARQUET RTC write=failed reason=gmtime");
    return;
  }
  const m5::rtc_datetime_t value(utc);
  M5.Rtc.setDateTime(value);
  m5::rtc_datetime_t readback{};
  const bool ok = M5.Rtc.getDateTime(&readback) &&
                  readback.date.year == value.date.year &&
                  readback.date.month == value.date.month &&
                  readback.date.date == value.date.date &&
                  readback.time.hours == value.time.hours &&
                  readback.time.minutes == value.time.minutes;
  rtc_state = ok ? 1 : 2;
  aqlog.printf("PARQUET RTC write=%s utc=%04d-%02d-%02dT%02d:%02d:%02dZ "
               "late_ms=%lld%s\n",
               ok ? "ok" : "readback-mismatch", utc.tm_year + 1900,
               utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
               static_cast<long long>(fraction_ns / 1000000),
               overdue ? " overdue=true" : "");
}

// ---- BLE `status` document and control handlers
// (docs/shared/ble-sync-protocol.md)

std::size_t build_status_json(char *out, std::size_t size) {
  std::int32_t generation, source;
  portENTER_CRITICAL(&clock_mutex);
  generation = clock_generation;
  source = clock_source;
  portEXIT_CRITICAL(&clock_mutex);
  const int written = std::snprintf(
      out, size,
      "{\"up_s\":%lu,\"int_s\":%lu,\"buf\":%lu,\"fin\":%lu,\"drop\":%lu,"
      "\"err\":%lu,\"miss\":%lld,\"fail\":%u,\"codec\":\"%s\",\"utc\":%u,"
      "\"gen\":%ld,\"clk\":%ld,\"rtc\":%ld,\"sd\":%u,\"sd_kib\":%lu,"
      "\"sd_used_kib\":%lu,\"heap\":%lu,\"part\":%lu,\"qf\":%lu,"
      "\"qb\":%llu,\"open\":%lu,\"open_rg\":%lu}",
      static_cast<unsigned long>(esp_timer_get_time() / 1000000),
      static_cast<unsigned long>(rotation_seconds.load()),
      static_cast<unsigned long>(buffered.load()),
      static_cast<unsigned long>(finalized.load()),
      static_cast<unsigned long>(dropped.load()),
      static_cast<unsigned long>(errors.load()),
      static_cast<long long>(missed_deadlines.load()),
      worker_failed.load() ? 1U : 0U, codec_name(selected_codec),
      generation ? 1U : 0U, static_cast<long>(generation),
      static_cast<long>(generation ? source : kClockNone),
      static_cast<long>(rtc_state.load()), storage_ok.load() ? 1U : 0U,
      static_cast<unsigned long>(total_kib.load()),
      static_cast<unsigned long>(used_kib.load()),
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(partials_seen.load()),
      static_cast<unsigned long>(partials_quarantined.load()),
      static_cast<unsigned long long>(quarantine_bytes.load()),
      static_cast<unsigned long>(open_rows.load()),
      static_cast<unsigned long>(open_groups.load()));
  return written > 0 && std::size_t(written) < size ? std::size_t(written) : 0;
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

// Builds the `live` snapshot from the row that was just queued. Keys are
// omitted for null values; the document must stay <= kMaxJson bytes.
void publish_live(const Sample &row) {
  char json[ble::kMaxJson + 96];
  std::size_t used = 0;
  auto put = [&](const char *format, auto... args) {
    if (used >= sizeof(json))
      return;
    const int written =
        std::snprintf(json + used, sizeof(json) - used, format, args...);
    if (written > 0)
      used += std::size_t(written);
  };
  auto i32 = [&](Field field) {
    std::int32_t value;
    std::memcpy(&value, &row.data[field], sizeof(value));
    return static_cast<long>(value);
  };
  auto i64 = [&](Field field) {
    return static_cast<long long>(row.data[field]);
  };
  put("{\"seq\":%lld,\"mono\":%lld", i64(sequence), i64(monotonic_us));
  if (row.valid[event_time_utc_ns])
    put(",\"utc\":%lld", i64(event_time_utc_ns));
  put(",\"pms\":%ld", i32(pms_status));
  if (row.valid[pm25_atmospheric_ug_m3]) {
    put(",\"pm1\":%ld,\"pm25\":%ld,\"pm10\":%ld", i32(pm1_atmospheric_ug_m3),
        i32(pm25_atmospheric_ug_m3), i32(pm10_atmospheric_ug_m3));
    put(",\"c1\":%ld,\"c25\":%ld,\"c10\":%ld", i32(pm1_cf1_ug_m3),
        i32(pm25_cf1_ug_m3), i32(pm10_cf1_ug_m3));
    put(",\"n03\":%ld,\"n05\":%ld,\"n1\":%ld,\"n25\":%ld,\"n5\":%ld,\"n10\":%"
        "ld",
        i32(particles_gt03_per_01l), i32(particles_gt05_per_01l),
        i32(particles_gt10_per_01l), i32(particles_gt25_per_01l),
        i32(particles_gt50_per_01l), i32(particles_gt100_per_01l));
  }
  if (row.valid[imu_temperature_c]) {
    float temperature;
    std::memcpy(&temperature, &row.data[imu_temperature_c],
                sizeof(temperature));
    put(",\"t\":%.1f", static_cast<double>(temperature));
  }
  if (row.valid[battery_mv])
    put(",\"bat\":%ld", i32(battery_mv));
  if (row.valid[battery_percent])
    put(",\"pct\":%ld", i32(battery_percent));
  if (row.valid[charging_status])
    put(",\"chg\":%ld", i32(charging_status));
  if (row.valid[vbus_mv])
    put(",\"vbus\":%ld", i32(vbus_mv));
  if (row.valid[light_ch0_raw])
    put(",\"als\":%ld", i32(light_ch0_raw));
  put("}");
  if (used < sizeof(json) && used <= ble::kMaxJson) {
    ble::publish_live(json, used);
    lan::publish_live(json, used);
  }
}

struct OpenFile {
  FILE *file = nullptr;
  std::uint16_t handle = 0;
  std::uint32_t size = 0;
  std::uint32_t crc = 0;
  std::uint32_t generation = 0;
  ble::Link link = ble::Link::Ble;
};
OpenFile open_file;
std::uint16_t next_handle = 1;

// The request being executed; handlers answer on its link.
const ble::ControlRequest *current_request = nullptr;

std::uint32_t link_generation(ble::Link link) {
  return link == ble::Link::Lan ? lan::connection_generation()
                                : ble::connection_generation();
}
std::uint16_t link_payload_max() {
  if (current_request && current_request->link == ble::Link::Lan)
    return lan::payload_max();
  // GATT attribute values are at most 512 bytes; Android's stack silently
  // discards larger notifications while macOS accepts them (measured
  // 2026-09-17: every 514-byte CHUNK vanished on a OnePlus, the short final
  // chunk arrived). So a BLE frame never exceeds 512 even at MTU 517.
  const std::uint16_t raw = ble::payload_max();
  return raw > ble::kMaxFrame ? ble::kMaxFrame : raw;
}
bool respond(const std::uint8_t *frame, std::size_t length) {
  // A long LIST or READ is progress, not a stall: stamp the heartbeat per
  // frame so the stall detector only fires when sending truly stops.
  worker_heartbeat_us = esp_timer_get_time();
  return current_request && current_request->link == ble::Link::Lan
             ? lan::send_response(frame, length)
             : ble::send_response(frame, length);
}
bool respond_error(ble::Op op, ble::Error code, const char *detail) {
  return current_request && current_request->link == ble::Link::Lan
             ? lan::send_error(op, code, detail)
             : ble::send_error(op, code, detail);
}
// A handle is only honoured on the link that opened it.
bool handle_matches(std::uint16_t handle) {
  return open_file.file && handle == open_file.handle && current_request &&
         current_request->link == open_file.link;
}

void close_open_file() {
  if (open_file.file) {
    BusLock lock;
    std::fclose(open_file.file);
  }
  open_file = OpenFile{};
}

// Drop the handle when the connection that opened it is gone.
void reconcile_open_file() {
  if (open_file.file && open_file.generation != link_generation(open_file.link))
    close_open_file();
}

void put_u16(std::uint8_t *out, std::uint16_t value) {
  out[0] = value & 0xff;
  out[1] = value >> 8;
}
void put_u32(std::uint8_t *out, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    out[i] = (value >> (8 * i)) & 0xff;
}
void put_i64(std::uint8_t *out, std::int64_t value) {
  const auto bits = static_cast<std::uint64_t>(value);
  for (unsigned i = 0; i < 8; ++i)
    out[i] = (bits >> (8 * i)) & 0xff;
}
std::uint16_t get_u16(const std::uint8_t *in) {
  return static_cast<std::uint16_t>(in[0] | (in[1] << 8));
}
std::uint32_t get_u32(const std::uint8_t *in) {
  return std::uint32_t(in[0]) | (std::uint32_t(in[1]) << 8) |
         (std::uint32_t(in[2]) << 16) | (std::uint32_t(in[3]) << 24);
}
std::int64_t get_i64(const std::uint8_t *in) {
  std::uint64_t bits = 0;
  for (unsigned i = 0; i < 8; ++i)
    bits |= std::uint64_t(in[i]) << (8 * i);
  return static_cast<std::int64_t>(bits);
}

struct ListContext {
  std::uint16_t count = 0;
  bool ok = true;
};

void emit_file_ble(void *context, const char *name, std::uint32_t bytes) {
  auto *list = static_cast<ListContext *>(context);
  if (!list->ok)
    return;
  const std::size_t name_length = std::strlen(name);
  const std::size_t payload_max = link_payload_max();
  if (payload_max < 6 || name_length + 5 > payload_max) {
    // Cannot fit this entry; the phone will not see it. Counted honestly.
    aqlog.printf("BLE LIST SKIP name_bytes=%u payload_max=%u\n",
                 unsigned(name_length), unsigned(payload_max));
    return;
  }
  std::uint8_t frame[5 + 400];
  frame[0] = ble::kFrameFile;
  put_u32(frame + 1, bytes);
  std::memcpy(frame + 5, name, name_length);
  if (!respond(frame, 5 + name_length)) {
    // The peer will see no LIST_END and time out; say so on serial.
    aqlog.printf("BLE LIST ABORT after=%u reason=notify-refused\n",
                 unsigned(list->count));
    list->ok = false;
    return;
  }
  ++list->count;
}

void ble_list() {
  ListContext list;
  const unsigned partials = list_all(emit_file_ble, &list);
  if (!list.ok)
    return;
  std::uint8_t frame[13];
  frame[0] = ble::kFrameListEnd;
  put_u16(frame + 1, list.count);
  put_u16(frame + 3,
          static_cast<std::uint16_t>(partials > 65535 ? 65535 : partials));
  put_u32(frame + 5, total_kib.load());
  put_u32(frame + 9, used_kib.load());
  respond(frame, sizeof(frame));
  unlisted_files = false;
  publish_advert();
}

void ble_open(const std::uint8_t *name_bytes, std::size_t name_length) {
  char name[400];
  if (name_length == 0 || name_length >= sizeof(name)) {
    respond_error(ble::kOpOpen, ble::kErrInvalidName, "length");
    return;
  }
  std::memcpy(name, name_bytes, name_length);
  name[name_length] = '\0';
  char path[416];
  if (!resolve_path(name, path, sizeof(path))) {
    respond_error(ble::kOpOpen, ble::kErrInvalidName, "charset");
    return;
  }
  close_open_file();
  std::uint32_t size = 0, crc = 0;
  if (!finalized_file(path, size, crc)) {
    respond_error(ble::kOpOpen, ble::kErrNotFinalized, name);
    return;
  }
  FILE *file;
  {
    BusLock lock;
    file = std::fopen(path, "rb");
  }
  if (!file) {
    respond_error(ble::kOpOpen, ble::kErrOpenFailed, name);
    return;
  }
  open_file.file = file;
  open_file.handle = next_handle++;
  if (next_handle == 0)
    next_handle = 1;
  open_file.size = size;
  open_file.crc = crc;
  open_file.link = current_request ? current_request->link : ble::Link::Ble;
  open_file.generation = link_generation(open_file.link);
  std::uint8_t frame[11 + 400];
  frame[0] = ble::kFrameOpened;
  put_u16(frame + 1, open_file.handle);
  put_u32(frame + 3, size);
  put_u32(frame + 7, crc);
  std::memcpy(frame + 11, name, name_length);
  aqlog.printf("BLE OPEN handle=%u bytes=%lu crc32=%08lx name=%s\n",
               unsigned(open_file.handle), static_cast<unsigned long>(size),
               static_cast<unsigned long>(crc), name);
  respond(frame, 11 + name_length);
}

void ble_read(std::uint16_t handle, std::uint32_t offset,
              std::uint32_t length) {
  auto end = [&](std::uint32_t next, ble::Error status) {
    std::uint8_t frame[8];
    frame[0] = ble::kFrameReadEnd;
    put_u16(frame + 1, handle);
    put_u32(frame + 3, next);
    frame[7] = static_cast<std::uint8_t>(status);
    respond(frame, sizeof(frame));
  };
  if (!handle_matches(handle)) {
    respond_error(ble::kOpRead, ble::kErrBadHandle, nullptr);
    return;
  }
  if (offset > open_file.size) {
    respond_error(ble::kOpRead, ble::kErrRange, nullptr);
    return;
  }
  if (length > ble::kMaxRead)
    length = ble::kMaxRead;
  if (offset + length > open_file.size)
    length = open_file.size - offset;
  const std::size_t payload_max = link_payload_max();
  if (payload_max <= 7) {
    end(offset, ble::kErrBusy);
    return;
  }
  constexpr std::size_t kChunkCap = lan::kPayloadMax - 7;
  const std::size_t chunk_max =
      payload_max - 7 > kChunkCap ? kChunkCap : payload_max - 7;
  bool seek_ok;
  {
    BusLock lock;
    seek_ok =
        std::fseek(open_file.file, static_cast<long>(offset), SEEK_SET) == 0;
  }
  if (!seek_ok) {
    end(offset, ble::kErrOpenFailed);
    return;
  }
  std::uint8_t frame[7 + kChunkCap];
  std::uint32_t sent = 0;
  while (sent < length) {
    const std::size_t want =
        length - sent < chunk_max ? std::size_t(length - sent) : chunk_max;
    std::size_t count;
    {
      BusLock lock;
      count = std::fread(frame + 7, 1, want, open_file.file);
    }
    if (count == 0) {
      end(offset + sent, ble::kErrOpenFailed);
      return;
    }
    frame[0] = ble::kFrameChunk;
    put_u16(frame + 1, handle);
    put_u32(frame + 3, offset + sent);
    if (!respond(frame, 7 + count)) {
      end(offset + sent, ble::kErrBusy);
      return;
    }
    sent += count;
  }
  aqlog.printf("BLE READ handle=%u offset=%lu bytes=%lu\n", unsigned(handle),
               static_cast<unsigned long>(offset),
               static_cast<unsigned long>(sent));
  end(offset + sent, static_cast<ble::Error>(0));
}

void ble_close(std::uint16_t handle) {
  if (!handle_matches(handle)) {
    respond_error(ble::kOpClose, ble::kErrBadHandle, nullptr);
    return;
  }
  close_open_file();
  std::uint8_t frame[3];
  frame[0] = ble::kFrameClosed;
  put_u16(frame + 1, handle);
  respond(frame, sizeof(frame));
}

void send_config() {
  const lan::Status wifi = lan::status();
  config::WifiView view{wifi.state,
                        wifi.ip,
                        wifi.rssi,
                        wifi.mac,
                        wifi.authenticated ? 1U : 0U,
                        wifi.host,
                        ble::link().bonds};
  std::uint8_t frame[2 + 400];
  const std::size_t length =
      config::build_json(reinterpret_cast<char *>(frame + 2), 400, view);
  if (!length) {
    respond_error(ble::kOpGetConfig, ble::kErrMalformed, "json");
    return;
  }
  frame[0] = ble::kFrameConfig;
  frame[1] = config::reboot_required() ? 1 : 0;
  respond(frame, 2 + length);
}

void send_log_tail(std::uint16_t max_bytes) {
  static char text[DebugLog::kRingBytes];
  std::uint32_t total = 0;
  const std::size_t wanted =
      max_bytes > DebugLog::kRingBytes ? DebugLog::kRingBytes : max_bytes;
  const std::size_t count = aqlog.tail(text, wanted, total);
  const std::size_t payload_max = link_payload_max();
  std::uint8_t frame[1 + lan::kPayloadMax];
  std::size_t sent = 0;
  if (payload_max > 1) {
    const std::size_t slice_max =
        payload_max - 1 > lan::kPayloadMax ? lan::kPayloadMax : payload_max - 1;
    while (sent < count) {
      const std::size_t slice =
          count - sent < slice_max ? count - sent : slice_max;
      frame[0] = ble::kFrameLog;
      std::memcpy(frame + 1, text + sent, slice);
      if (!respond(frame, 1 + slice))
        break;
      sent += slice;
    }
  }
  std::uint8_t end[7];
  end[0] = ble::kFrameLogEnd;
  put_u32(end + 1, total);
  put_u16(end + 5, static_cast<std::uint16_t>(sent));
  respond(end, sizeof(end));
}

struct WorkerState {
  std::size_t &count;
  bool &failed;
  bool storage_ready;
};

void handle_control_request(const ble::ControlRequest &request,
                            WorkerState &state) {
  reconcile_open_file();
  const std::uint8_t *body = request.bytes + 1;
  const std::size_t body_length = request.length - 1;
  switch (request.bytes[0]) {
  case ble::kOpList:
    if (!state.storage_ready) {
      respond_error(ble::kOpList, ble::kErrStorage, nullptr);
      return;
    }
    ble_list();
    return;
  case ble::kOpOpen:
    if (!state.storage_ready) {
      respond_error(ble::kOpOpen, ble::kErrStorage, nullptr);
      return;
    }
    ble_open(body, body_length);
    return;
  case ble::kOpRead:
    if (body_length != 10) {
      respond_error(ble::kOpRead, ble::kErrMalformed, nullptr);
      return;
    }
    ble_read(get_u16(body), get_u32(body + 2), get_u32(body + 6));
    return;
  case ble::kOpClose:
    if (body_length != 2) {
      respond_error(ble::kOpClose, ble::kErrMalformed, nullptr);
      return;
    }
    ble_close(get_u16(body));
    return;
  case ble::kOpSetTime: {
    if (body_length != 8) {
      respond_error(ble::kOpSetTime, ble::kErrMalformed, nullptr);
      return;
    }
    const std::int64_t seconds = get_i64(body);
    std::int64_t skew_ns = 0;
    std::int32_t previous = kClockNone;
    if (!set_clock(seconds, request.received_mono_us, kClockHost, &skew_ns,
                   &previous)) {
      respond_error(ble::kOpSetTime, ble::kErrInvalidEpoch, nullptr);
      return;
    }
    report_host_clock(request.link == ble::Link::Ble ? "ble" : "lan", seconds,
                      request.received_mono_us, previous, skew_ns);
    std::uint8_t frame[17];
    frame[0] = ble::kFrameTimeSet;
    put_i64(frame + 1, seconds);
    put_i64(frame + 9, request.received_mono_us);
    respond(frame, sizeof(frame));
    publish_status();
    return;
  }
  case ble::kOpFlush: {
    if ((!state.count && !writer_state->telemetry.open()) ||
        !state.storage_ready) {
      respond_error(ble::kOpFlush, ble::kErrNothingToFlush, nullptr);
      return;
    }
    // Rows reported: RAM batch plus row groups already on the card.
    const auto rows =
        static_cast<std::uint16_t>(state.count + open_rows.load());
    if (!write_batch(state.count)) {
      state.failed = true;
      worker_failed = true;
      respond_error(ble::kOpFlush, ble::kErrStorage, "write");
      publish_status();
      return;
    }
    state.failed = false;
    worker_failed = false;
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
  case ble::kOpGetConfig:
    send_config();
    return;
  case ble::kOpSetConfig: {
    char bad_key[48];
    config::Actions actions;
    if (!config::apply_lines(reinterpret_cast<const char *>(body), body_length,
                             bad_key, sizeof(bad_key), actions)) {
      respond_error(ble::kOpSetConfig, ble::kErrInvalidConfig, bad_key);
      return;
    }
    if (actions.clear_bonds)
      ble::clear_bonds();
    if (actions.rotate_token)
      lan::drop_session();
    if (actions.wifi_changed)
      lan::apply_settings();
    send_config();
    return;
  }
  case ble::kOpReboot: {
    // Never lose the RAM batch to a reboot the owner asked for.
    if ((state.count || writer_state->telemetry.open()) &&
        state.storage_ready && !state.failed) {
      if (!write_batch(state.count)) {
        state.failed = true;
        worker_failed = true;
      }
    }
    constexpr std::uint16_t kDelayMs = 500;
    std::uint8_t frame[3];
    frame[0] = ble::kFrameRebooting;
    put_u16(frame + 1, kDelayMs);
    respond(frame, sizeof(frame));
    aqlog.printf("PARQUET REBOOT source=%s delay_ms=%u\n",
                 request.link == ble::Link::Lan ? "lan" : "ble",
                 unsigned(kDelayMs));
    Serial.flush();
    vTaskDelay(pdMS_TO_TICKS(kDelayMs));
    esp_restart();
    return;
  }
  case ble::kOpLogTail: {
    if (body_length != 2) {
      respond_error(ble::kOpLogTail, ble::kErrMalformed, nullptr);
      return;
    }
    send_log_tail(get_u16(body));
    return;
  }
  case ble::kOpGetToken: {
    if (request.link != ble::Link::Ble) {
      respond_error(ble::kOpGetToken, ble::kErrNotOnThisLink, "ble-only");
      return;
    }
    const config::Settings settings = config::get();
    std::uint8_t frame[3 + config::kTokenBytes];
    frame[0] = ble::kFrameToken;
    put_u16(frame + 1, config::kLanPort);
    std::memcpy(frame + 3, settings.token, config::kTokenBytes);
    respond(frame, sizeof(frame));
    aqlog.println("LAN TOKEN issued=ble");
    return;
  }
  case ble::kOpWifiScan:
    // Normally intercepted in enqueue_request(); reaching here means the
    // scan task already had one pending.
    respond_error(ble::kOpWifiScan, ble::kErrBusy, "scan-pending");
    return;
  default:
    respond_error(static_cast<ble::Op>(request.bytes[0]), ble::kErrUnknownOp,
                  nullptr);
  }
}

void storage_worker(void *) {
  bool storage_ready;
  {
    BusLock lock;
    storage_ready =
        mounted && (::mkdir(kDirectory, 0700) == 0 || errno == EEXIST);
  }
  storage_ok = storage_ready;
  if (storage_ready) {
    {
      BusLock lock;
      total_kib = SD.totalBytes() / 1024;
      used_kib = SD.usedBytes() / 1024;
    }
    const unsigned quarantined = quarantine_partials();
    aqlog.printf("PARQUET RECOVERY quarantined=%u repaired=0 deleted=0\n",
                 quarantined);
    list_files();
  } else {
    ++errors;
    aqlog.println("PARQUET ERROR operation=mount-or-directory "
                  "samples_will_be_dropped=true");
  }
  std::size_t count = 0;
  bool failed = false;
  Sample row;
  Command command;
  for (;;) {
    worker_heartbeat_us = esp_timer_get_time();
    if (xQueueReceive(samples, &row, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (!storage_ready || failed || count == kMaxRows) {
        ++dropped;
      } else {
        // Never mix clock epochs or UTC windows (including midnight) in a
        // file: the open file, or else the RAM batch, is the reference.
        const OutputFile &open = writer_state->telemetry;
        bool fits = true;
        if (open.open())
          fits = same_window(row, open.epoch, open.dated, open.window);
        else if (count) {
          const auto &previous = writer_state->rows[0];
          fits = same_window(
              row, previous.data[clock_epoch],
              previous.valid[event_time_utc_ns] != 0,
              previous.valid[event_time_utc_ns] ? window_index(previous) : 0);
        }
        if (!fits && !write_batch(count)) {
          failed = true;
          ++dropped;
          continue;
        }
        writer_state->rows[count++] = row;
        buffered = count;
        if (count >= rows_per_group() && !commit(count, false))
          failed = true;
      }
    }
    worker_failed = failed;
    if (xQueueReceive(commands, &command, 0) != pdTRUE)
      continue;
    if (command.source == Command::Source::Control) {
      WorkerState state{count, failed, storage_ready};
      current_request = &command.control;
      handle_control_request(command.control, state);
      current_request = nullptr;
      continue;
    }
    if (std::strcmp(command.text, "parquet status") == 0) {
      aqlog.printf("PARQUET STATUS interval_s=%lu buffered=%u open_rows=%lu "
                   "open_groups=%lu finalized=%lu "
                   "dropped=%lu errors=%lu queue_peak=%lu failed=%s station=%s "
                   "codec=%s schema=%s config=%s deployment=unknown "
                   "calibration=unknown\n",
                   static_cast<unsigned long>(rotation_seconds.load()),
                   unsigned(count),
                   static_cast<unsigned long>(open_rows.load()),
                   static_cast<unsigned long>(open_groups.load()),
                   static_cast<unsigned long>(finalized.load()),
                   static_cast<unsigned long>(dropped.load()),
                   static_cast<unsigned long>(errors.load()),
                   static_cast<unsigned long>(queue_peak.load()),
                   failed ? "true" : "false", station_text,
                   codec_name(selected_codec), kSchemaName, kConfigurationId);
    } else if (std::strcmp(command.text, "parquet schema") == 0) {
      aqlog.printf("PARQUET SCHEMA BEGIN schema=%s columns=%u sha256=%s\n",
                   kSchemaName, unsigned(field_count), kDictionarySha256);
      for (const auto &field : kFields) {
        aqlog.printf("PARQUET FIELD name=%s type=%u procedure=%s unit=%s "
                     "validity=%s property=%s\n",
                     field.name, unsigned(field.type), field.procedure,
                     field.unit, field.validity, field.property_uri);
        delay(1);
      }
      aqlog.println("PARQUET SCHEMA END");
    } else if (std::strcmp(command.text, "parquet codec-test") == 0) {
      if (!count || !storage_ready || failed) {
        aqlog.println("PARQUET ERROR operation=codec-test "
                      "reason=empty-or-storage-failed");
      } else if (write_benchmark(count, Codec::Uncompressed) &&
                 write_benchmark(count, Codec::Lz4Raw)) {
        // Keep the original batch for normal telemetry rotation. Benchmark
        // copies live outside station trees and are explicitly labeled.
        aqlog.printf("PARQUET BENCH END rows=%u retained_for_telemetry=true\n",
                     unsigned(count));
      }
    } else if (std::strcmp(command.text, "parquet codec none") == 0 ||
               std::strcmp(command.text, "parquet codec lz4") == 0) {
      // A file holds one codec: finish the open one before switching.
      if (!write_batch(count)) {
        failed = true;
        continue;
      }
      failed = false;
      selected_codec =
          command.text[14] == 'l' ? Codec::Lz4Raw : Codec::Uncompressed;
      aqlog.printf("PARQUET CONFIG codec=%s persistent=false\n",
                   codec_name(selected_codec));
    } else if (std::strcmp(command.text, "parquet flush") == 0) {
      if ((count || writer_state->telemetry.open()) && storage_ready) {
        if (write_batch(count))
          failed = false;
      } else
        aqlog.println(
            "PARQUET ERROR operation=flush reason=empty-or-no-storage");
    } else if (std::strncmp(command.text, "parquet interval ", 17) == 0 &&
               (std::strcmp(command.text + 17, "600") == 0 ||
                std::strcmp(command.text + 17, "900") == 0 ||
                std::strcmp(command.text + 17, "1800") == 0 ||
                std::strcmp(command.text + 17, "3600") == 0)) {
      // The window defines the file: finish the open one first.
      if (!write_batch(count)) {
        failed = true;
        continue;
      }
      failed = false;
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

void collect(const PmsSnapshot &pms, std::int64_t now, std::int64_t scheduled) {
  Sample row;
  row.integer(schema_version, kSchemaVersion);
  row.counter(device_id, device);
  row.counter(boot_id_hi, boot_hi);
  row.counter(boot_id_lo, boot_lo);
  row.counter(sequence, sample_sequence++);
  row.counter(monotonic_us, now);
  row.counter(scheduled_us, scheduled);
  row.counter(sample_jitter_us, now - scheduled);
  std::int64_t utc_anchor, mono_anchor;
  std::int32_t generation, source;
  portENTER_CRITICAL(&clock_mutex);
  utc_anchor = anchor_utc_ns;
  mono_anchor = anchor_mono_us;
  generation = clock_generation;
  source = clock_source;
  portEXIT_CRITICAL(&clock_mutex);
  apply_clock(row, now, mono_anchor, utc_anchor, generation, source);
  const int status = !pms.present        ? 0
                     : now < 30000000    ? 1
                     : pms.age_ms > 5000 ? 2
                     : pms.error         ? 3
                                         : 4;
  row.integer(pms_status, status);
  if (pms.present) {
    row.counter(pms_age_ms, pms.age_ms);
    row.counter(pms_received_mono_us, pms.received_mono_us);
    row.integer(pms_firmware, pms.firmware);
    row.integer(pms_error, pms.error);
  }
  if (status == 4)
    for (std::size_t i = 0; i < 12; ++i)
      row.integer(static_cast<Field>(pm1_cf1_ug_m3 + i), pms.values[i]);
  row.counter(pms_frames, pms.frames);
  row.counter(pms_checksum_errors, pms.checksum_errors);
  row.counter(pms_length_errors, pms.length_errors);
  const auto fresh =
      M5.Imu.isEnabled() ? M5.Imu.update() : m5::IMU_Class::sensor_mask_none;
  row.integer(imu_fresh_mask, fresh);
  m5::IMU_Class::imu_data_t imu{};
  M5.Imu.getImuData(&imu);
  if (fresh & m5::IMU_Class::sensor_mask_accel) {
    row.number(accel_x_g, imu.accel.x);
    row.number(accel_y_g, imu.accel.y);
    row.number(accel_z_g, imu.accel.z);
  }
  if (fresh & m5::IMU_Class::sensor_mask_gyro) {
    row.number(gyro_x_dps, imu.gyro.x);
    row.number(gyro_y_dps, imu.gyro.y);
    row.number(gyro_z_dps, imu.gyro.z);
  }
  if (fresh & m5::IMU_Class::sensor_mask_mag) {
    row.integer(mag_x_raw, M5.Imu.getRawData(6));
    row.integer(mag_y_raw, M5.Imu.getRawData(7));
    row.integer(mag_z_raw, M5.Imu.getRawData(8));
  }
  float temperature;
  if (M5.Imu.isEnabled() && M5.Imu.getTemp(&temperature))
    row.number(imu_temperature_c, temperature);
  const auto light = light_sensor.read();
  row.integer(light_status, !light_sensor.available() ? 0
                            : light.io_error          ? 4
                            : !light.als_fresh        ? 1
                            : light.als_valid         ? 3
                                                      : 2);
  row.integer(proximity_status, !light_sensor.available() ? 0
                                : light.io_error          ? 4
                                : !light.proximity_fresh  ? 1
                                : light.proximity_valid   ? 3
                                                          : 2);
  if (light.als_valid) {
    row.integer(light_ch0_raw, light.als_ch0);
    row.integer(light_ch1_raw, light.als_ch1);
  }
  if (light.proximity_valid)
    row.integer(proximity_raw, light.proximity);
  row.integer(vbus_mv, M5.Power.getVBUSVoltage());
  row.integer(battery_mv, M5.Power.getBatteryVoltage());
  const int battery = M5.Power.getBatteryLevel();
  if (battery >= 0 && battery <= 100)
    row.integer(battery_percent, battery);
  row.integer(charging_status, M5.Power.isCharging());
  // CoreS3 getBatteryCurrent() is an unsupported constant zero. Leave null.
  row.integer(external_5v_enabled, M5.Power.getExtOutput());
  row.integer(usb_output_enabled, M5.Power.getUsbOutput());
  m5::rtc_datetime_t rtc{};
  const bool rtc_ok = M5.Rtc.isEnabled() && M5.Rtc.getDateTime(&rtc);
  row.integer(rtc_read_ok, rtc_ok);
  if (rtc_ok) {
    row.integer(rtc_date_yyyymmdd,
                rtc.date.year * 10000 + rtc.date.month * 100 + rtc.date.date);
    row.integer(rtc_time_hhmmss, rtc.time.hours * 10000 +
                                     rtc.time.minutes * 100 + rtc.time.seconds);
  }
  row.integer(touch_points, M5.Touch.getCount());
  if (M5.Touch.getCount()) {
    const auto &touch = M5.Touch.getDetail();
    row.integer(touch_x_px, touch.x);
    row.integer(touch_y_px, touch.y);
  }
  row.counter(heap_free_bytes, ESP.getFreeHeap());
  row.counter(heap_min_free_bytes, ESP.getMinFreeHeap());
  row.counter(psram_free_bytes, ESP.getFreePsram());
  if (mounted) {
    row.counter(sd_total_bytes, std::int64_t(total_kib.load()) * 1024);
    row.counter(sd_used_bytes, std::int64_t(used_kib.load()) * 1024);
  }
  row.counter(rows_dropped, dropped.load());
  row.counter(sample_deadlines_missed, missed_deadlines.load());
  row.counter(storage_errors, errors.load());
  row.counter(files_finalized, finalized.load());
  row.counter(last_write_us, write_us.load());
  row.counter(last_sync_us, sync_us.load());
  row.integer(queue_high_water, queue_peak.load());
  row.counter(collection_completed_mono_us, esp_timer_get_time());
  if (xQueueSend(samples, &row, 0) != pdTRUE)
    ++dropped;
  const auto depth =
      static_cast<std::uint32_t>(uxQueueMessagesWaiting(samples));
  if (depth > queue_peak.load())
    queue_peak = depth;
  publish_live(row);
  publish_status();
  aqlog.printf("PARQUET ROW sequence=%lld monotonic_us=%lld jitter_us=%lld "
               "pms_status=%d imu_mask=%u light_valid=%s proximity_valid=%s "
               "buffered=%lu dropped=%lu\n",
               static_cast<long long>(row.data[sequence]),
               static_cast<long long>(now),
               static_cast<long long>(now - scheduled), status, unsigned(fresh),
               light.als_valid ? "true" : "false",
               light.proximity_valid ? "true" : "false",
               static_cast<unsigned long>(buffered.load()),
               static_cast<unsigned long>(dropped.load()));
}
} // namespace

void lock_display() {
  if (spi_mutex)
    xSemaphoreTake(spi_mutex, portMAX_DELAY);
}
void unlock_display() {
  if (spi_mutex)
    xSemaphoreGive(spi_mutex);
}

void begin_logger(bool sd_mounted) {
  mounted = sd_mounted;
  if (!station_identity()) {
    aqlog.println("PARQUET ERROR operation=station-identity logging=false");
    return;
  }
  std::uint8_t mac[6]{};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    aqlog.println("PARQUET ERROR operation=identity");
    return;
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
  samples = xQueueCreate(8, sizeof(Sample));
  commands = xQueueCreate(6, sizeof(Command));
  writer_state = static_cast<WriterState *>(heap_caps_calloc(
      1, sizeof(WriterState), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!spi_mutex || !samples || !commands || !writer_state) {
    aqlog.println("PARQUET ERROR operation=allocate logging=false");
    return;
  }
  new (writer_state) WriterState{};
  writer_state->telemetry.staging = staging_telemetry;
  writer_state->benchmark.staging = staging_benchmark;
  prepare_columns();
  const bool light = light_sensor.begin();
  // Before the worker starts, so the first row of the boot is already dated
  // when the RTC holds an earlier host sync.
  seed_clock_from_rtc();
  next_sample_us = esp_timer_get_time() + kSampleUs;
  if (xTaskCreate(storage_worker, "parquet-sd", 24576, nullptr, 1,
                  &worker_task) != pdPASS) {
    aqlog.println("PARQUET ERROR operation=task logging=false");
    return;
  }
  accepting = true;
  aqlog.printf(
      "PARQUET BEGIN schema=%s columns=%u sample_s=10 "
      "interval_s=900 max_rows=90 max_row_groups=%u psram_workspace_bytes=%u "
      "row_bytes=%u boot=%s station=%s light_available=%s "
      "codec=UNCOMPRESSED\n",
      kSchemaName, unsigned(field_count), unsigned(kMaxRowGroups),
      unsigned(sizeof(WriterState)), unsigned(sizeof(Sample)), boot_text,
      station_text, light ? "true" : "false");
}

void poll_logger(const PmsSnapshot &pms) {
  if (!accepting)
    return;
  const auto now = esp_timer_get_time();
  {
    static std::int64_t last_stall_warning_us = 0;
    const auto heartbeat = worker_heartbeat_us.load();
    if (heartbeat && now - heartbeat > 5000000LL &&
        now - last_stall_warning_us > 30000000LL) {
      last_stall_warning_us = now;
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
      aqlog.printf(
          "PARQUET ERROR operation=worker-stall seconds=%lld "
          "state=%s stack_free=%u\n",
          static_cast<long long>((now - heartbeat) / 1000000), state_name,
          worker_task ? unsigned(uxTaskGetStackHighWaterMark(worker_task))
                      : 0U);
    }
  }
  if (now >= next_sample_us) {
    const auto skipped = (now - next_sample_us) / kSampleUs;
    missed_deadlines += skipped;
    sample_sequence += skipped;
    next_sample_us += skipped * kSampleUs;
    collect(pms, now, next_sample_us);
    next_sample_us += kSampleUs;
  }
  service_rtc_write(esp_timer_get_time());
  static Command input{};
  static std::size_t length = 0;
  static bool overflow = false;
  input.source = Command::Source::Serial;
  for (unsigned limit = 0; limit < 128 && Serial.available(); ++limit) {
    const int byte = Serial.read();
    if (byte == '\r')
      continue;
    if (byte == '\n') {
      input.text[length] = '\0';
      input.received_mono_us = esp_timer_get_time();
      if (overflow || xQueueSend(commands, &input, 0) != pdTRUE)
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
  config::load(display_detected);
  const config::Settings settings = config::get();
  static const ble::Identity identity{
      station_text,          device_text,       boot_text, kSchemaName,
      unsigned(field_count), kDictionarySha256, kFirmware};
  const bool ble_ok = ble::begin(identity, settings.pair, settings.pin);
  // mDNS host label mirrors the BLE name: AQ-6b40 -> aq-6b40.
  char host[24];
  std::snprintf(host, sizeof(host), "%s", ble::local_name());
  for (char *p = host; *p; ++p)
    if (*p >= 'A' && *p <= 'Z')
      *p = static_cast<char>(*p - 'A' + 'a');
  lan::begin(host);
  return ble_ok;
}

bool enqueue_request(const ble::ControlRequest &request) {
  if (!commands)
    return false;
  if (request.length && request.bytes[0] == ble::kOpWifiScan) {
    // Scans block for seconds; they run on the LAN task, not the worker.
    if (lan::request_scan(request.link, request.link_generation))
      return true;
    // Fall through to the worker, which answers "busy" on the right link.
  }
  static Command command;
  command = Command{};
  command.source = Command::Source::Control;
  command.received_mono_us = request.received_mono_us;
  command.control = request;
  return xQueueSend(commands, &command, 0) == pdTRUE;
}

const char *station_text_id() { return station_text; }
const char *device_text_id() { return device_text; }
const char *firmware_text_id() { return kFirmware; }

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
} // namespace telemetry
