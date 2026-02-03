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

class SimpleCommand;
namespace janus {

class TxLogServer;
class RaftServer;

// @safe
class RaftServiceImpl : public RaftService {
 public:
  RaftServer* svr_;
  RaftServiceImpl(TxLogServer* sched);

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

};

} // namespace janus
