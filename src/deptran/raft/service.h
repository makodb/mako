#pragma once

#include "__dep__.h"
#include "constants.h"
#include "../rcc_rpc.h"
#include "server.h"
#include <atomic>
#include <mutex>
#include <map>

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
  // Static registry to find services by site_id (for Kill/Restart support)
  static std::map<siteid_t, RaftServiceImpl*> service_registry_;
  static std::mutex registry_mutex_;

  // Atomic pointer - allows lock-free reads on RPC hot path
  std::atomic<RaftServer*> svr_;
  siteid_t site_id_;

  // Store the poll thread so Restart() can reuse the original, ensuring
  // inbound and outbound RPCs share a thread.
  rusty::Option<rusty::Arc<srpc::PollThread>> poll_thread_;

  RaftServiceImpl(RaftServer* sched, rusty::Arc<srpc::PollThread> poll_thread);

  // Called by test framework during Kill/Restart to update server pointer
  static void UpdateServer(siteid_t site_id, RaftServer* new_svr);

  // Called by test framework during Restart to get the original poll thread
  static rusty::Option<rusty::Arc<srpc::PollThread>> GetPollThread(siteid_t site_id);

  // Called by RPC handlers - lock-free atomic read
  RaftServer* GetServer();

  // Generated fiber-RPC overrides. The srpc codegen wraps each one in a
  // Fiber::create_run; we return a packed response struct and the
  // framework sends the reply on fiber completion. No DeferredReply.
  rusty::Result<RpcVoteResponse,                srpc::i32> Vote(const RpcVoteRequest& req) override;
  rusty::Result<RpcVoteDurableResponse,         srpc::i32> VoteDurable(const RpcVoteDurableRequest& req) override;
  rusty::Result<RpcAppendEntriesResponse,       srpc::i32> AppendEntries(const RpcAppendEntriesRequest& req) override;
  rusty::Result<RpcEmptyAppendEntriesResponse,  srpc::i32> EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req) override;
  rusty::Result<RpcAppendEntriesDurableResponse, srpc::i32> AppendEntriesDurable(const RpcAppendEntriesDurableRequest& req) override;
  rusty::Result<RpcTimeoutNowResponse,          srpc::i32> TimeoutNow(const RpcTimeoutNowRequest& req) override;
  rusty::Result<RpcNotifyRestartResponse,       srpc::i32> NotifyRestart(const RpcNotifyRestartRequest& req) override;
  rusty::Result<RpcInstallSnapshotResponse,     srpc::i32> InstallSnapshot(const RpcInstallSnapshotRequest& req) override;
  rusty::Result<RpcAddServerResponse,           srpc::i32> AddServer(const RpcAddServerRequest& req) override;
  rusty::Result<RpcRemoveServerResponse,        srpc::i32> RemoveServer(const RpcRemoveServerRequest& req) override;
};

} // namespace janus
