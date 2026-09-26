#pragma once

// The C ABI over RaftTransport -- Raft's RPC transport on the Rust srpc lane.
// The definitions are Rust, in src/deptran/raft/rt/src/transport.rs (and
// raft_rt_run_lab in rt/src/lab_runtime.rs). Included only by
// raft_lane_rust.cc, which is compiled only for MAKO_RAFT_LANE=rust.
//
// Hand-written, unlike server_exports.h: that header is generated from the
// signature table in scripts/raft_gen_exports.py, whose subject is methods of
// RaftServerBase. These are methods of a different type.
//
// THREADING. The transport is not Send: srpc's Client holds RefCell and Cell
// fields, so a client belongs to the poll thread that owns it. Call these
// from the worker thread that made the transport -- except
// raft_transport_set_network_enabled (one atomic) and raft_transport_post (a
// channel send).

#include <cstdint>

namespace janus {

struct RaftServerBase;
struct RaftTransport;
// srpc::server::Server, a kSingleGroup stub listener (raft_transport_serve_stub).
struct RaftStubServer;

extern "C" {

RaftTransport* raft_transport_new();

// Bind and start serving Raft's RPCs for `server`, admission CLOSED, and bind
// the transport to `server` for the seam kernels. 0 on success.
int32_t raft_transport_serve(RaftTransport* t, RaftServerBase* server,
                             const char* bind_addr);

// Connect to one site (retrying for up to 120 s, as Communicator does) and
// record it in `par_id`. False means already known, or it never came up.
bool raft_transport_add_peer(RaftTransport* t, uint32_t par_id,
                             uint16_t site_id, const char* addr);

void raft_transport_set_network_enabled(RaftTransport* t, bool enabled);
void raft_transport_set_admission_ready(RaftTransport* t, bool ready);
int32_t raft_transport_bound_port(const RaftTransport* t);
uint64_t raft_transport_rpc_count(const RaftTransport* t);

// Run f(ctx) once on the transport's poll thread.
void raft_transport_post(RaftTransport* t, void (*f)(void*), void* ctx);

// Close admission and wait for handlers already inside.
bool raft_transport_drain(RaftTransport* t, uint64_t timeout_ms);

// Drop the RPC server and unbind, keeping the poll thread and clients.
void raft_transport_close_server(RaftTransport* t);

// Close the clients, stop the poll thread, free the transport.
void raft_transport_delete(RaftTransport* t);

// kSingleGroup stubs: another listener for `server` on this transport's poll
// thread (no new thread, no registry rebind). Null on a bind failure.
RaftStubServer* raft_transport_serve_stub(RaftTransport* t,
                                          RaftServerBase* server,
                                          const char* bind_addr);
void raft_stub_server_set_admission_ready(RaftStubServer* s, bool ready);
bool raft_stub_server_drain(RaftStubServer* s, uint64_t timeout_ms);
void raft_stub_server_delete(RaftStubServer* s);

// The RaftLab harness on this thread's Rust reactor (RAFT_TEST builds).
int32_t raft_rt_run_lab();

}  // extern "C"
}  // namespace janus
