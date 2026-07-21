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

#if RUSTYCPP_RUST
pub fn raft_service_server_unavailable(has_server: bool,
                                       disconnected: bool) -> bool {
    !has_server || disconnected
}

pub fn raft_service_default_vote_granted() -> bool {
    false
}

pub fn raft_service_default_acknowledged() -> bool {
    false
}

pub fn raft_service_default_append_ok() -> u64 {
    0
}

pub fn raft_service_default_term() -> u64 {
    0
}

pub fn raft_service_default_last_log_index() -> u64 {
    0
}

pub fn raft_service_memory_ack_type() -> u64 {
    0
}

pub fn raft_service_default_timeout_success() -> bool {
    false
}

pub fn raft_service_default_config_success() -> bool {
    false
}

pub fn raft_service_default_leader_hint() -> u64 {
    0
}

pub fn raft_service_should_reconnect(has_commo: bool) -> bool {
    has_commo
}

pub fn raft_service_notify_ack_from_reconnect(success: bool) -> bool {
    success
}

pub fn raft_service_poll_thread_available(found: bool,
                                          has_poll_thread: bool) -> bool {
    found && has_poll_thread
}
#endif
/*RUSTYCPP:GEN-BEGIN id=service.1 version=1 rust_sha256=692b4d993ef0f2302bebbf21d9e2e1b70617a8bac00c6b517d5ae2f8418ff3dd*/
bool raft_service_server_unavailable(bool has_server, bool disconnected);
bool raft_service_default_vote_granted();
bool raft_service_default_acknowledged();
uint64_t raft_service_default_append_ok();
uint64_t raft_service_default_term();
uint64_t raft_service_default_last_log_index();
uint64_t raft_service_memory_ack_type();
bool raft_service_default_timeout_success();
bool raft_service_default_config_success();
uint64_t raft_service_default_leader_hint();
bool raft_service_should_reconnect(bool has_commo);
bool raft_service_notify_ack_from_reconnect(bool success);
bool raft_service_poll_thread_available(bool found, bool has_poll_thread);

bool raft_service_server_unavailable(bool has_server, bool disconnected) {
    return !has_server || rusty::detail::deref_if_pointer_like(disconnected);
}

bool raft_service_default_vote_granted() {
    return false;
}

bool raft_service_default_acknowledged() {
    return false;
}

uint64_t raft_service_default_append_ok() {
    return static_cast<uint64_t>(0);
}

uint64_t raft_service_default_term() {
    return static_cast<uint64_t>(0);
}

uint64_t raft_service_default_last_log_index() {
    return static_cast<uint64_t>(0);
}

uint64_t raft_service_memory_ack_type() {
    return static_cast<uint64_t>(0);
}

bool raft_service_default_timeout_success() {
    return false;
}

bool raft_service_default_config_success() {
    return false;
}

uint64_t raft_service_default_leader_hint() {
    return static_cast<uint64_t>(0);
}

bool raft_service_should_reconnect(bool has_commo) {
    return std::move(has_commo);
}

bool raft_service_notify_ack_from_reconnect(bool success) {
    return std::move(success);
}

bool raft_service_poll_thread_available(bool found, bool has_poll_thread) {
    return rusty::detail::deref_if_pointer_like(found) && rusty::detail::deref_if_pointer_like(has_poll_thread);
}
/*RUSTYCPP:GEN-END id=service.1*/

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
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  if (raft_service_server_unavailable(has_server, disconnected)) {
    resp.max_ballot = req.cur_term;
    resp.vote_granted = raft_service_default_vote_granted();
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
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  if (raft_service_server_unavailable(has_server, disconnected)) {
    resp.acknowledged = raft_service_default_acknowledged();
    return Result<RpcVoteDurableResponse, rrr::i32>::Ok(resp);
  }
  svr->OnVoteDurable(req.term, req.voter_id, &resp.acknowledged);
  return Result<RpcVoteDurableResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcAppendEntriesResponse, rrr::i32>
RaftServiceImpl::AppendEntries(const RpcAppendEntriesRequest& req) {
  RpcAppendEntriesResponse resp{};
  RaftServer* svr = GetServer();
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  if (raft_service_server_unavailable(has_server, disconnected)) {
    resp.followerAppendOK = raft_service_default_append_ok();
    resp.followerCurrentTerm = raft_service_default_term();
    resp.followerLastLogIndex = raft_service_default_last_log_index();
    resp.followerAckType = raft_service_memory_ack_type();
    return Result<RpcAppendEntriesResponse, rrr::i32>::Ok(resp);
  }
  resp.followerAckType = raft_service_memory_ack_type();
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
  Log_debug("RaftServiceImpl: EmptyAppendEntries answering leader %d", req.leaderSiteId);
  RpcEmptyAppendEntriesResponse resp{};
  RaftServer* svr = GetServer();
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  if (raft_service_server_unavailable(has_server, disconnected)) {
    resp.followerAppendOK = raft_service_default_append_ok();
    resp.followerCurrentTerm = raft_service_default_term();
    resp.followerLastLogIndex = raft_service_default_last_log_index();
    resp.followerAckType = raft_service_memory_ack_type();
    return Result<RpcEmptyAppendEntriesResponse, rrr::i32>::Ok(resp);
  }
  resp.followerAckType = raft_service_memory_ack_type();
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
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  if (raft_service_server_unavailable(has_server, disconnected)) {
    resp.acknowledged = raft_service_default_acknowledged();
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
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  if (raft_service_server_unavailable(has_server, disconnected)) {
    resp.followerTerm = raft_service_default_term();
    resp.success = raft_service_default_timeout_success();
    return Result<RpcTimeoutNowResponse, rrr::i32>::Ok(resp);
  }
  svr->OnTimeoutNow(req.leaderTerm, req.leaderSiteId,
                    &resp.followerTerm, &resp.success);
  return Result<RpcTimeoutNowResponse, rrr::i32>::Ok(resp);
}

Result<RaftService::RpcNotifyRestartResponse, rrr::i32>
RaftServiceImpl::NotifyRestart(const RpcNotifyRestartRequest& req) {
  Log_info("[NOTIFY-RESTART] Received restart notification from site %d",
           req.restartedSiteId);
  RpcNotifyRestartResponse resp{};
  RaftServer* svr = GetServer();
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  if (raft_service_server_unavailable(has_server, disconnected)) {
    resp.acknowledged = raft_service_default_acknowledged();
    return Result<RpcNotifyRestartResponse, rrr::i32>::Ok(resp);
  }
  auto commo = svr->commo();
  if (raft_service_should_reconnect(commo != nullptr)) {
    bool success = commo->ReconnectToSite(req.restartedSiteId,
                                          svr->partition_id_);
    resp.acknowledged = raft_service_notify_ack_from_reconnect(success);
    Log_info("[NOTIFY-RESTART] Reconnected to site %d: %s",
             req.restartedSiteId, success ? "success" : "failed");
  } else {
    resp.acknowledged = raft_service_default_acknowledged();
    Log_warn("[NOTIFY-RESTART] commo is null, cannot reconnect to site %d",
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
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  if (raft_service_server_unavailable(has_server, disconnected)) {
    resp.term_out = raft_service_default_term();
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
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  if (raft_service_server_unavailable(has_server, disconnected)) {
    resp.success = raft_service_default_config_success();
    resp.error_msg = "server down";
    resp.leader_hint = raft_service_default_leader_hint();
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
  bool has_server = svr != nullptr;
  bool disconnected = has_server && svr->IsDisconnected();
  if (raft_service_server_unavailable(has_server, disconnected)) {
    resp.success = raft_service_default_config_success();
    resp.error_msg = "server down";
    resp.leader_hint = raft_service_default_leader_hint();
    return Result<RpcRemoveServerResponse, rrr::i32>::Ok(resp);
  }
  svr->OnRemoveServer(req.term, req.server_id,
                      &resp.success, &resp.error_msg, &resp.leader_hint);
  return Result<RpcRemoveServerResponse, rrr::i32>::Ok(resp);
}

// =====================================================================
// Registry + lifecycle plumbing.
//
// RaftServiceImpl instances are owned by the rrr::Server after registration.
// The registry stores borrowed service pointers so the RAFT_TEST_CORO
// Kill/Restart harness can swap the borrowed RaftServer pointer without
// rebuilding the RPC service or poll thread.
// =====================================================================

std::map<siteid_t, RaftServiceImpl*> RaftServiceImpl::service_registry_;
std::mutex RaftServiceImpl::registry_mutex_;

// @unsafe - C-style cast from scheduler base to borrowed RaftServer pointer.
// The service stores the pointer atomically but does not own the server.
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
    // Publish a borrowed server pointer for future RPC handlers. nullptr is
    // intentional during Kill(); handlers then return disconnected defaults.
    it->second->svr_.store(new_svr, std::memory_order_release);
    Log_info("[RAFT-SERVICE] UpdateServer: site %d -> %p", site_id, new_svr);
  } else {
    Log_warn("[RAFT-SERVICE] UpdateServer: site %d not found in registry", site_id);
  }
}

RaftServer* RaftServiceImpl::GetServer() {
  // Borrowed pointer load paired with UpdateServer's release-store. The caller
  // must null-check before dereferencing because Kill() publishes nullptr.
  return svr_.load(std::memory_order_acquire);
}

rusty::Option<rusty::Arc<rrr::PollThread>>
RaftServiceImpl::GetPollThread(siteid_t site_id) {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  auto it = service_registry_.find(site_id);
  if (raft_service_poll_thread_available(
          it != service_registry_.end(),
          it != service_registry_.end() && it->second->poll_thread_.is_some())) {
    return rusty::Some(it->second->poll_thread_.as_ref().unwrap().clone());
  }
  return rusty::None;
}

} // namespace janus
