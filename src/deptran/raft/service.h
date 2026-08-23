#pragma once

#include "__dep__.h"
#include "constants.h"
#include "../command.h"
#include "deptran/procedure.h"
#include "../command_marshaler.h"
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

class SimpleCommand;
namespace janus {

class TxLogServer;
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
  rusty::Option<rusty::Arc<rrr::PollThread>> poll_thread_;

  RaftServiceImpl(TxLogServer* sched, rusty::Arc<rrr::PollThread> poll_thread);

  // Called by test framework during Kill/Restart to update server pointer
  static void UpdateServer(siteid_t site_id, RaftServer* new_svr);

  // Called by test framework during Restart to get the original poll thread
  static rusty::Option<rusty::Arc<rrr::PollThread>> GetPollThread(siteid_t site_id);

  // Called by RPC handlers - lock-free atomic read
  RaftServer* GetServer();

  // Generated fiber-RPC overrides. The rrr codegen wraps each one in a
  // Fiber::create_run; we return a packed response struct and the
  // framework sends the reply on fiber completion. No DeferredReply.
  rusty::Result<RpcVoteResponse,                rrr::i32> Vote(const RpcVoteRequest& req) override;
  rusty::Result<RpcVoteDurableResponse,         rrr::i32> VoteDurable(const RpcVoteDurableRequest& req) override;
  rusty::Result<RpcAppendEntriesResponse,       rrr::i32> AppendEntries(const RpcAppendEntriesRequest& req) override;
  rusty::Result<RpcEmptyAppendEntriesResponse,  rrr::i32> EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req) override;
  rusty::Result<RpcAppendEntriesDurableResponse, rrr::i32> AppendEntriesDurable(const RpcAppendEntriesDurableRequest& req) override;
  rusty::Result<RpcTimeoutNowResponse,          rrr::i32> TimeoutNow(const RpcTimeoutNowRequest& req) override;
  rusty::Result<RpcNotifyRestartResponse,       rrr::i32> NotifyRestart(const RpcNotifyRestartRequest& req) override;
  rusty::Result<RpcInstallSnapshotResponse,     rrr::i32> InstallSnapshot(const RpcInstallSnapshotRequest& req) override;
  rusty::Result<RpcAddServerResponse,           rrr::i32> AddServer(const RpcAddServerRequest& req) override;
  rusty::Result<RpcRemoveServerResponse,        rrr::i32> RemoveServer(const RpcRemoveServerRequest& req) override;
};

} // namespace janus
