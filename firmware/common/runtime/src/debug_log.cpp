#include "debug_log.h"

#include "aq_console.h"
#include <freertos/FreeRTOS.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
portMUX_TYPE ring_mutex = portMUX_INITIALIZER_UNLOCKED;

constexpr std::uint32_t kMagic = 0x474c5141; // "AQLG"
constexpr std::uint16_t kVersion = 1;

// Small nibble-table CRC-32 (IEEE); the header is sealed on every write.
std::uint32_t crc32(const std::uint8_t *data, std::size_t size) {
  static constexpr std::uint32_t kTable[16] = {
      0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
      0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
      0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    crc = (crc >> 4) ^ kTable[crc & 0x0f];
    crc = (crc >> 4) ^ kTable[crc & 0x0f];
  }
  return ~crc;
}
} // namespace

struct DebugLog::Header {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t boot_seq;
  std::uint32_t ring_bytes;
  std::uint32_t event_slots;
  std::uint32_t head;
  std::uint32_t retained;
  std::uint32_t event_head;
  std::uint32_t event_count;
  std::uint16_t counters[kCounters];
  std::uint32_t reserved[3];
  std::uint32_t crc; // over every preceding byte
};
static_assert(sizeof(DebugLog::Header) == DebugLog::kHeaderBytes,
              "persistent log header layout");

DebugLog aqlog;
DebugLog::DebugLog()
    : output_([](void *, const std::uint8_t *data, std::size_t size) {
        return aq::console::write(data, size);
      }) {}

std::size_t LogOutput::print(const char *text) {
  return text ? write(reinterpret_cast<const std::uint8_t *>(text),
                      std::strlen(text))
              : 0;
}

std::size_t LogOutput::println(const char *text) {
  return print(text) + write(static_cast<std::uint8_t>('\n'));
}

int LogOutput::printf(const char *format, ...) {
  char local[256];
  va_list args;
  va_start(args, format);
  va_list measured;
  va_copy(measured, args);
  const int length = std::vsnprintf(local, sizeof(local), format, measured);
  va_end(measured);
  if (length <= 0) {
    va_end(args);
    return length;
  }
  char *text = local;
  if (static_cast<std::size_t>(length) >= sizeof(local)) {
    text =
        static_cast<char *>(std::malloc(static_cast<std::size_t>(length) + 1));
    if (!text) {
      va_end(args);
      return -1;
    }
    std::vsnprintf(text, static_cast<std::size_t>(length) + 1, format, args);
  }
  va_end(args);
  const auto written =
      write(reinterpret_cast<const std::uint8_t *>(text), length);
  if (text != local)
    std::free(text);
  return static_cast<int>(written);
}

bool DebugLog::attach(void *block, std::size_t bytes, const Layout &layout,
                      bool cold) {
  if (!block || layout.ring_bytes == 0 || layout.event_slots == 0 ||
      bytes < persistent_bytes(layout) ||
      reinterpret_cast<std::uintptr_t>(block) % alignof(Header) != 0)
    return false;
  auto *header = static_cast<Header *>(block);
  char *ring = static_cast<char *>(block) + kHeaderBytes;
  auto *event_ring =
      reinterpret_cast<std::uint8_t (*)[kEventBytes]>(ring + layout.ring_bytes);

  portENTER_CRITICAL(&ring_mutex);
  if (header_) {
    portEXIT_CRITICAL(&ring_mutex);
    return false;
  }
  const bool kept =
      !cold && header->magic == kMagic && header->version == kVersion &&
      header->ring_bytes == layout.ring_bytes &&
      header->event_slots == layout.event_slots &&
      header->head < layout.ring_bytes &&
      header->retained <= layout.ring_bytes &&
      header->event_head < layout.event_slots &&
      header->event_count <= layout.event_slots &&
      header->crc == crc32(reinterpret_cast<const std::uint8_t *>(header),
                           offsetof(Header, crc));
  if (!kept) {
    std::memset(header, 0, sizeof(Header));
    header->magic = kMagic;
    header->version = kVersion;
    header->ring_bytes = static_cast<std::uint32_t>(layout.ring_bytes);
    header->event_slots = static_cast<std::uint32_t>(layout.event_slots);
  }
  header->boot_seq = static_cast<std::uint16_t>(
      kept ? (header->boot_seq == 0xffff ? 1 : header->boot_seq + 1) : 1);

  // Move what the internal ring captured since power-up behind the retained
  // contents, then switch every reader and writer to the block.
  const std::size_t early_bytes = retained_;
  const std::size_t early_start =
      (head_ + kFallbackRingBytes - early_bytes) % kFallbackRingBytes;
  const std::size_t early_events = event_count_;
  const std::size_t early_event_start =
      (event_head_ + kFallbackEventSlots - early_events) % kFallbackEventSlots;
  std::uint16_t early_counters[kCounters];
  std::memcpy(early_counters, counters_, sizeof(early_counters));

  header_ = header;
  ring_ = ring;
  ring_size_ = layout.ring_bytes;
  event_ring_ = event_ring;
  event_slots_ = layout.event_slots;
  counters_ = header->counters;
  head_ = header->head;
  retained_ = header->retained;
  written_ = retained_;
  event_head_ = header->event_head;
  event_count_ = header->event_count;
  events_written_ = event_count_;
  boot_seq_ = header->boot_seq;

  for (std::size_t copied = 0; copied < early_bytes;) {
    const std::size_t index = (early_start + copied) % kFallbackRingBytes;
    const std::size_t run = kFallbackRingBytes - index < early_bytes - copied
                                ? kFallbackRingBytes - index
                                : early_bytes - copied;
    append_locked(
        reinterpret_cast<const std::uint8_t *>(fallback_ring_) + index, run);
    copied += run;
  }
  for (std::size_t i = 0; i < early_events; ++i)
    append_event_locked(
        fallback_events_[(early_event_start + i) % kFallbackEventSlots]);
  for (std::size_t i = 0; i < kCounters; ++i) {
    const unsigned sum = unsigned(counters_[i]) + early_counters[i];
    counters_[i] = static_cast<std::uint16_t>(sum > 0xffff ? 0xffff : sum);
  }
  seal_locked();
  portEXIT_CRITICAL(&ring_mutex);
  return kept;
}

bool DebugLog::persistent() const {
  portENTER_CRITICAL(&ring_mutex);
  const bool attached = header_ != nullptr;
  portEXIT_CRITICAL(&ring_mutex);
  return attached;
}

std::uint16_t DebugLog::boot_seq() const {
  portENTER_CRITICAL(&ring_mutex);
  const auto seq = boot_seq_;
  portEXIT_CRITICAL(&ring_mutex);
  return seq;
}

std::size_t DebugLog::ring_bytes() const {
  portENTER_CRITICAL(&ring_mutex);
  const auto size = ring_size_;
  portEXIT_CRITICAL(&ring_mutex);
  return size;
}

std::size_t DebugLog::retained() const {
  portENTER_CRITICAL(&ring_mutex);
  const auto count = retained_;
  portEXIT_CRITICAL(&ring_mutex);
  return count;
}

size_t DebugLog::write(const uint8_t *buffer, size_t size) {
  if (write_record_only(buffer, size) == 0)
    return 0;
  return output_ ? output_(output_context_, buffer, size) : size;
}

size_t DebugLog::write_record_only(const uint8_t *buffer, size_t size) {
  if (!buffer || size == 0)
    return 0;
  // Ordinary lines fit one slice. A longer write may interleave with another
  // writer at a slice boundary, which only affects human-readable text.
  for (std::size_t done = 0; done < size;) {
    const std::size_t count =
        size - done < kLockedCopyBytes ? size - done : kLockedCopyBytes;
    portENTER_CRITICAL(&ring_mutex);
    append_locked(buffer + done, count);
    total_ += static_cast<std::uint32_t>(count);
    seal_locked();
    portEXIT_CRITICAL(&ring_mutex);
    done += count;
  }
  return size;
}

void DebugLog::append_locked(const std::uint8_t *source, std::size_t count) {
  written_ += count;
  // Keep only the newest ring_size_ bytes of an oversized write.
  if (count > ring_size_) {
    source += count - ring_size_;
    count = ring_size_;
  }
  const std::size_t first =
      ring_size_ - head_ < count ? ring_size_ - head_ : count;
  std::memcpy(ring_ + head_, source, first);
  if (count > first)
    std::memcpy(ring_, source + first, count - first);
  head_ = (head_ + count) % ring_size_;
  retained_ = retained_ + count > ring_size_ ? ring_size_ : retained_ + count;
}

void DebugLog::append_event_locked(const std::uint8_t *event) {
  std::memcpy(event_ring_[event_head_], event, kEventBytes);
  event_head_ = (event_head_ + 1) % event_slots_;
  if (event_count_ < event_slots_)
    ++event_count_;
  ++events_written_;
}

void DebugLog::seal_locked() {
  if (!header_)
    return;
  header_->head = static_cast<std::uint32_t>(head_);
  header_->retained = static_cast<std::uint32_t>(retained_);
  header_->event_head = static_cast<std::uint32_t>(event_head_);
  header_->event_count = static_cast<std::uint32_t>(event_count_);
  header_->crc = crc32(reinterpret_cast<const std::uint8_t *>(header_),
                       offsetof(Header, crc));
}

std::size_t DebugLog::copy_locked(std::uint64_t position, char *out,
                                  std::size_t count) const {
  // `position` lies within the retained window, which ends at head_.
  const std::size_t back = static_cast<std::size_t>(written_ - position);
  const std::size_t start = (head_ + ring_size_ - back) % ring_size_;
  const std::size_t first =
      ring_size_ - start < count ? ring_size_ - start : count;
  std::memcpy(out, ring_ + start, first);
  if (count > first)
    std::memcpy(out + first, ring_, count - first);
  return count;
}

std::size_t DebugLog::tail(char *out, std::size_t max,
                           std::uint32_t &total) const {
  auto cursor = tail_cursor(max, total);
  std::size_t copied = 0;
  while (cursor.remaining) {
    const std::size_t want =
        cursor.remaining < max - copied ? cursor.remaining : max - copied;
    const std::size_t count = read(cursor, out + copied, max - copied);
    copied += count;
    // A short read means a writer overtook the copy. Stop rather than skip
    // ahead, so the result stays one contiguous span.
    if (count < want)
      break;
  }
  return copied;
}

DebugLog::Cursor DebugLog::tail_cursor(std::size_t max,
                                       std::uint32_t &total) const {
  portENTER_CRITICAL(&ring_mutex);
  total = total_;
  const std::size_t count = retained_ < max ? retained_ : max;
  const Cursor cursor{written_ - count, count};
  portEXIT_CRITICAL(&ring_mutex);
  return cursor;
}

std::size_t DebugLog::read(Cursor &cursor, char *out, std::size_t max) const {
  std::size_t copied = 0;
  while (copied < max && cursor.remaining) {
    portENTER_CRITICAL(&ring_mutex);
    const std::uint64_t oldest = written_ - retained_;
    if (cursor.position < oldest && copied) {
      // Overtaken mid-read. Return the contiguous part, the next call clips.
      portEXIT_CRITICAL(&ring_mutex);
      break;
    }
    if (cursor.position < oldest) {
      const std::uint64_t lost = oldest - cursor.position;
      cursor.remaining =
          lost >= cursor.remaining ? 0 : cursor.remaining - std::size_t(lost);
      cursor.position = oldest;
    }
    std::size_t count =
        cursor.remaining < max - copied ? cursor.remaining : max - copied;
    if (count > kLockedCopyBytes)
      count = kLockedCopyBytes;
    const std::uint64_t available = written_ - cursor.position;
    if (count > available)
      count = static_cast<std::size_t>(available);
    copy_locked(cursor.position, out + copied, count);
    cursor.position += count;
    cursor.remaining -= count;
    portEXIT_CRITICAL(&ring_mutex);
    if (count == 0)
      break;
    copied += count;
  }
  return copied;
}

void DebugLog::record_event(const std::uint8_t (&event)[kEventBytes],
                            std::size_t counter_index) {
  portENTER_CRITICAL(&ring_mutex);
  append_event_locked(event);
  if (counter_index < kCounters && counters_[counter_index] != 0xffff)
    ++counters_[counter_index];
  seal_locked();
  portEXIT_CRITICAL(&ring_mutex);
}

void DebugLog::copy_events_locked(std::uint64_t sequence,
                                  std::uint8_t (*out)[kEventBytes],
                                  std::size_t count) const {
  // `sequence` lies within the retained events, which end at event_head_.
  const std::size_t back = static_cast<std::size_t>(events_written_ - sequence);
  const std::size_t start = (event_head_ + event_slots_ - back) % event_slots_;
  for (std::size_t i = 0; i < count; ++i)
    std::memcpy(out[i], event_ring_[(start + i) % event_slots_], kEventBytes);
}

std::size_t DebugLog::events(std::uint8_t (*out)[kEventBytes],
                             std::size_t max) const {
  constexpr std::size_t kSlice = kLockedCopyBytes / kEventBytes;
  portENTER_CRITICAL(&ring_mutex);
  const std::size_t wanted = event_count_ < max ? event_count_ : max;
  std::uint64_t next = events_written_ - wanted;
  const std::uint64_t end = events_written_;
  portEXIT_CRITICAL(&ring_mutex);
  std::size_t copied = 0;
  while (next < end) {
    portENTER_CRITICAL(&ring_mutex);
    const std::uint64_t oldest = events_written_ - event_count_;
    if (next < oldest)
      next = oldest < end ? oldest : end;
    std::size_t count = static_cast<std::size_t>(end - next);
    if (count > kSlice)
      count = kSlice;
    copy_events_locked(next, out + copied, count);
    portEXIT_CRITICAL(&ring_mutex);
    next += count;
    copied += count;
  }
  return copied;
}

std::uint16_t DebugLog::counter(std::size_t index) const {
  if (index >= kCounters)
    return 0;
  portENTER_CRITICAL(&ring_mutex);
  const auto value = counters_[index];
  portEXIT_CRITICAL(&ring_mutex);
  return value;
}
