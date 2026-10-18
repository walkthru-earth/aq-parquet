// Persisted codec tests. The real codec_store load(), store() and resolve()
// run over a host NVS fake that keeps namespaces, types and staged commits.
#include "aq_logger_codec_store.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace store = aqlogger::codec_store;
using fake_nvs::Type;
using telemetry::Codec;

namespace {
const fake_nvs::Entry *committed(const char *ns, const char *key) {
  const auto found = fake_nvs::state.committed.find({ns, key});
  return found == fake_nvs::state.committed.end() ? nullptr : &found->second;
}

void wire_values() {
  // The stored byte is the Parquet CompressionCodec value.
  static_assert(store::encode(Codec::Uncompressed) == 0);
  static_assert(store::encode(Codec::Lz4Raw) == 7);
  static_assert(*store::decode(0) == Codec::Uncompressed);
  static_assert(*store::decode(7) == Codec::Lz4Raw);
  static_assert(store::kDefault == Codec::Lz4Raw);
  static_assert(std::string_view{store::kNamespace} == "parquet");
  static_assert(std::string_view{store::kKey} == "codec");
  for (unsigned raw = 0; raw <= 0xff; ++raw) {
    const auto decoded = store::decode(static_cast<std::uint8_t>(raw));
    if (raw == 0 || raw == 7) {
      assert(decoded.has_value());
      assert(store::encode(*decoded) == raw);
    } else {
      // SNAPPY (1), ZSTD (6) and every other byte are not written here.
      assert(!decoded.has_value() && decoded.error() == store::Error::Corrupt);
    }
  }
  for (const auto error :
       {store::Error::Open, store::Error::Missing, store::Error::Read,
        store::Error::Corrupt, store::Error::Write, store::Error::Commit})
    assert(std::strcmp(store::error_name(error), "unknown") != 0);
}

void missing_key_boots_default() {
  fake_nvs::reset();
  // The station key lives in the same namespace and must not be mistaken.
  fake_nvs::put("parquet", "station", Type::Str, 0);
  const auto loaded = store::load();
  assert(!loaded.has_value() && loaded.error() == store::Error::Missing);
  const auto boot = store::resolve(loaded);
  assert(boot.codec == Codec::Lz4Raw);
  assert(boot.fallback && *boot.fallback == store::Error::Missing);
  // Loading never writes the default back.
  assert(fake_nvs::state.sets == 0 && fake_nvs::state.commits == 0);
  assert(!committed("parquet", "codec"));
  assert(fake_nvs::state.opens == fake_nvs::state.closes);
}

void store_then_load_round_trips() {
  for (const auto codec : {Codec::Uncompressed, Codec::Lz4Raw}) {
    fake_nvs::reset();
    fake_nvs::put("parquet", "station", Type::Str, 42);
    const auto saved = store::store(codec);
    assert(saved.has_value());
    const auto *entry = committed("parquet", "codec");
    assert(entry && entry->type == Type::U8);
    assert(entry->number == store::encode(codec));
    // The neighbouring key and other namespaces are untouched.
    const auto *station = committed("parquet", "station");
    assert(station && station->type == Type::Str && station->number == 42);
    assert(fake_nvs::state.committed.size() == 2);
    assert(fake_nvs::state.commits == 1);
    // A reboot reads it back as the boot codec.
    const auto boot = store::resolve(store::load());
    assert(boot.codec == codec && !boot.fallback);
    assert(fake_nvs::state.opens == fake_nvs::state.closes);
  }
  // A later choice replaces the earlier one.
  fake_nvs::reset();
  assert(store::store(Codec::Lz4Raw).has_value());
  assert(store::store(Codec::Uncompressed).has_value());
  const auto loaded = store::load();
  assert(loaded.has_value() && *loaded == Codec::Uncompressed);
}

void corrupt_value_falls_back_and_stays() {
  fake_nvs::reset();
  fake_nvs::put("parquet", "codec", Type::U8, 6); // ZSTD, not supported here
  auto loaded = store::load();
  assert(!loaded.has_value() && loaded.error() == store::Error::Corrupt);
  auto boot = store::resolve(loaded);
  assert(boot.codec == store::kDefault);
  assert(boot.fallback && *boot.fallback == store::Error::Corrupt);
  // The bad byte stays as evidence, nothing is erased or rewritten.
  const auto *entry = committed("parquet", "codec");
  assert(entry && entry->number == 6 && fake_nvs::state.sets == 0);

  // The same key under another NVS type is corrupt too.
  fake_nvs::reset();
  fake_nvs::put("parquet", "codec", Type::U32, 7);
  loaded = store::load();
  assert(!loaded.has_value() && loaded.error() == store::Error::Corrupt);
  boot = store::resolve(loaded);
  assert(boot.codec == store::kDefault && boot.fallback);

  // Selecting a codec again repairs it through the normal store path.
  fake_nvs::reset();
  fake_nvs::put("parquet", "codec", Type::U8, 0xff);
  assert(store::store(Codec::Uncompressed).has_value());
  loaded = store::load();
  assert(loaded.has_value() && *loaded == Codec::Uncompressed);
}

void read_failures_fall_back() {
  fake_nvs::reset();
  fake_nvs::state.fail_open = true;
  auto loaded = store::load();
  assert(!loaded.has_value() && loaded.error() == store::Error::Open);
  auto boot = store::resolve(loaded);
  assert(boot.codec == store::kDefault && boot.fallback &&
         *boot.fallback == store::Error::Open);

  fake_nvs::reset();
  fake_nvs::put("parquet", "codec", Type::U8, 0);
  fake_nvs::state.fail_get = ESP_FAIL;
  loaded = store::load();
  assert(!loaded.has_value() && loaded.error() == store::Error::Read);
  boot = store::resolve(loaded);
  assert(boot.codec == store::kDefault && boot.fallback &&
         *boot.fallback == store::Error::Read);
  assert(fake_nvs::state.opens == fake_nvs::state.closes);
}

void write_failures_keep_previous_value() {
  fake_nvs::reset();
  fake_nvs::put("parquet", "codec", Type::U8, 0);
  fake_nvs::state.fail_open = true;
  auto saved = store::store(Codec::Lz4Raw);
  assert(!saved.has_value() && saved.error() == store::Error::Open);

  fake_nvs::state.fail_open = false;
  fake_nvs::state.fail_set = true;
  saved = store::store(Codec::Lz4Raw);
  assert(!saved.has_value() && saved.error() == store::Error::Write);
  assert(fake_nvs::state.commits == 0);

  fake_nvs::state.fail_set = false;
  fake_nvs::state.fail_commit = true;
  saved = store::store(Codec::Lz4Raw);
  assert(!saved.has_value() && saved.error() == store::Error::Commit);

  // None of the failed stores changed what the next boot reads.
  fake_nvs::state.fail_commit = false;
  const auto loaded = store::load();
  assert(loaded.has_value() && *loaded == Codec::Uncompressed);
}
} // namespace

int main() {
  wire_values();
  missing_key_boots_default();
  store_then_load_round_trips();
  corrupt_value_falls_back_and_stays();
  read_failures_fall_back();
  write_failures_keep_previous_value();
  std::puts("codec store tests passed");
}
