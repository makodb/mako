#pragma once

// Raft's RPC transport, from C++: raft-rt's RaftTransport, which owns Raft's
// poll thread, its RPC server and its peer clients
// (src/deptran/raft/rt/src/transport.rs). The workers (RaftWorker,
// ServerWorker, raft_main_helper's stubs) and RaftFrame reach it only through
// this header: the C ABI below, defined in Rust (raft_rt_run_lab in
// rt/src/lab_runtime.rs, raft_rt_install_trace in rt/src/trace.rs), and the
// three helpers at the end, which need Mako's C++ (raft_transport.cc).
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
#include <functional>
#include <string>

namespace janus {

class RaftServer;
struct RaftServerBase;
struct RaftTransport;
// srpc::server::Server, a kSingleGroup stub listener (raft_transport_serve_stub).
struct RaftStubServer;

extern "C" {

RaftTransport* raft_transport_new();

// [M0] trace kit: fill raft-rt's hook slots (rt/src/trace.rs).
void raft_rt_install_trace(void (*through)(int32_t, uint64_t, uint64_t),
                           uint64_t (*now_us)());

// Bind and start serving Raft's RPCs for `server`, admission CLOSED, and bind
// the transport to `server` for the seam kernels. 0 on success.
int32_t raft_transport_serve(RaftTransport* t, RaftServerBase* server,
                             const char* bind_addr);

// Connect to one site (retrying for up to 120 s, as Communicator does) and
// record it in `par_id`. False means already known, or it never came up.
// Disk builds try for 1 s, then leave the site to a dial thread (B22).
bool raft_transport_add_peer(RaftTransport* t, uint32_t par_id,
                             uint16_t site_id, const char* addr);

// After every add_peer: wait (120 s) until each partition has a majority
// connected, this site included. Immediate in a memory build.
bool raft_transport_wait_majority(RaftTransport* t);

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

// The RaftLab harness on this thread's Rust reactor. Defined only when
// raft-rt is built with raft_test, which CMake turns on exactly when
// RAFT_TEST_CORO is defined.
int32_t raft_rt_run_lab();

}  // extern "C"

namespace raft_transport {

// Bind and serve Raft's RPCs for `server` on `bind_addr`, admission closed,
// with the trace kit's hooks installed first when tracing is on. Aborts the
// process on a bind failure (Log_fatal).
RaftTransport* Serve(RaftServer* server, const std::string& bind_addr);

// Connect to every site of every partition in the config, this one
// included, exactly as Communicator's constructor does, so a partition's
// recorded size is its configured size; then wait for a majority of each.
void ConnectPeers(RaftTransport* t);

// Run `job` once on the transport's poll thread. The Raft fibers EnsureSetup
// spawns belong to whichever reactor runs it, so this is how they end up on
// the Rust one.
void Post(RaftTransport* t, std::function<void()> job);

}  // namespace raft_transport
}  // namespace janus
