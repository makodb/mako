#pragma once

#include "__dep__.h"
#include "constants.h"
#include "../rcc_rpc.h"
#include "server.h"
#include <cstdint>

// @external: {
//   verify: [safe, (bool) -> void],
//   clock_gettime: [safe, (int, timespec*) -> int],
//   srand: [safe, (unsigned int) -> void]
// }

namespace janus {

class RaftServer;

// @unsafe - inherits from non-@interface RaftService
class RaftServiceImpl : public RaftService {
 public:
  // Non-owning pointer to the Raft server this proxy fronts. Set once at
  // construction and never cleared. Destruction order gives the server a
  // longer life than the services registered on it, but that alone says
  // nothing about handlers still executing: a handler fiber that already
  // passed the availability check below can be mid-call while another thread
  // tears the server down. The owner of the rrr::Server must therefore drain
  // its in-flight requests (set_admission_ready(false) + drain()) before it
  // destroys either the server or this service -- see RaftWorker::ShutDown
  // and destroy_stub_servers().
  RaftSpecific* svr_{nullptr};

  // @unsafe - Stores the raw Raft server pointer. The poll thread is owned by
  // the rrr::Server that registers this proxy and is not passed in.
  explicit RaftServiceImpl(RaftSpecific* sched);

  // Generated fiber-RPC overrides. The rrr codegen wraps each one in a
  // Fiber::create_run; we return a packed response struct and the
  // framework sends the reply on fiber completion. No DeferredReply.
  rusty::Result<RpcVoteResponse,                rrr::i32> Vote(const RpcVoteRequest& req) override;
  rusty::Result<RpcAppendEntriesResponse,       rrr::i32> AppendEntries(const RpcAppendEntriesRequest& req) override;
  rusty::Result<RpcEmptyAppendEntriesResponse,  rrr::i32> EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req) override;
  rusty::Result<RpcInstallSnapshotResponse,     rrr::i32> InstallSnapshot(const RpcInstallSnapshotRequest& req) override;
};

} // namespace janus
