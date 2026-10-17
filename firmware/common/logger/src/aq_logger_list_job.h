#pragma once

#include <cstdint>
#include <cstring>

// Storage worker glue around a running radio LIST job (archive_sync.h).
// Pure decisions with no IO, queue or clock, so the host tests drive the same
// code as the firmware. The worker asks these once per pass.
namespace aqlogger::listjob {

// What service_storage runs on this pass.
enum class StoragePass : std::uint8_t {
  Idle,      // nothing is due
  Probe,     // the governor probe, with a remount when the board has one
  KeepMount, // the governor probe without the remount
  Recheck,   // the plain free space measurement
};

struct StorageInputs {
  bool probe_due = false;    // Retry and its backoff elapsed
  bool can_probe = false;    // storage ready, or the board can remount
  bool recheck_due = false;  // Full or Degraded and the recheck period passed
  bool full = false;         // the governor is Full
  bool retry = false;        // the governor is in Retry
  bool remount = false;      // the board has a remount hook
  bool files_closed = false; // no writer file is open
  bool listing = false;      // a radio LIST job is running
  bool owner = false;        // an owner action or the emergency close asked
};

// A remount ends a running LIST job with ERROR 8, because every directory
// handle of the walk dies with the mount. A Full card is remounted every
// recheck so space freed on a PC shows up, and that would end every walk
// longer than the recheck period. So periodic remounts wait for the job. A
// Full card gets the plain free space recheck meanwhile, and a Retry card a
// probe that keeps the mount and still proves a write. The skipped remount
// runs on the first pass after the job ends. An owner action, the emergency
// close among them, still remounts at once and may end the job.
class StoragePlanner {
public:
  StoragePass next(const StorageInputs &in) {
    const bool full_remount = in.full && in.remount && in.files_closed;
    if (in.probe_due && in.can_probe) {
      if (in.listing && in.remount && !in.owner) {
        deferred_ = true;
        return StoragePass::KeepMount;
      }
      deferred_ = false;
      return StoragePass::Probe;
    }
    if (deferred_ && !in.listing) {
      deferred_ = false;
      if (in.can_probe && (in.retry || full_remount))
        return StoragePass::Probe;
    }
    if (!in.recheck_due)
      return StoragePass::Idle;
    if (full_remount) {
      if (in.listing) {
        deferred_ = true;
        return StoragePass::Recheck;
      }
      deferred_ = false;
      return StoragePass::Probe;
    }
    return StoragePass::Recheck;
  }

  // A remount is waiting for the job to end.
  bool deferred() const { return deferred_; }

private:
  bool deferred_ = false;
};

// What the worker does with its command queue on this pass.
enum class Take : std::uint8_t {
  Wait,   // serve nothing. One tick delay while a job runs, else idle wait
  Head,   // receive the queue head and serve it
  Park,   // receive the queue head into the parked slot and serve nothing
  Parked, // serve the parked request
};

// While a job runs, a request from the job's own link and peer, its ABORT
// marker included, must wait so its replies keep their order
// (ArchiveSession::must_wait_for_list). The first such request moves to one
// parked slot outside the queue, so the requests behind it from other links
// and peers still run between steps. A LIST from another link or peer does
// not wait, it reaches the session and gets ERROR busy "list" at once. A
// second waiting request from the job's own peer stays at the queue head
// until the job ends. That hold is bounded by the client, because the phone
// sends one request per link at a time plus at most one ABORT, and ABORT
// ends the job within one step. Once the job ends the parked request runs
// first, ahead of anything its peer queued later.
inline Take take(bool listing, bool parked, bool has_head,
                 bool head_must_wait) {
  if (!listing)
    return parked ? Take::Parked : has_head ? Take::Head : Take::Wait;
  if (!has_head)
    return Take::Wait;
  if (!head_must_wait)
    return Take::Head;
  return parked ? Take::Wait : Take::Park;
}

// Serial commands that walk the whole archive in one call. Refused while a
// radio LIST job runs, since a second walk would hold a second set of
// directory handles that the FATFS lock budget does not cover
// (archive_files.h). `parquet get` opens one file by name and is not one.
inline bool serial_walks_archive(const char *text) {
  return text && std::strcmp(text, "parquet list") == 0;
}

} // namespace aqlogger::listjob
