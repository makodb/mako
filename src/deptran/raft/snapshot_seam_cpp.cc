// The snapshot store's SEAM kernels for the C++ lanes (hybrid, cpp): the
// bodies server.cc held until plan phase N4, moved here verbatim, so hybrid
// and cpp behave exactly as before. The Rust lane defines the same kernels in
// raft-rt (rt/src/snapshot.rs) over a Rust store, and does not compile this
// file.
//
// The Prepare -> save -> Commit order stays HOST (server.cc,
// raft_install_snapshot_payload and raft_snapshot_serialize_and_save); only
// their "save" line calls raft_snapshot_store_save below.

#include <stdint.h>
#include <stddef.h>
#include <cstring>
#include <memory>
#include <string>

#include "server.h"
#include "lane_kernels.h"
#include "memory_snapshot_manager.hpp"

namespace janus {

namespace {
// Destroy whatever the slot holds, then construct the new value there: the
// out-parameter protocol of every carrier kernel (see server.cc).
template <typename T, typename V>
void construct_into(T* dst, V&& value) {
  std::destroy_at(dst);
  new (dst) T(std::forward<V>(value));
}
}  // namespace

extern "C" {

// The C++ lanes' send, defined in server_seam_cpp.cc; only this file calls it.
void raft_lane_send_install_snapshot(
    RaftServerBase* self, uint16_t site_id, uint32_t partition_id,
    uint64_t term, uint64_t leader_id, uint64_t last_included_index,
    uint64_t last_included_term, const uint8_t* data, size_t len, void* ctx);

bool raft_snapshot_manager_is_set( const rusty::RaftSnapshotManagerPtr* manager) {
  return *manager != nullptr;
}

void raft_snapshot_manager_ptr_clone_into(const rusty::RaftSnapshotManagerPtr* src,
                                          rusty::RaftSnapshotManagerPtr* dst) {
  construct_into(dst, *src);
}

void raft_destroy_snapshot_manager_ptr(rusty::RaftSnapshotManagerPtr* p) { std::destroy_at(p); }

// Memory-only Raft has no on-disk snapshot store. A manager injected through
// SetSnapshotManager() before Setup keeps the latest snapshot it holds;
// otherwise start from an empty in-memory manager.
void raft_snapshot_recovery_pick_manager(
    const rusty::RaftSnapshotManagerPtr* current,
    rusty::RaftSnapshotManagerPtr* out) {
  if (*current) {
    construct_into(out, *current);
    return;
  }
  construct_into(out, std::make_shared<janus::raft::MemorySnapshotManager>());
}

bool raft_snapshot_manager_latest(
    const rusty::RaftSnapshotManagerPtr* manager, uint64_t* index,
    uint64_t* term) {
  const auto latest = (*manager)->GetLatestSnapshot();
  if (latest.is_none()) {
    return false;
  }
  const auto discovered = latest.unwrap();
  *index = discovered.last_included_index;
  *term = discovered.last_included_term;
  return true;
}

bool raft_snapshot_manager_load(const rusty::RaftSnapshotManagerPtr* manager,
                                rusty::RaftByteString* data, uint64_t* index,
                                uint64_t* term, uint64_t* size_bytes) {
  janus::raft::SnapshotMetadata metadata;
  if (!(*manager)->LoadLatestSnapshot(&metadata, data)) {
    return false;
  }
  *index = metadata.last_included_index;
  *term = metadata.last_included_term;
  *size_bytes = metadata.size_bytes;
  return true;
}

// The store line of the two HOST kernels (the create save and the install
// save): what each held inline as `(*snapshot_manager)->TakeSnapshot(...)`.
bool raft_snapshot_store_save(const rusty::RaftSnapshotManagerPtr* manager,
                              uint64_t index, uint64_t term,
                              const uint8_t* data, size_t len) {
  return (*manager)->TakeSnapshot(index, term,
                                  reinterpret_cast<const char*>(data), len);
}

// Loads the latest snapshot and sends it. Returns false when there is no
// snapshot to load; the caller logs that and skips the follower either way.
// CALLER MUST HOLD mtx_.
bool raft_phase1_load_and_send_snapshot(
    RaftServerBase* self,
    const rusty::RaftSnapshotManagerPtr* snapshot_manager,
    const rusty::RaftAsyncCallbackLifetimePtr* lifetime,
    uint16_t self_site_id, uint32_t partition_id, uint64_t send_term,
    uint16_t site_id, size_t ord) {
  janus::raft::SnapshotMetadata snap_meta;
  std::string snap_data;
  if (!(*snapshot_manager)->LoadLatestSnapshot(&snap_meta, &snap_data)) {
    return false;
  }
  const uint64_t snap_last_idx = snap_meta.last_included_index;
  const uint64_t snap_last_term = snap_meta.last_included_term;
  // The reply handler's context: everything the completion needs, captured
  // by value, freed exactly once by raft_snapshot_reply_free. The send itself
  // is the lane's (raft_lane_send_install_snapshot): RaftCommo on the C++
  // lane, RaftTransport on the Rust lane.
  void* ctx = raft_snapshot_reply_ctx_new(lifetime, site_id, self_site_id, ord,
                                          snap_last_idx, send_term);
  raft_lane_send_install_snapshot(
      self, site_id, partition_id, send_term, self_site_id, snap_last_idx,
      snap_last_term, reinterpret_cast<const uint8_t*>(snap_data.data()),
      snap_data.size(), ctx);
  return true;
}

#ifdef RAFT_TEST_CORO
// --- The snapshot manager, for the Rust lab harness ------------------------
//
// Everything a case compares across a snapshot operation, in one struct so a
// before/after comparison is one call. Declared here rather than in server.h's
// inline block because it is lab-only: no production build sees it.
struct RaftLabSnapshotProbe {
  bool present;
  uint64_t last_included_index;
  uint64_t last_included_term;
  uint64_t timestamp_ms;
  uint64_t size_bytes;
  // FNV-1a over the checksum STRING (SnapshotMetadata::checksum is a
  // std::string) and over the payload. A digest compares as well as a copy
  // for a before/after assertion and marshals nothing into Rust.
  uint64_t checksum_digest;
  uint64_t data_digest;
  uint64_t count;
};

namespace {
uint64_t lab_fnv1a(const std::string& bytes) {
  uint64_t digest = 0xcbf29ce484222325ull;
  for (const char byte : bytes) {
    digest ^= static_cast<unsigned char>(byte);
    digest *= 0x100000001b3ull;
  }
  return digest;
}
}  // namespace

void raft_lab_new_snapshot_manager(rusty::RaftSnapshotManagerPtr* out) {
  construct_into(out, std::make_shared<janus::raft::MemorySnapshotManager>());
}

uint64_t raft_lab_snapshot_delete_all(
    const rusty::RaftSnapshotManagerPtr* manager) {
  if (!*manager) {
    return 0;
  }
  return static_cast<uint64_t>((*manager)->DeleteAllSnapshots());
}

// One call for everything a case compares across an operation: the metadata,
// a digest of the payload, and how many snapshots the manager holds. Test 58
// asserts five metadata fields, the bytes and the count are all unchanged; a
// digest says that as well as a copy would and does not marshal a std::string
// into Rust.
void raft_lab_snapshot_probe(const rusty::RaftSnapshotManagerPtr* manager,
                             RaftLabSnapshotProbe* out) {
  *out = RaftLabSnapshotProbe{};
  if (!*manager) {
    return;
  }
  janus::raft::SnapshotMetadata metadata;
  std::string data;
  if (!(*manager)->LoadLatestSnapshot(&metadata, &data)) {
    return;
  }
  out->present = true;
  out->last_included_index = metadata.last_included_index;
  out->last_included_term = metadata.last_included_term;
  out->timestamp_ms = metadata.timestamp_ms;
  out->size_bytes = static_cast<uint64_t>(metadata.size_bytes);
  out->checksum_digest = lab_fnv1a(metadata.checksum);
  out->data_digest = lab_fnv1a(data);
  out->count = static_cast<uint64_t>((*manager)->ListSnapshots().size());
}

// Copy one manager's latest checkpoint into another. What
// InstallAndSeedSnapshotManager does when rotating a manager on a replica
// that has ALREADY compacted: a boundary means nothing without its exact
// bytes, so the checkpoint is copied rather than regenerated.
bool raft_lab_snapshot_copy_latest(const rusty::RaftSnapshotManagerPtr* src,
                                   const rusty::RaftSnapshotManagerPtr* dst) {
  if (!*src || !*dst) {
    return false;
  }
  janus::raft::SnapshotMetadata metadata;
  std::string data;
  if (!(*src)->LoadLatestSnapshot(&metadata, &data)) {
    return false;
  }
  return (*dst)->TakeSnapshot(metadata.last_included_index,
                              metadata.last_included_term,
                              data.data(), data.size());
}
#endif  // RAFT_TEST_CORO

}  // extern "C"
}  // namespace janus
