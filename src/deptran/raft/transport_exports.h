#pragma once

// The C ABI over RaftTransport -- Raft's RPC transport on the Rust srpc lane
// (stage 3e). The definitions are Rust, in src/deptran/raft/src/transport.rs.
//
// Hand-written, unlike server_exports.h: that header is generated from the
// signature table in scripts/raft_gen_exports.py, whose subject is methods of
// RaftServerBase. These are methods of a different type and there are six of
// them, so a second generator would cost more than it saves.
//
// THREADING. The transport is not Send: srpc's Client holds RefCell and Cell
// fields, so a client belongs to the poll thread that created it. Call these
// from the worker thread that made the transport -- except
// raft_transport_set_network_enabled, which touches one atomic and is what
// the lab suite's Disconnect reaches from elsewhere.

#include <cstdint>

namespace janus {

struct RaftServerBase;
struct RaftTransport;

extern "C" {

RaftTransport* raft_transport_new();
void raft_transport_delete(RaftTransport* t);

// Bind and start serving Raft's four RPCs for `server`. Returns 0 on success,
// which is srpc::Server::start's convention.
int32_t raft_transport_serve(RaftTransport* t, RaftServerBase* server,
                             const char* bind_addr);

// Connect to one peer and record it in `par_id`. False means already known,
// or the connection did not come up.
bool raft_transport_add_peer(RaftTransport* t, uint32_t par_id,
                             uint16_t site_id, const char* addr);

void raft_transport_set_network_enabled(RaftTransport* t, bool enabled);

// Close admission and wait for handlers already inside. The barrier
// RaftWorker::ShutDown needs before the server is destroyed.
bool raft_transport_drain(RaftTransport* t, uint64_t timeout_ms);

}  // extern "C"
}  // namespace janus
