#pragma once

// Raft's RPC endpoint, per lane.
//
// The workers (RaftWorker, ServerWorker, raft_main_helper's stubs) build
// Raft's RPC plumbing in one of two ways, selected at build time by
// MAKO_RAFT_LANE (CMakeLists.txt):
//
//   hybrid / cpp  the C++ srpc runtime: srpc::Server + RaftServiceImpl +
//                 RaftCommo on a C++ PollThread. That code is where it always
//                 was, in the workers themselves.
//   rust          the Rust srpc runtime: raft-rt's RaftTransport, which owns
//                 its poll thread, its server and its peer clients
//                 (src/deptran/raft/rt/src/transport.rs). The workers reach it
//                 only through the functions below, which are defined in
//                 raft_lane_rust.cc and compiled only on that lane.
//
// The C++ lane never calls these; the workers' `#if MAKO_RAFT_LANE_RUST`
// hooks are the only callers, one line each.

#include <cstdint>
#include <functional>
#include <string>

namespace janus {

class RaftServer;

// Opaque: raft-rt's RaftTransport.
struct RaftTransport;

namespace raft_lane {

// Bind and serve Raft's RPCs for `server` on `bind_addr`, admission closed.
// Aborts the process on a bind failure, as the C++ lane's Log_fatal does.
RaftTransport* Serve(RaftServer* server, const std::string& bind_addr);

// Connect to every site of every partition in the config, this one
// included, exactly as Communicator's constructor does, so a partition's
// recorded size is its configured size.
void ConnectPeers(RaftTransport* t);

// Run `job` once on the transport's poll thread. The Raft fibers EnsureSetup
// spawns belong to whichever reactor runs it, so this is how they end up on
// the Rust one.
void Post(RaftTransport* t, std::function<void()> job);

void SetAdmissionReady(RaftTransport* t, bool ready);
bool Drain(RaftTransport* t, uint64_t timeout_ms);

// Drop the RPC server (keeping the poll thread and clients alive), then later
// destroy the rest; the two halves of the C++ lane's `delete rpc_server_` and
// poll-thread shutdown, in the same order.
void CloseServer(RaftTransport* t);
void Destroy(RaftTransport* t);

uint64_t RpcCount(const RaftTransport* t);

// kSingleGroup stub sites: another listener for the same one server on the
// same poll thread. Returns an opaque stub handle.
void* ServeStub(RaftTransport* t, RaftServer* server, const std::string& bind_addr);
void StubSetAdmissionReady(void* stub, bool ready);
bool StubDrain(void* stub, uint64_t timeout_ms);
void StubDestroy(void* stub);

// The lab harness (RAFT_TEST builds): run raft_lab_rust_run on a Rust fiber
// on the calling thread's Rust reactor, and return its verdict.
int RunLab();

}  // namespace raft_lane
}  // namespace janus
