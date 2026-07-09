#pragma once

#include "__dep__.h"
#include "constants.h"
#include "../rcc/graph.h"
#include "../rcc/graph_marshaler.h"
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

// @unsafe - RPC service adapter. rrr owns service object lifetime after
// registration; this class only tracks a borrowed/atomic RaftServer pointer.
class RaftServiceImpl : public RaftService {
 public:
  // Static registry to find services by site_id (for Kill/Restart support).
  // @unsafe - stores borrowed service pointers; registry does not own entries.
  static std::map<siteid_t, RaftServiceImpl*> service_registry_;
  static std::mutex registry_mutex_;

  // @unsafe - borrowed atomic server pointer. Updated during Kill/Restart;
  // RaftServiceImpl never owns or deletes the server. A nullptr means this
  // service remains registered but the replica is currently killed/down.
  std::atomic<RaftServer*> svr_;
  siteid_t site_id_;

  // Store the poll thread so Restart() can reuse the original, ensuring
  // inbound and outbound RPCs share a thread.
  // @safe - Arc/Option manages shared PollThread lifetime.
  rusty::Option<rusty::Arc<rrr::PollThread>> poll_thread_;

  RaftServiceImpl(TxLogServer* sched, rusty::Arc<rrr::PollThread> poll_thread);

  // Called by test framework during Kill/Restart to publish the current
  // borrowed server pointer. Passing nullptr marks the service as down.
  static void UpdateServer(siteid_t site_id, RaftServer* new_svr);

  // Called by test framework during Restart to get the original poll thread.
  static rusty::Option<rusty::Arc<rrr::PollThread>> GetPollThread(siteid_t site_id);

  // Called by RPC handlers - lock-free atomic read of the borrowed server.
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
