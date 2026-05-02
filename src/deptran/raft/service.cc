#include "service.h"
#include "server.h"
#include "raft_server_dispatcher.hpp"

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
  if (dispatcher_.is_none()) {
    resp.max_ballot = req.cur_term;
    resp.vote_granted = false;
    return Result<RpcVoteResponse, rrr::i32>::Ok(resp);
  }

  raft::VoteReq dreq{};
  dreq.last_log_idx = req.lst_log_idx;
  dreq.last_log_term = req.lst_log_term;
  dreq.candidate_site_id = req.site_id;
  dreq.current_term = req.cur_term;
  auto dresp = dispatcher_.as_ref().unwrap()->handle_vote(dreq);
  resp.max_ballot = dresp.max_ballot;
  resp.vote_granted = dresp.vote_granted;
  return Result<RpcVoteResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcVoteDurableResponse, rrr::i32>
RaftServiceImpl::VoteDurable(const RpcVoteDurableRequest& req) {
  RpcVoteDurableResponse resp{};
  if (dispatcher_.is_none()) {
    resp.acknowledged = false;
    return Result<RpcVoteDurableResponse, rrr::i32>::Ok(resp);
  }

  raft::VoteDurableReq dreq{};
  dreq.term = req.term;
  dreq.voter_id = req.voter_id;
  auto dresp = dispatcher_.as_ref().unwrap()->handle_vote_durable(dreq);
  resp.acknowledged = dresp.acknowledged;
  return Result<RpcVoteDurableResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcAppendEntriesResponse, rrr::i32>
RaftServiceImpl::AppendEntries(const RpcAppendEntriesRequest& req) {
  RpcAppendEntriesResponse resp{};
  if (dispatcher_.is_none()) {
    resp.followerAppendOK = 0;
    resp.followerCurrentTerm = 0;
    resp.followerLastLogIndex = 0;
    resp.followerAckType = 0;  // Memory
    return Result<RpcAppendEntriesResponse, rrr::i32>::Ok(resp);
  }

  raft::AppendEntriesReq dreq{};
  dreq.slot = req.slot;
  dreq.ballot = req.ballot;
  dreq.leader_current_term = req.leaderCurrentTerm;
  dreq.leader_site_id = req.leaderSiteId;
  dreq.leader_prev_log_index = req.leaderPrevLogIndex;
  dreq.leader_prev_log_term = req.leaderPrevLogTerm;
  dreq.leader_commit_index = req.leaderCommitIndex;
  dreq.cmd = req.cmd;
  dreq.leader_next_log_term = req.leaderNextLogTerm;
  auto dresp = dispatcher_.as_ref().unwrap()->handle_append_entries(dreq);
  resp.followerAppendOK = dresp.follower_append_ok;
  resp.followerCurrentTerm = dresp.follower_current_term;
  resp.followerLastLogIndex = dresp.follower_last_log_index;
  resp.followerAckType = dresp.follower_ack_type;
  return Result<RpcAppendEntriesResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcEmptyAppendEntriesResponse, rrr::i32>
RaftServiceImpl::EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req) {
  Log_debug("RaftServiceImpl: EmptyAppendEntries answering leader %d", req.leaderSiteId);
  RpcEmptyAppendEntriesResponse resp{};
  if (dispatcher_.is_none()) {
    resp.followerAppendOK = 0;
    resp.followerCurrentTerm = 0;
    resp.followerLastLogIndex = 0;
    resp.followerAckType = 0;
    return Result<RpcEmptyAppendEntriesResponse, rrr::i32>::Ok(resp);
  }

  raft::EmptyAppendEntriesReq dreq{};
  dreq.slot = req.slot;
  dreq.ballot = req.ballot;
  dreq.leader_current_term = req.leaderCurrentTerm;
  dreq.leader_site_id = req.leaderSiteId;
  dreq.leader_prev_log_index = req.leaderPrevLogIndex;
  dreq.leader_prev_log_term = req.leaderPrevLogTerm;
  dreq.leader_commit_index = req.leaderCommitIndex;
  dreq.trigger_election_now = req.trigger_election_now;
  auto dresp = dispatcher_.as_ref().unwrap()->handle_empty_append_entries(dreq);
  resp.followerAppendOK = dresp.follower_append_ok;
  resp.followerCurrentTerm = dresp.follower_current_term;
  resp.followerLastLogIndex = dresp.follower_last_log_index;
  resp.followerAckType = dresp.follower_ack_type;
  return Result<RpcEmptyAppendEntriesResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcAppendEntriesDurableResponse, rrr::i32>
RaftServiceImpl::AppendEntriesDurable(const RpcAppendEntriesDurableRequest& req) {
  RpcAppendEntriesDurableResponse resp{};
  if (dispatcher_.is_none()) {
    resp.acknowledged = false;
    return Result<RpcAppendEntriesDurableResponse, rrr::i32>::Ok(resp);
  }

  raft::AppendEntriesDurableReq dreq{};
  dreq.term = req.term;
  dreq.follower_id = req.follower_id;
  dreq.last_log_index = req.lastLogIndex;
  auto dresp = dispatcher_.as_ref().unwrap()->handle_append_entries_durable(dreq);
  resp.acknowledged = dresp.acknowledged;
  return Result<RpcAppendEntriesDurableResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcTimeoutNowResponse, rrr::i32>
RaftServiceImpl::TimeoutNow(const RpcTimeoutNowRequest& req) {
  RpcTimeoutNowResponse resp{};
  if (dispatcher_.is_none()) {
    resp.followerTerm = 0;
    resp.success = false;
    return Result<RpcTimeoutNowResponse, rrr::i32>::Ok(resp);
  }

  raft::TimeoutNowReq dreq{};
  dreq.leader_term = req.leaderTerm;
  dreq.leader_site_id = req.leaderSiteId;
  auto dresp = dispatcher_.as_ref().unwrap()->handle_timeout_now(dreq);
  resp.followerTerm = dresp.follower_term;
  resp.success = dresp.success;
  return Result<RpcTimeoutNowResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcNotifyRestartResponse, rrr::i32>
RaftServiceImpl::NotifyRestart(const RpcNotifyRestartRequest& req) {
  Log_info("[NOTIFY-RESTART] Received restart notification from site %d",
           req.restartedSiteId);
  RpcNotifyRestartResponse resp{};
  if (dispatcher_.is_none()) {
    resp.acknowledged = false;
    return Result<RpcNotifyRestartResponse, rrr::i32>::Ok(resp);
  }

  raft::NotifyRestartReq dreq{};
  dreq.restarted_site_id = req.restartedSiteId;
  auto dresp = dispatcher_.as_ref().unwrap()->handle_notify_restart(dreq);
  resp.acknowledged = dresp.acknowledged;
  Log_info("[NOTIFY-RESTART] Reconnected to site %d: %s",
           req.restartedSiteId, dresp.acknowledged ? "success" : "failed");
  return Result<RpcNotifyRestartResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcInstallSnapshotResponse, rrr::i32>
RaftServiceImpl::InstallSnapshot(const RpcInstallSnapshotRequest& req) {
  RpcInstallSnapshotResponse resp{};
  if (dispatcher_.is_none()) {
    resp.term_out = 0;
    return Result<RpcInstallSnapshotResponse, rrr::i32>::Ok(resp);
  }

  raft::InstallSnapshotReq dreq{};
  dreq.term = req.term;
  dreq.leader_id = req.leader_id;
  dreq.last_included_index = req.last_included_index;
  dreq.last_included_term = req.last_included_term;
  dreq.data = req.data;
  auto dresp = dispatcher_.as_ref().unwrap()->handle_install_snapshot(dreq);
  resp.term_out = dresp.term_out;
  return Result<RpcInstallSnapshotResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcAddServerResponse, rrr::i32>
RaftServiceImpl::AddServer(const RpcAddServerRequest& req) {
  RpcAddServerResponse resp{};
  if (dispatcher_.is_none()) {
    resp.success = false;
    resp.error_msg = "server down";
    resp.leader_hint = 0;
    return Result<RpcAddServerResponse, rrr::i32>::Ok(resp);
  }

  raft::AddServerReq dreq{};
  dreq.term = req.term;
  dreq.new_server_id = req.new_server_id;
  dreq.new_server_addr = req.new_server_addr;
  auto dresp = dispatcher_.as_ref().unwrap()->handle_add_server(dreq);
  resp.success = dresp.success;
  resp.error_msg = dresp.error_msg;
  resp.leader_hint = dresp.leader_hint;
  return Result<RpcAddServerResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcRemoveServerResponse, rrr::i32>
RaftServiceImpl::RemoveServer(const RpcRemoveServerRequest& req) {
  RpcRemoveServerResponse resp{};
  if (dispatcher_.is_none()) {
    resp.success = false;
    resp.error_msg = "server down";
    resp.leader_hint = 0;
    return Result<RpcRemoveServerResponse, rrr::i32>::Ok(resp);
  }

  raft::RemoveServerReq dreq{};
  dreq.term = req.term;
  dreq.server_id = req.server_id;
  auto dresp = dispatcher_.as_ref().unwrap()->handle_remove_server(dreq);
  resp.success = dresp.success;
  resp.error_msg = dresp.error_msg;
  resp.leader_hint = dresp.leader_hint;
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
  if (svr != nullptr) {
    dispatcher_ = rusty::Some(raft::make_raft_server_dispatcher(svr));
  } else {
    dispatcher_ = rusty::None;
  }
  site_id_ = (svr != nullptr) ? svr->site_id_ : 0;
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
    if (new_svr != nullptr) {
      it->second->dispatcher_ = rusty::Some(raft::make_raft_server_dispatcher(new_svr));
    } else {
      it->second->dispatcher_ = rusty::None;
    }
    Log_info("[RAFT-SERVICE] UpdateServer: site %d -> %p", site_id, new_svr);
  } else {
    Log_warn("[RAFT-SERVICE] UpdateServer: site %d not found in registry", site_id);
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
