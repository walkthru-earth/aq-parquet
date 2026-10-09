#include "aq_logger.h"
#include "aq_logger_provision.h"
#include "aq_logger_status.h"
#include "aq_logger_work_queue.h"
#include "debug_log.h"
#include "device_config.h"
#include "lz4_codec.h"
#include "parquet_writer.h"
#include "wifi_link.h"
#include <archive_sync.h>
#include <control_sync.h>
#include <sync_codec.h>
#include <sync_service.h>
#include <time_sync.h>
#include <utc_clock.h>

#include <Arduino.h>
#include <Preferences.h>
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

namespace aqlogger {
namespace {
using namespace telemetry;
using Sample = Row;
Config config;
Hooks hooks;
std::size_t field_count = 0;
std::size_t sequence = 0, event_time_utc_ns = 0, clock_epoch = 0;
constexpr std::int32_t kClockNone = aq::utc::None;
constexpr std::int32_t kClockHost = aq::utc::Host;
constexpr std::int32_t kClockRtc = aq::utc::Rtc;
using aq::sync::crc_update;
using aq::sync::get_i64;
using aq::sync::get_u16;
using aq::sync::get_u32;
using aq::sync::put_i64;
using aq::sync::put_u16;
using aq::sync::put_u32;
using aq::utc::days_from_civil;
constexpr std::size_t kMaxRows = 90;
constexpr std::int64_t kSampleUs = 10000000;
// Keep unanchored data readable within a minute at normal sampling cadence.
// The bounded RAM batch can still be lost if power disappears before flush.
constexpr std::size_t kUnsyncedRows = 6;
const char *kDirectory = "/sd/output";
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
  Column columns[telemetry::kMaxColumns];
  OutputFile telemetry;
  OutputFile benchmark; // codec-test copies never touch the telemetry file
  Lz4Workspace lz4{};
};
// stdio staging stays in internal RAM, distinct from the PSRAM row buffer.
char staging_telemetry[4096];
char staging_benchmark[4096];
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
// Worker liveness: the loop stamps this every pass; poll warns when it
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
// Worker-owned: finalize the first genuine row for a fresh external anchor.
std::int32_t pending_anchor_epoch = 0;
// A host sync asks the main task to copy the anchor through the optional
// board RTC callback; the storage worker never performs board RTC IO.
std::atomic<bool> rtc_write_pending{false};
std::atomic<std::int64_t> rtc_write_requested_us{0};
std::atomic<std::int32_t> rtc_state{0}; // 0 unread, 1 seeded, 2 unusable
bool mounted = false;
bool accepting = false;
std::atomic<Codec> selected_codec{Codec::Uncompressed};
const char *codec_name(Codec codec) {
  return codec == Codec::Lz4Raw ? "LZ4_RAW" : "UNCOMPRESSED";
}

class BusLock {
public:
  BusLock() { lock_bus(); }
  ~BusLock() { unlock_bus(); }
};

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
        ok = ok && aq::sync::parquet_complete(size, magic, tail) &&
             std::fseek(file, 0, SEEK_SET) == 0;
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
  for (std::size_t i = 0; i < field_count; ++i) {
    const auto &field = config.fields[i];
    const auto logical =
        field.type == PhysicalType::Int64 &&
                std::strcmp(field.unit, "ns_since_unix_epoch") == 0
            ? LogicalType::TimestampNanosUtc
            : LogicalType::None;
    writer_state->columns[i] = Column{field.name,
                                      field.type,
                                      &writer_state->rows[0].data[i],
                                      sizeof(Sample),
                                      &writer_state->rows[0].valid[i],
                                      sizeof(Sample),
                                      logical};
  }
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
  // The board already mounted its filesystem; create directories only.
  for (char *part = copy + 1;; ++part) {
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
  KeyValue metadata[64] = {
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
  std::size_t metadata_count = 22;
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

bool safe_name(const char *name) { return aq::sync::safe_parquet_name(name); }

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
    char path[64];
    std::snprintf(path, sizeof(path), "%s/quarantine", kDirectory);
    directory = ::opendir(path);
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
    std::snprintf(path, sizeof(path), "%s/quarantine/%s", kDirectory,
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
  char directory[64];
  std::snprintf(directory, sizeof(directory), "%s/quarantine", kDirectory);
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
    has_legacy = config.legacy_directory &&
                 ::stat(config.legacy_directory, &legacy) == 0 &&
                 S_ISDIR(legacy.st_mode);
  }
  if (has_legacy)
    list_directory("", 0, partials, emit, context, config.legacy_directory,
                   "legacy-parquet/");
  partials_seen = partials;
  return partials;
}

void list_files(bool emit_serial = true) {
  std::uint32_t count = 0;
  const FileEmitter count_file = [](void *context, const char *,
                                    std::uint32_t) {
    auto &files = *static_cast<std::uint32_t *>(context);
    if (files != UINT32_MAX)
      ++files;
  };
  const unsigned partials =
      list_all(emit_serial ? emit_file_serial : count_file,
               emit_serial ? nullptr : &count);
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
  return aq::utc::source_name(source);
}

// Shared by `parquet time`, the BLE/LAN SET_TIME op and the boot-time RTC
// seed. False for a bad epoch (before 2020 or after 2100, which also rejects
// an RTC that was never written). Each call starts a new clock epoch; rows
// already captured keep theirs. When an anchor already existed, `skew_ns`
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
  if (previous)
    *previous = clock_generation ? clock_source : kClockNone;
  if (skew_ns)
    *skew_ns = clock_generation
                   ? utc_ns - (aq::utc::estimate_ns(mono, anchor_mono_us,
                                                    anchor_utc_ns))
                   : 0;
  anchor_mono_us = mono;
  anchor_utc_ns = utc_ns;
  clock_source = source;
  ++clock_generation;
  portEXIT_CRITICAL(&clock_mutex);
  if (source == kClockHost || source == aq::utc::Network) {
    pending_anchor_epoch = clock_generation;
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
  portENTER_CRITICAL(&clock_mutex);
  generation = clock_generation;
  source = clock_source;
  portEXIT_CRITICAL(&clock_mutex);
  StatusSnapshot view;
  view.uptime_seconds =
      static_cast<std::uint32_t>(esp_timer_get_time() / 1000000);
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
  view.clock_source = generation ? source : kClockNone;
  view.rtc_state = rtc_state.load();
  view.storage_ok = storage_ok.load();
  view.total_kib = total_kib.load();
  view.used_kib = used_kib.load();
  view.heap_free = ESP.getFreeHeap();
  view.partials = partials_seen.load();
  view.quarantined = partials_quarantined.load();
  view.quarantine_bytes = quarantine_bytes.load();
  view.open_rows = open_rows.load();
  view.open_groups = open_groups.load();
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
  put("{\"seq\":%lld,\"mono\":%lld", static_cast<long long>(row.data[sequence]),
      static_cast<long long>(row.data[field_index("monotonic_us")]));
  struct LiveField {
    const char *field;
    const char *key;
  };
  constexpr LiveField fields[] = {{"event_time_utc_ns", "utc"},
                                  {"pms_status", "pms"},
                                  {"pm1_atmospheric_ug_m3", "pm1"},
                                  {"pm25_atmospheric_ug_m3", "pm25"},
                                  {"pm10_atmospheric_ug_m3", "pm10"},
                                  {"pm1_cf1_ug_m3", "c1"},
                                  {"pm25_cf1_ug_m3", "c25"},
                                  {"pm10_cf1_ug_m3", "c10"},
                                  {"particles_gt03_per_01l", "n03"},
                                  {"particles_gt05_per_01l", "n05"},
                                  {"particles_gt10_per_01l", "n1"},
                                  {"particles_gt25_per_01l", "n25"},
                                  {"particles_gt50_per_01l", "n5"},
                                  {"particles_gt100_per_01l", "n10"},
                                  {"ambient_temperature_c", "t"},
                                  {"relative_humidity_percent", "rh"},
                                  {"imu_temperature_c", "t"},
                                  {"battery_mv", "bat"},
                                  {"battery_percent", "pct"},
                                  {"charging_status", "chg"},
                                  {"vbus_mv", "vbus"},
                                  {"light_ch0_raw", "als"}};
  for (const auto &field : fields) {
    if (std::strcmp(field.field, "imu_temperature_c") == 0 &&
        field_index("ambient_temperature_c") < field_count &&
        row.valid[field_index("ambient_temperature_c")])
      continue;
    const auto index = field_index(field.field);
    if (index >= field_count || !row.valid[index])
      continue;
    switch (config.fields[index].type) {
    case PhysicalType::Int64:
      put(",\"%s\":%lld", field.key, static_cast<long long>(row.data[index]));
      break;
    case PhysicalType::Int32: {
      std::int32_t value;
      std::memcpy(&value, &row.data[index], sizeof(value));
      put(",\"%s\":%ld", field.key, static_cast<long>(value));
      break;
    }
    case PhysicalType::Float: {
      float value;
      std::memcpy(&value, &row.data[index], sizeof(value));
      put(",\"%s\":%.1f", field.key, static_cast<double>(value));
      break;
    }
    }
  }
  put("}");
  if (used < sizeof(json) && used <= ble::kMaxJson) {
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
bool respond(const std::uint8_t *frame, std::size_t length) {
  // A long LIST or READ is progress, not a stall: stamp the heartbeat per
  // frame so the stall detector only fires when sending truly stops.
  worker_heartbeat_us = esp_timer_get_time();
  return current_request && current_request->link == ble::Link::Lan
             ? lan::send_response(frame, length, current_request->peer,
                                  current_request->link_generation)
             : ble::send_response(
                   frame, length,
                   current_request ? current_request->link_generation : 0);
}
bool respond_error(ble::Op op, ble::Error code, const char *detail) {
  return current_request && current_request->link == ble::Link::Lan
             ? lan::send_error(op, code, detail, current_request->peer,
                               current_request->link_generation)
             : ble::send_error(
                   op, code, detail,
                   current_request ? current_request->link_generation : 0);
}
// Archive policy stays with the board worker; file-transfer sessions and wire
// frames are shared across Arduino boards and BLE/LAN transports.
aqsync::ArchiveSession archive_session(
    {nullptr,
     [](void *, aqsync::FileEmitter emit, void *context) {
       return list_all(emit, context);
     },
     [](void *, const char *name, char *path, std::size_t size) {
       return resolve_path(name, path, size);
     },
     [](void *, const char *path, std::uint32_t &size, std::uint32_t &crc) {
       return finalized_file(path, size, crc);
     },
     [](void *) { return total_kib.load(); },
     [](void *) { return used_kib.load(); },
     [](void *) {
       unlisted_files = false;
       publish_advert();
     },
     [](void *) { lock_bus(); }, [](void *) { unlock_bus(); },
     [](void *, const char *line) { aqlog.record_only().print(line); }},
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
                  ? lan::send_response(frame, length, peer, generation)
                  : ble::send_response(frame, length, generation);
     },
     [](void *, ble::Link link, std::uint8_t peer, std::uint32_t generation,
        ble::Op op, ble::Error code, const char *detail) {
       return link == ble::Link::Lan
                  ? lan::send_error(op, code, detail, peer, generation)
                  : ble::send_error(op, code, detail, generation);
     },
     [](void *) { worker_heartbeat_us = esp_timer_get_time(); }});

const aqsync::ControlReplies control_replies{
    nullptr, [](void *, ble::Link) { return link_payload_max(); },
    [](void *, ble::Link, const std::uint8_t *frame, std::size_t length) {
      return respond(frame, length);
    },
    [](void *, ble::Link, ble::Op op, ble::Error code, const char *detail) {
      return respond_error(op, code, detail);
    }};

struct WorkerState {
  std::size_t &count;
  bool &failed;
  bool storage_ready;
};

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
    // Publish all captured rows immediately so the phone can reconcile with
    // the acknowledgement's boot-scoped anchor before its LIST/download.
    if (state.storage_ready && !state.failed && !write_batch(state.count)) {
      state.failed = true;
      worker_failed = true;
    }
    std::uint8_t frame[21];
    frame[0] = ble::kFrameTimeSet;
    const auto length =
        aq::sync::encode_time_ack(frame + 1, time, request.received_mono_us);
    respond(frame, length + 1);
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
      total_kib =
          (hooks.storage_total_bytes ? hooks.storage_total_bytes(hooks.context)
                                     : 0) /
          1024;
      used_kib =
          (hooks.storage_used_bytes ? hooks.storage_used_bytes(hooks.context)
                                    : 0) /
          1024;
    }
    const unsigned quarantined = quarantine_partials();
    aqlog.printf("PARQUET RECOVERY quarantined=%u repaired=0 deleted=0\n",
                 quarantined);
    list_files(false);
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
    // Sample priority is retained without sleeping before every control
    // request. Successful producers signal after copying to a queue.
    if (xQueueReceive(samples, &row, 0) == pdTRUE) {
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
        if (pending_anchor_epoch && row.valid[event_time_utc_ns] &&
            row.data[clock_epoch] >= pending_anchor_epoch) {
          if (!write_batch(count))
            failed = true;
          else
            pending_anchor_epoch = 0;
        } else if (!row.valid[event_time_utc_ns] && count >= kUnsyncedRows) {
          if (!write_batch(count))
            failed = true;
        } else if (count >= rows_per_group() && !commit(count, false))
          failed = true;
      }
    }
    worker_failed = failed;
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
        if (storage_ready && !failed && !write_batch(count))
          failed = true;
        aqlog.printf(
            "PARQUET TIME epoch_s=%lld source=network monotonic_us=%lld\n",
            static_cast<long long>(anchor.seconds),
            static_cast<long long>(anchor.monotonic_us));
        publish_status();
      }
      continue;
    }
    if (command.source == Command::Source::Control) {
      WorkerState state{count, failed, storage_ready};
      current_request = &command.control;
      handle_control_request(command.control, state);
      current_request = nullptr;
      continue;
    }
    if (std::strcmp(command.text, "parquet owner-pin") == 0) {
      // Physical serial only. Bypass aqlog so the PIN never enters LOG_TAIL.
      // Control requests use a separate opcode dispatcher above and cannot
      // reach this command. No PIN is printed at boot.
      const auto settings = ::config::get();
      if (settings.pair == ::config::PairMode::Fixed)
        Serial.printf("AQ OWNER_PIN %06lu\n",
                      static_cast<unsigned long>(settings.pin));
      else
        Serial.printf("AQ OWNER_PIN ERROR mode=%s reason=use-pairing-display\n",
                      ::config::pair_name(settings.pair));
    } else if (std::strcmp(command.text, "parquet wifi-profile") == 0) {
      // Credential-bearing output never passes through the log ring.
      char profile[256];
      const auto length =
          format_wifi_profile(::config::get(), profile, sizeof(profile));
      if (length)
        Serial.write(reinterpret_cast<const std::uint8_t *>(profile), length);
      else
        aqlog.println("AQ WIFI_PROFILE ERROR reason=capacity");
    } else if (std::strncmp(command.text, "parquet config-hex ", 19) == 0) {
      const auto result = apply_config_hex(command.text + 19);
      if (result.ok)
        Serial.printf("AQ CONFIG ok=1 reboot_required=%u\n",
                      result.reboot_required ? 1U : 0U);
      else
        aqlog.printf("AQ CONFIG ok=0 key=%s\n", result.bad_key);
      std::memset(command.text, 0, sizeof(command.text));
    } else if (std::strcmp(command.text, "parquet status") == 0) {
      aqlog.printf(
          "PARQUET STATUS interval_s=%lu buffered=%u open_rows=%lu "
          "open_groups=%lu finalized=%lu "
          "dropped=%lu errors=%lu queue_peak=%lu failed=%s station=%s "
          "codec=%s schema=%s config=%s deployment=unknown "
          "calibration=unknown\n",
          static_cast<unsigned long>(rotation_seconds.load()), unsigned(count),
          static_cast<unsigned long>(open_rows.load()),
          static_cast<unsigned long>(open_groups.load()),
          static_cast<unsigned long>(finalized.load()),
          static_cast<unsigned long>(dropped.load()),
          static_cast<unsigned long>(errors.load()),
          static_cast<unsigned long>(queue_peak.load()),
          failed ? "true" : "false", station_text, codec_name(selected_codec),
          config.schema_name, config.configuration_id);
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
      if (storage_ready && !failed && !write_batch(count))
        failed = true;
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
  if (hooks.collect)
    hooks.collect(row, now, scheduled, hooks.context);
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
    counter(row, "event_time_utc_ns",
            aq::utc::estimate_ns(now, mono_anchor, utc_anchor));
    counter(row, "clock_anchor_mono_us", mono_anchor);
    counter(row, "clock_anchor_utc_ns", utc_anchor);
  }
  counter(row, "heap_free_bytes", ESP.getFreeHeap());
  counter(row, "heap_min_free_bytes", ESP.getMinFreeHeap());
  counter(row, "psram_free_bytes", ESP.getFreePsram());
  if (mounted) {
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

bool begin(const Config &configuration, const Hooks &board_hooks,
           bool sd_mounted) {
  if (writer_state || accepting || !configuration.fields ||
      configuration.column_count == 0 ||
      configuration.column_count > telemetry::kMaxColumns ||
      configuration.metadata_count > 32 ||
      (configuration.metadata_count && !configuration.metadata) ||
      bool(board_hooks.lock_bus) != bool(board_hooks.unlock_bus) ||
      (sd_mounted &&
       (!board_hooks.storage_total_bytes || !board_hooks.storage_used_bytes)))
    return false;
  config = configuration;
  hooks = board_hooks;
  field_count = config.column_count;
  if (!config.schema_name || !config.firmware || !config.dictionary_version ||
      !config.dictionary_uri || !config.dictionary_sha256 ||
      !config.configuration_id || !config.configuration ||
      !config.time_semantics || !config.board || !config.output_directory ||
      config.output_directory[0] != '/' ||
      std::strlen(config.output_directory) > 31)
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
  sequence = field_index("sequence");
  event_time_utc_ns = field_index("event_time_utc_ns");
  clock_epoch = field_index("clock_epoch");
  const auto monotonic = field_index("monotonic_us");
  if (sequence == field_count || event_time_utc_ns == field_count ||
      clock_epoch == field_count || monotonic == field_count ||
      config.fields[sequence].type != telemetry::PhysicalType::Int64 ||
      config.fields[event_time_utc_ns].type != telemetry::PhysicalType::Int64 ||
      config.fields[monotonic].type != telemetry::PhysicalType::Int64 ||
      config.fields[clock_epoch].type != telemetry::PhysicalType::Int32)
    return false;
  kDirectory = config.output_directory;
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
  samples = xQueueCreate(8, sizeof(Sample));
  commands = xQueueCreate(6, sizeof(Command));
  // Created before the worker: wakeups do not depend on task-handle
  // publication.
  worker_wakeup = xSemaphoreCreateBinary();
  writer_state = static_cast<WriterState *>(heap_caps_calloc(
      1, sizeof(WriterState), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!spi_mutex || !samples || !commands || !worker_wakeup || !writer_state) {
    aqlog.println("PARQUET ERROR operation=allocate logging=false");
    return false;
  }
  new (writer_state) WriterState{};
  writer_state->telemetry.staging = staging_telemetry;
  writer_state->benchmark.staging = staging_benchmark;
  prepare_columns();
  // Before the worker starts, so the first row of the boot is already dated
  // when the RTC holds an earlier host sync.
  seed_clock_from_rtc();
  next_sample_us = esp_timer_get_time() + kSampleUs;
  if (xTaskCreate(storage_worker, "parquet-sd", 24576, nullptr, 1,
                  &worker_task) != pdPASS) {
    aqlog.println("PARQUET ERROR operation=task logging=false");
    return false;
  }
  accepting = true;
  aqlog.printf(
      "PARQUET BEGIN schema=%s columns=%u sample_s=10 "
      "interval_s=900 max_rows=90 max_row_groups=%u psram_workspace_bytes=%u "
      "row_bytes=%u boot=%s station=%s "
      "codec=UNCOMPRESSED\n",
      config.schema_name, unsigned(field_count), unsigned(kMaxRowGroups),
      unsigned(sizeof(WriterState)), unsigned(sizeof(Sample)), boot_text,
      station_text);
  return true;
}

void poll() {
  if (!accepting)
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
    collect(now, next_sample_us);
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
  return aqsync::begin(identity, enqueue_request, display_detected).ble_started;
}

bool enqueue_request(const ble::ControlRequest &request) {
  if (!commands || !worker_wakeup || request.length == 0 ||
      request.length > sizeof(request.bytes))
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
