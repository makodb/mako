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
#include "macros.h"
#include <atomic>
#include <mutex>
#include <map>

class SimpleCommand;
namespace janus {

class TxLogServer;
class RaftServer;

// @safe
class RaftServiceImpl : public RaftService {
 public:
  // Static registry to find services by site_id (for Kill/Restart support)
  static std::map<siteid_t, RaftServiceImpl*> service_registry_;
  static std::mutex registry_mutex_;

  // Atomic pointer - allows lock-free reads on RPC hot path
  std::atomic<RaftServer*> svr_;
  siteid_t site_id_;

  RaftServiceImpl(TxLogServer* sched);

  // Called by test framework during Kill/Restart to update server pointer
  static void UpdateServer(siteid_t site_id, RaftServer* new_svr);

  // Called by RPC handlers - lock-free atomic read
  RaftServer* GetServer();

  RpcHandler(Vote, 6,
             const uint64_t&, lst_log_idx,
             const ballot_t&, lst_log_term,
             const siteid_t&, can_id,
             const ballot_t&, can_term,
             ballot_t*, reply_term,
             bool_t*, vote_granted) {
    *reply_term = can_term;
    *vote_granted = false;
  }

  RpcHandler(AppendEntries, 12,
             const uint64_t&, slot,
             const ballot_t&, ballot,
             const uint64_t&, leaderCurrentTerm,
             const siteid_t&, leaderSiteId,
             const uint64_t&, leaderPrevLogIndex,
             const uint64_t&, leaderPrevLogTerm,
             const uint64_t&, leaderCommitIndex,
             const MarshallDeputy&, cmd,
             const uint64_t&, leaderNextLogTerm,
             uint64_t*, followerAppendOK,
             uint64_t*, followerCurrentTerm,
             uint64_t*, followerLastLogIndex) {
    *followerAppendOK = false;
    *followerCurrentTerm = 0;
    *followerLastLogIndex = 0;
  }

  RpcHandler(EmptyAppendEntries, 11,
             const uint64_t&, slot,
             const ballot_t&, ballot,
             const uint64_t&, leaderCurrentTerm,
             const siteid_t&, leaderSiteId,
             const uint64_t&, leaderPrevLogIndex,
             const uint64_t&, leaderPrevLogTerm,
             const uint64_t&, leaderCommitIndex,
             const bool_t&, trigger_election_now,
             uint64_t*, followerAppendOK,
             uint64_t*, followerCurrentTerm,
             uint64_t*, followerLastLogIndex) {
    *followerAppendOK = false;
    *followerCurrentTerm = 0;
    *followerLastLogIndex = 0;
  }

  RpcHandler(TimeoutNow, 4,
             const uint64_t&, leaderTerm,
             const siteid_t&, leaderSiteId,
             uint64_t*, followerTerm,
             bool_t*, success) {
    *followerTerm = 0;
    *success = false;
  }

  RpcHandler(NotifyRestart, 2,
             const siteid_t&, restartedSiteId,
             bool_t*, acknowledged) {
    *acknowledged = false;
  }

};

} // namespace janus
