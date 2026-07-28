#include "service.h"
#include "server.h"

#include "rrr/rrr.hpp"

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
// Each method here is invoked by the rrr-generated wrapper on a fresh
// Fiber (see src/rrr/pylib/simplerpcgen/lang_cpp.py). We do synchronous
// work — including any PersistState fsync from RaftServer — and return
// the response struct by value. The framework marshals and sends the
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
  RaftServer* svr = GetServer();
  if (svr == nullptr || svr->IsDisconnected()) {
    resp.max_ballot = req.cur_term;
    resp.vote_granted = false;
    return Result<RpcVoteResponse, rrr::i32>::Ok(resp);
  }
  svr->OnRequestVote(req.lst_log_idx, req.lst_log_term,
                     req.site_id, req.cur_term,
                     &resp.max_ballot, &resp.vote_granted);
  return Result<RpcVoteResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcVoteDurableResponse, rrr::i32>
RaftServiceImpl::VoteDurable(const RpcVoteDurableRequest& req) {
  RpcVoteDurableResponse resp{};
  RaftServer* svr = GetServer();
  if (svr == nullptr || svr->IsDisconnected()) {
    resp.acknowledged = false;
    return Result<RpcVoteDurableResponse, rrr::i32>::Ok(resp);
  }
  svr->OnVoteDurable(req.term, req.voter_id, &resp.acknowledged);
  return Result<RpcVoteDurableResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcAppendEntriesResponse, rrr::i32>
RaftServiceImpl::AppendEntries(const RpcAppendEntriesRequest& req) {
  RpcAppendEntriesResponse resp{};
  RaftServer* svr = GetServer();
  if (svr == nullptr || svr->IsDisconnected()) {
    resp.followerAppendOK = 0;
    resp.followerCurrentTerm = 0;
    resp.followerLastLogIndex = 0;
    resp.followerAckType = 0;  // Memory
    return Result<RpcAppendEntriesResponse, rrr::i32>::Ok(resp);
  }
  resp.followerAckType = 0;  // Memory — response precedes fsync
  svr->OnAppendEntries(req.slot, req.ballot, req.leaderCurrentTerm,
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
  RaftServer* svr = GetServer();
  if (svr == nullptr || svr->IsDisconnected()) {
    resp.followerAppendOK = 0;
    resp.followerCurrentTerm = 0;
    resp.followerLastLogIndex = 0;
    resp.followerAckType = 0;
    return Result<RpcEmptyAppendEntriesResponse, rrr::i32>::Ok(resp);
  }
  resp.followerAckType = 0;
  // OnAppendEntries uses the same fields as the non-empty variant with
  // an empty cmd and leaderNextLogTerm == 0 (heartbeat path).
  // followerAppendOK/Term/LastLogIndex are shared layout with the non-empty
  // response, so we can pass pointers directly into our resp struct.
  svr->OnAppendEntries(req.slot, req.ballot, req.leaderCurrentTerm,
                       req.leaderSiteId, req.leaderPrevLogIndex,
                       req.leaderPrevLogTerm, req.leaderCommitIndex,
                       janus::Command{}, 0,
                       &resp.followerAppendOK, &resp.followerCurrentTerm,
                       &resp.followerLastLogIndex,
                       req.trigger_election_now);
  return Result<RpcEmptyAppendEntriesResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcAppendEntriesDurableResponse, rrr::i32>
RaftServiceImpl::AppendEntriesDurable(const RpcAppendEntriesDurableRequest& req) {
  RpcAppendEntriesDurableResponse resp{};
  RaftServer* svr = GetServer();
  if (svr == nullptr || svr->IsDisconnected()) {
    resp.acknowledged = false;
    return Result<RpcAppendEntriesDurableResponse, rrr::i32>::Ok(resp);
  }
  svr->OnAppendEntriesDurable(req.term, req.follower_id,
                              req.lastLogIndex, &resp.acknowledged);
  return Result<RpcAppendEntriesDurableResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcTimeoutNowResponse, rrr::i32>
RaftServiceImpl::TimeoutNow(const RpcTimeoutNowRequest& req) {
  RpcTimeoutNowResponse resp{};
  RaftServer* svr = GetServer();
  if (svr == nullptr || svr->IsDisconnected()) {
    resp.followerTerm = 0;
    resp.success = false;
    return Result<RpcTimeoutNowResponse, rrr::i32>::Ok(resp);
  }
  svr->OnTimeoutNow(req.leaderTerm, req.leaderSiteId,
                    &resp.followerTerm, &resp.success);
  return Result<RpcTimeoutNowResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcNotifyRestartResponse, rrr::i32>
RaftServiceImpl::NotifyRestart(const RpcNotifyRestartRequest& req) {
  Log_info("[NOTIFY-RESTART] Received restart notification from site {}",
           req.restartedSiteId);
  RpcNotifyRestartResponse resp{};
  RaftServer* svr = GetServer();
  if (svr == nullptr || svr->IsDisconnected()) {
    resp.acknowledged = false;
    return Result<RpcNotifyRestartResponse, rrr::i32>::Ok(resp);
  }
  auto commo = svr->commo();
  if (commo != nullptr) {
    bool success = commo->ReconnectToSite(req.restartedSiteId,
                                          svr->partition_id_);
    resp.acknowledged = success;
    Log_info("[NOTIFY-RESTART] Reconnected to site {}: {}",
             req.restartedSiteId, success ? "success" : "failed");
  } else {
    resp.acknowledged = false;
    Log_warn("[NOTIFY-RESTART] commo is null, cannot reconnect to site {}",
             req.restartedSiteId);
  }
  // Invalidate speculative state for the peer that just restarted.
  svr->OnPeerRestart(req.restartedSiteId);
  return Result<RpcNotifyRestartResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcInstallSnapshotResponse, rrr::i32>
RaftServiceImpl::InstallSnapshot(const RpcInstallSnapshotRequest& req) {
  RpcInstallSnapshotResponse resp{};
  RaftServer* svr = GetServer();
  if (svr == nullptr || svr->IsDisconnected()) {
    resp.term_out = 0;
    return Result<RpcInstallSnapshotResponse, rrr::i32>::Ok(resp);
  }
  svr->OnInstallSnapshot(req.term, req.leader_id,
                         req.last_included_index, req.last_included_term,
                         req.data, &resp.term_out);
  return Result<RpcInstallSnapshotResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcAddServerResponse, rrr::i32>
RaftServiceImpl::AddServer(const RpcAddServerRequest& req) {
  RpcAddServerResponse resp{};
  RaftServer* svr = GetServer();
  if (svr == nullptr || svr->IsDisconnected()) {
    resp.success = false;
    resp.error_msg = "server down";
    resp.leader_hint = 0;
    return Result<RpcAddServerResponse, rrr::i32>::Ok(resp);
  }
  svr->OnAddServer(req.term, req.new_server_id, req.new_server_addr,
                   &resp.success, &resp.error_msg, &resp.leader_hint);
  return Result<RpcAddServerResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcRemoveServerResponse, rrr::i32>
RaftServiceImpl::RemoveServer(const RpcRemoveServerRequest& req) {
  RpcRemoveServerResponse resp{};
  RaftServer* svr = GetServer();
  if (svr == nullptr || svr->IsDisconnected()) {
    resp.success = false;
    resp.error_msg = "server down";
    resp.leader_hint = 0;
    return Result<RpcRemoveServerResponse, rrr::i32>::Ok(resp);
  }
  svr->OnRemoveServer(req.term, req.server_id,
                      &resp.success, &resp.error_msg, &resp.leader_hint);
  return Result<RpcRemoveServerResponse, rrr::i32>::Ok(resp);
}

// =====================================================================
// Registry + lifecycle plumbing (unchanged from prior commit)
// =====================================================================

std::map<siteid_t, RaftServiceImpl*> RaftServiceImpl::service_registry_;
std::mutex RaftServiceImpl::registry_mutex_;

// @unsafe - C-style cast in @unsafe block
RaftServiceImpl::RaftServiceImpl(TxLogServer *sched, rusty::Arc<rrr::PollThread> poll_thread)
    : poll_thread_(rusty::Some(std::move(poll_thread))) {
  // @unsafe
  RaftServer* svr = (RaftServer*)sched;
  svr_.store(svr, std::memory_order_release);
  site_id_ = svr->site_id_;
  {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    service_registry_[site_id_] = this;
  }
  struct timespec curr_time;
  clock_gettime(CLOCK_MONOTONIC_RAW, &curr_time);
  srand(curr_time.tv_nsec);
}

void RaftServiceImpl::UpdateServer(siteid_t site_id, RaftServer* new_svr) {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  auto it = service_registry_.find(site_id);
  if (it != service_registry_.end()) {
    it->second->svr_.store(new_svr, std::memory_order_release);
    Log_info("[RAFT-SERVICE] UpdateServer: site {} -> {}", site_id, (void*)new_svr);
  } else {
    Log_warn("[RAFT-SERVICE] UpdateServer: site {} not found in registry", site_id);
  }
}

RaftServer* RaftServiceImpl::GetServer() {
  return svr_.load(std::memory_order_acquire);
}

rusty::Option<rusty::Arc<rrr::PollThread>>
RaftServiceImpl::GetPollThread(siteid_t site_id) {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  auto it = service_registry_.find(site_id);
  if (it != service_registry_.end() && it->second->poll_thread_.is_some()) {
    return rusty::Some(it->second->poll_thread_.as_ref().unwrap().clone());
  }
  return rusty::None;
}

} // namespace janus
