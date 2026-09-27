#pragma once

// The kernels that cross between server.cc (HOST: Mako's objects -- the
// Command payload, the snapshot manager, embedder callbacks; linked in every
// lane) and a lane's runtime seam (server_seam_cpp.cc on the C++ lanes,
// raft-rt's src/seam.rs on the Rust lane). Every other kernel goes from the
// Rust core to one side or the other and is declared in the core's extern
// blocks; these are called by C++ on both sides, so they need a header.

#include <cstddef>
#include <cstdint>

namespace janus {

struct RaftServerBase;

extern "C" {

// LANE: send an InstallSnapshot the host has loaded. Delivers the reply to
// `ctx` through raft_snapshot_reply_deliver at most once (0 on any failure,
// inline when there is no peer) and frees it through raft_snapshot_reply_free
// exactly once.
void raft_lane_send_install_snapshot(
    RaftServerBase* self, uint16_t site_id, uint32_t partition_id,
    uint64_t term, uint64_t leader_id, uint64_t last_included_index,
    uint64_t last_included_term, const uint8_t* data, size_t len, void* ctx);

// HOST: the reply side of the above.
void raft_snapshot_reply_deliver(void* ctx, uint64_t follower_term);
void raft_snapshot_reply_free(void* ctx);

// HOST: InstallSnapshot RPC counters, bumped by every lane's send path and
// receive handler and read by raft_bench (phase N0).
void raft_install_rpc_note_sent(uint64_t bytes);
void raft_install_rpc_note_received();
void raft_install_rpc_stats(uint64_t* sent, uint64_t* bytes_sent, uint64_t* received);

}  // extern "C"
}  // namespace janus
