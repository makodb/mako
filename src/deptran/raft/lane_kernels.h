#pragma once

// The kernels that cross between server.cc (HOST: Mako's objects -- the
// Command payload, the snapshot manager, embedder callbacks; linked in every
// lane) and the runtime seam (raft-rt's src/seam.rs; the C++ lanes' seam went
// with those lanes, docs/verus/modification-plan.md Q9). Every other kernel goes from the
// Rust core to one side or the other and is declared in the core's extern
// blocks; these are called by C++ on both sides, so they need a header.

#include <cstddef>
#include <cstdint>

namespace janus {

struct RaftServerBase;

extern "C" {

// LANE: the snapshot store's accessors that HOST C++ calls (plan N4), defined
// in raft-rt's rt/src/snapshot.rs over the Rust store. The carrier types
// are server.h's, so include this after it.
bool raft_snapshot_manager_is_set(const rusty::RaftSnapshotManagerPtr* manager);
void raft_snapshot_manager_ptr_clone_into(const rusty::RaftSnapshotManagerPtr* src,
                                          rusty::RaftSnapshotManagerPtr* dst);
void raft_destroy_snapshot_manager_ptr(rusty::RaftSnapshotManagerPtr* p);
bool raft_snapshot_manager_latest(const rusty::RaftSnapshotManagerPtr* manager,
                                  uint64_t* index, uint64_t* term);
bool raft_snapshot_manager_load(const rusty::RaftSnapshotManagerPtr* manager,
                                rusty::RaftByteString* data, uint64_t* index,
                                uint64_t* term, uint64_t* size_bytes);
bool raft_snapshot_store_save(const rusty::RaftSnapshotManagerPtr* manager,
                              uint64_t index, uint64_t term,
                              const uint8_t* data, size_t len);

// HOST: the reply context each lane's raft_phase1_load_and_send_snapshot
// hands its send, and the reply side. The context is delivered to at most
// once (0 on any failure, inline when there is no peer) and freed through
// raft_snapshot_reply_free exactly once.
void* raft_snapshot_reply_ctx_new(const rusty::RaftAsyncCallbackLifetimePtr* lifetime,
                                  uint16_t site_id, uint16_t self_site_id, size_t ord,
                                  uint64_t snap_last_idx, uint64_t send_term);
void raft_snapshot_reply_deliver(void* ctx, uint64_t follower_term);
void raft_snapshot_reply_free(void* ctx);

// HOST: InstallSnapshot RPC counters, bumped by every lane's send path and
// receive handler and read by raft_bench (phase N0).
void raft_install_rpc_note_sent(uint64_t bytes);
void raft_install_rpc_note_received();
void raft_install_rpc_stats(uint64_t* sent, uint64_t* bytes_sent, uint64_t* received);

}  // extern "C"
}  // namespace janus
