#pragma once

// Request buffers for the HTTPS transport (#81). Pure, so the host test can
// prove the wipe. The STS form body carries the API key, and the headers
// carry the session token and the signature, so every buffer is zeroed when
// a lease ends on any exit path.

#include "os_upload_session.h"
#include "os_upload_text.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace opensensor::upload {

struct TransportScratch {
  std::array<char, 8 + 255 + 1 + 1800> url{};
  std::array<char, 64> name{};
  std::array<char, 2100> value{};
  std::array<std::uint8_t, kChunkBytes> chunk{};
  std::atomic<bool> busy{false};

  void wipe() {
    text::secure_zero(url);
    text::secure_zero(name);
    text::secure_zero(value);
    text::secure_zero(chunk);
  }
};

// Holds the scratch for one request. Only one lease at a time, a second one
// reports owned() == false and must not touch the buffers. The destructor
// wipes every buffer, so an early return cannot leave a secret behind.
class ScratchLease {
public:
  explicit ScratchLease(TransportScratch &scratch)
      : scratch_(scratch),
        owned_(!scratch.busy.exchange(true, std::memory_order_acquire)) {}
  ~ScratchLease() {
    if (!owned_)
      return;
    scratch_.wipe();
    scratch_.busy.store(false, std::memory_order_release);
  }
  ScratchLease(const ScratchLease &) = delete;
  ScratchLease &operator=(const ScratchLease &) = delete;

  [[nodiscard]] bool owned() const { return owned_; }

private:
  TransportScratch &scratch_;
  bool owned_;
};

} // namespace opensensor::upload
