#include "service.h"
#include "server.h"

#include "srpc/srpc.hpp"
#include <rusty/slice.hpp>

// @external: {
//   Log_info:   [safe, (...) -> void]
//   Log_debug:  [safe, (...) -> void]
//   Log_warn:   [safe, (...) -> void]
//   Log_error:  [safe, (...) -> void]
//   verify:     [safe, (...) -> void]
//   clock_gettime: [safe, (...) -> int]
//   srand:      [safe, (...) -> void]
// }

namespace janus {

using rusty::Result;

// =====================================================================
// Fiber-RPC handlers.
//
// Each method here is invoked by the srpc-generated wrapper on a fresh
// Fiber (see src/srpc/pylib/simplerpcgen/lang_cpp.py). We do synchronous
// work and return the response struct by value. The framework marshals and sends the
// reply when the fiber completes; no DeferredReply anywhere.
//
// Disconnected/killed server path: the response carries the same
// defaults the old RpcHandler macro's OnDisconnected##name bodies used,
// and we return Ok(resp). We deliberately do NOT return Err(...): the
// peer code treats nonzero error codes as "drop this reply", which
// would hide the disconnected-server signal that other code paths
// depend on (e.g., lost-RPC detection in SendAppendEntries).
//
// THE ADMISSION GATE IS NO LONGER HERE. It used to be three ABI crossings
// per request -- IsDisconnected, IsRpcReady, then the handler -- and it is
// now one: RaftServerBase::ServeVote and friends apply the gate and write
// the unavailable reply themselves. What is left on this side is the null
// test, because a null server has no method to call.
// =====================================================================

Result<RaftService::RpcVoteResponse, srpc::i32>
RaftServiceImpl::Vote(const RpcVoteRequest& req) const {
  RpcVoteResponse resp{};
  RaftSpecific* svr = svr_;
  if (svr == nullptr) {
    resp.max_ballot = req.cur_term;
    resp.vote_granted = false;
    return Result<RpcVoteResponse, srpc::i32>::Ok(resp);
  }
  svr->ServeVote(req.lst_log_idx, req.lst_log_term,
                 req.site_id, req.cur_term,
                 &resp.max_ballot, &resp.vote_granted);
  return Result<RpcVoteResponse, srpc::i32>::Ok(resp);
}

Result<RaftService::RpcAppendEntriesResponse, srpc::i32>
RaftServiceImpl::AppendEntries(const RpcAppendEntriesRequest& req) const {
  RpcAppendEntriesResponse resp{};
  RaftSpecific* svr = svr_;
  if (svr == nullptr) {
    resp.followerAppendOK = 0;
    resp.followerCurrentTerm = 0;
    resp.followerLastLogIndex = 0;
    return Result<RpcAppendEntriesResponse, srpc::i32>::Ok(resp);
  }
  svr->ServeAppendEntries(req.leaderCurrentTerm,
                          req.leaderSiteId, req.leaderPrevLogIndex,
                          req.leaderPrevLogTerm, req.leaderCommitIndex,
                          req.cmd, req.leaderNextLogTerm,
                          &resp.followerAppendOK, &resp.followerCurrentTerm,
                          &resp.followerLastLogIndex);
  return Result<RpcAppendEntriesResponse, srpc::i32>::Ok(resp);
}

Result<RaftService::RpcEmptyAppendEntriesResponse, srpc::i32>
RaftServiceImpl::EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req) const {
  Log_debug("RaftServiceImpl: EmptyAppendEntries answering leader {}", req.leaderSiteId);
  RpcEmptyAppendEntriesResponse resp{};
  RaftSpecific* svr = svr_;
  if (svr == nullptr) {
    resp.followerAppendOK = 0;
    resp.followerCurrentTerm = 0;
    resp.followerLastLogIndex = 0;
    return Result<RpcEmptyAppendEntriesResponse, srpc::i32>::Ok(resp);
  }
  // ServeAppendEntries uses the same fields as the non-empty variant with
  // an empty cmd and leaderNextLogTerm == 0 (heartbeat path).
  // followerAppendOK/Term/LastLogIndex are shared layout with the non-empty
  // response, so we can pass pointers directly into our resp struct.
  svr->ServeAppendEntries(req.leaderCurrentTerm,
                          req.leaderSiteId, req.leaderPrevLogIndex,
                          req.leaderPrevLogTerm, req.leaderCommitIndex,
                          janus::Command{}, 0,
                          &resp.followerAppendOK, &resp.followerCurrentTerm,
                          &resp.followerLastLogIndex);
  return Result<RpcEmptyAppendEntriesResponse, srpc::i32>::Ok(resp);
}

Result<RaftService::RpcInstallSnapshotResponse, srpc::i32>
RaftServiceImpl::InstallSnapshot(const RpcInstallSnapshotRequest& req) const {
  RpcInstallSnapshotResponse resp{};
  RaftSpecific* svr = svr_;
  if (svr == nullptr) {
    resp.term_out = 0;
    return Result<RpcInstallSnapshotResponse, srpc::i32>::Ok(resp);
  }
  svr->ServeInstallSnapshot(req.term, req.leader_id,
                            req.last_included_index, req.last_included_term,
                            req.data, &resp.term_out);
  return Result<RpcInstallSnapshotResponse, srpc::i32>::Ok(resp);
}

// @unsafe - Stores the raw Raft server pointer for the handlers above.
RaftServiceImpl::RaftServiceImpl(RaftSpecific* sched)
    : svr_(sched) {
  struct timespec curr_time;
  clock_gettime(CLOCK_MONOTONIC_RAW, &curr_time);
  srand(curr_time.tv_nsec);
}

} // namespace janus
