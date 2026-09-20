#include "service.h"
#include "server.h"

#include "rrr/rrr.hpp"
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

// Pure RPC availability decisions over copied booleans. Each call site still
// computes `disconnected` behind a `has_server && ...` guard so the generated
// helper cannot broaden raw-pointer evaluation.
#if RUSTYCPP_RUST
pub const fn raft_service_server_unavailable(has_server: bool,
                                             disconnected: bool,
                                             rpc_ready: bool) -> bool {
    !has_server || disconnected || !rpc_ready
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_service.scalar_decisions version=1 rust_sha256=00d3fccb2b5ef9f153e09d839b3deeffd873ae4a7787681cb84c7d3b775e4505*/
constexpr bool raft_service_server_unavailable(bool has_server, bool disconnected, bool rpc_ready);
constexpr bool raft_service_server_unavailable(bool has_server, bool disconnected, bool rpc_ready) {
    return (!has_server || rusty::detail::deref_if_pointer_like(disconnected)) || !rpc_ready;
}
/*RUSTYCPP:GEN-END id=raft_service.scalar_decisions*/

static_assert(raft_service_server_unavailable(false, false, false));
static_assert(raft_service_server_unavailable(true, true, true));
static_assert(raft_service_server_unavailable(true, false, false));
static_assert(!raft_service_server_unavailable(true, false, true));

// =====================================================================
// Fiber-RPC handlers.
//
// Each method here is invoked by the rrr-generated wrapper on a fresh
// Fiber (see src/rrr/pylib/simplerpcgen/lang_cpp.py). We do synchronous
// work and return the response struct by value. The framework marshals and sends the
// reply when the fiber completes; no DeferredReply anywhere.
//
// Disconnected/killed server path: fill the response with the same
// defaults the old RpcHandler macro's OnDisconnected##name bodies used,
// and return Ok(resp). We deliberately do NOT return Err(...): the
// peer code treats nonzero error codes as "drop this reply", which
// would hide the disconnected-server signal that other code paths
// depend on (e.g., lost-RPC detection in SendAppendEntries).
// =====================================================================

Result<RaftService::RpcVoteResponse, rrr::i32>
RaftServiceImpl::Vote(const RpcVoteRequest& req) {
  RpcVoteResponse resp{};
  RaftServer* svr = svr_;
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  bool rpc_ready = has_server && svr->IsRpcReady();
  if (raft_service_server_unavailable(
          has_server, disconnected, rpc_ready)) {
    resp.max_ballot = req.cur_term;
    resp.vote_granted = false;
    return Result<RpcVoteResponse, rrr::i32>::Ok(resp);
  }
  svr->OnRequestVote(req.lst_log_idx, req.lst_log_term,
                     req.site_id, req.cur_term,
                     &resp.max_ballot, &resp.vote_granted);
  return Result<RpcVoteResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcAppendEntriesResponse, rrr::i32>
RaftServiceImpl::AppendEntries(const RpcAppendEntriesRequest& req) {
  RpcAppendEntriesResponse resp{};
  RaftServer* svr = svr_;
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  bool rpc_ready = has_server && svr->IsRpcReady();
  if (raft_service_server_unavailable(
          has_server, disconnected, rpc_ready)) {
    resp.followerAppendOK = 0;
    resp.followerCurrentTerm = 0;
    resp.followerLastLogIndex = 0;
    return Result<RpcAppendEntriesResponse, rrr::i32>::Ok(resp);
  }
  svr->OnAppendEntries(req.leaderCurrentTerm,
                       req.leaderSiteId, req.leaderPrevLogIndex,
                       req.leaderPrevLogTerm, req.leaderCommitIndex,
                       req.cmd, req.leaderNextLogTerm,
                       &resp.followerAppendOK, &resp.followerCurrentTerm,
                       &resp.followerLastLogIndex);
  return Result<RpcAppendEntriesResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcEmptyAppendEntriesResponse, rrr::i32>
RaftServiceImpl::EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req) {
  Log_debug("RaftServiceImpl: EmptyAppendEntries answering leader {}", req.leaderSiteId);
  RpcEmptyAppendEntriesResponse resp{};
  RaftServer* svr = svr_;
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  bool rpc_ready = has_server && svr->IsRpcReady();
  if (raft_service_server_unavailable(
          has_server, disconnected, rpc_ready)) {
    resp.followerAppendOK = 0;
    resp.followerCurrentTerm = 0;
    resp.followerLastLogIndex = 0;
    return Result<RpcEmptyAppendEntriesResponse, rrr::i32>::Ok(resp);
  }
  // OnAppendEntries uses the same fields as the non-empty variant with
  // an empty cmd and leaderNextLogTerm == 0 (heartbeat path).
  // followerAppendOK/Term/LastLogIndex are shared layout with the non-empty
  // response, so we can pass pointers directly into our resp struct.
  svr->OnAppendEntries(req.leaderCurrentTerm,
                       req.leaderSiteId, req.leaderPrevLogIndex,
                       req.leaderPrevLogTerm, req.leaderCommitIndex,
                       janus::Command{}, 0,
                       &resp.followerAppendOK, &resp.followerCurrentTerm,
                       &resp.followerLastLogIndex);
  return Result<RpcEmptyAppendEntriesResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcInstallSnapshotResponse, rrr::i32>
RaftServiceImpl::InstallSnapshot(const RpcInstallSnapshotRequest& req) {
  RpcInstallSnapshotResponse resp{};
  RaftServer* svr = svr_;
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  bool rpc_ready = has_server && svr->IsRpcReady();
  if (raft_service_server_unavailable(
          has_server, disconnected, rpc_ready)) {
    resp.term_out = 0;
    return Result<RpcInstallSnapshotResponse, rrr::i32>::Ok(resp);
  }
  svr->OnInstallSnapshot(req.term, req.leader_id,
                         req.last_included_index, req.last_included_term,
                         req.data, &resp.term_out);
  return Result<RpcInstallSnapshotResponse, rrr::i32>::Ok(resp);
}

// @unsafe - Stores the raw Raft server pointer for the handlers above.
RaftServiceImpl::RaftServiceImpl(RaftServer* sched)
    : svr_(sched) {
  struct timespec curr_time;
  clock_gettime(CLOCK_MONOTONIC_RAW, &curr_time);
  srand(curr_time.tv_nsec);
}

} // namespace janus
