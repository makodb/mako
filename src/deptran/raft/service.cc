
#include "../../rrr/misc/marshal.hpp"
#include "service.h"
#include "server.h"

// @external: {
//   Log_info: [safe, (...) -> void]
//   Log_debug: [safe, (...) -> void]
//   verify: [safe, (...) -> void]
//   clock_gettime: [safe, (...) -> int]
//   srand: [safe, (...) -> void]
//   rrr::Fiber::create_run: [safe, (...) -> owned]
// }

namespace janus {
void RaftServiceImpl::Vote(const RaftService::RpcVoteRequest& req,
                           RaftService::RpcVoteResponse& resp,
                           rrr::DeferredReply defer) {
  this->Vote(req.lst_log_idx,
             req.lst_log_term,
             req.site_id,
             req.cur_term,
             &resp.max_ballot,
             &resp.vote_granted,
             std::move(defer));
}

void RaftServiceImpl::VoteDurable(const RaftService::RpcVoteDurableRequest& req,
                                  RaftService::RpcVoteDurableResponse& resp,
                                  rrr::DeferredReply defer) {
  this->VoteDurable(req.term, req.voter_id, &resp.acknowledged, std::move(defer));
}

void RaftServiceImpl::AppendEntries(const RaftService::RpcAppendEntriesRequest& req,
                                    RaftService::RpcAppendEntriesResponse& resp,
                                    rrr::DeferredReply defer) {
  this->AppendEntries(req.slot,
                      req.ballot,
                      req.leaderCurrentTerm,
                      req.leaderSiteId,
                      req.leaderPrevLogIndex,
                      req.leaderPrevLogTerm,
                      req.leaderCommitIndex,
                      req.cmd,
                      req.leaderNextLogTerm,
                      &resp.followerAppendOK,
                      &resp.followerCurrentTerm,
                      &resp.followerLastLogIndex,
                      &resp.followerAckType,
                      std::move(defer));
}

void RaftServiceImpl::EmptyAppendEntries(const RaftService::RpcEmptyAppendEntriesRequest& req,
                                         RaftService::RpcEmptyAppendEntriesResponse& resp,
                                         rrr::DeferredReply defer) {
  this->EmptyAppendEntries(req.slot,
                           req.ballot,
                           req.leaderCurrentTerm,
                           req.leaderSiteId,
                           req.leaderPrevLogIndex,
                           req.leaderPrevLogTerm,
                           req.leaderCommitIndex,
                           req.trigger_election_now,
                           &resp.followerAppendOK,
                           &resp.followerCurrentTerm,
                           &resp.followerLastLogIndex,
                           &resp.followerAckType,
                           std::move(defer));
}

void RaftServiceImpl::AppendEntriesDurable(
    const RaftService::RpcAppendEntriesDurableRequest& req,
    RaftService::RpcAppendEntriesDurableResponse& resp,
    rrr::DeferredReply defer) {
  this->AppendEntriesDurable(req.term,
                             req.follower_id,
                             req.lastLogIndex,
                             &resp.acknowledged,
                             std::move(defer));
}

void RaftServiceImpl::TimeoutNow(const RaftService::RpcTimeoutNowRequest& req,
                                 RaftService::RpcTimeoutNowResponse& resp,
                                 rrr::DeferredReply defer) {
  this->TimeoutNow(req.leaderTerm,
                   req.leaderSiteId,
                   &resp.followerTerm,
                   &resp.success,
                   std::move(defer));
}

void RaftServiceImpl::NotifyRestart(const RaftService::RpcNotifyRestartRequest& req,
                                    RaftService::RpcNotifyRestartResponse& resp,
                                    rrr::DeferredReply defer) {
  this->NotifyRestart(req.restartedSiteId, &resp.acknowledged, std::move(defer));
}

void RaftServiceImpl::InstallSnapshot(const RaftService::RpcInstallSnapshotRequest& req,
                                      RaftService::RpcInstallSnapshotResponse& resp,
                                      rrr::DeferredReply defer) {
  this->InstallSnapshot(req.term,
                        req.leader_id,
                        req.last_included_index,
                        req.last_included_term,
                        req.data,
                        &resp.term_out,
                        std::move(defer));
}

// @safe - svr_ pointer is bounded, delegates to RaftServer::OnInstallSnapshot
void RaftServiceImpl::HandleInstallSnapshot(const uint64_t& term,
                                             const uint64_t& leader_id,
                                             const uint64_t& last_included_index,
                                             const uint64_t& last_included_term,
                                             const std::string& data,
                                             uint64_t* term_out,
                                             rrr::DeferredReply defer) {
  RaftServer* svr = GetServer();
  if (svr == nullptr) {
    // Server is killed, return failure
    *term_out = 0;
    defer.reply();
    return;
  }
  svr->OnInstallSnapshot(term, leader_id, last_included_index,
                         last_included_term, data, term_out,
                         [defer = std::move(defer)]() mutable { defer.reply(); });
}

// Static member definitions for service registry
std::map<siteid_t, RaftServiceImpl*> RaftServiceImpl::service_registry_;
std::mutex RaftServiceImpl::registry_mutex_;

// @safe - C-style cast in @unsafe block, clock_gettime/srand marked @external [safe]
RaftServiceImpl::RaftServiceImpl(TxLogServer *sched, rusty::Arc<rrr::PollThread> poll_thread)
    : poll_thread_(rusty::Some(std::move(poll_thread))) {
  // @unsafe
  RaftServer* svr = (RaftServer*)sched;
  svr_.store(svr, std::memory_order_release);
  site_id_ = svr->site_id_;

  // Register this service instance in the static registry
  {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    service_registry_[site_id_] = this;
  }

  struct timespec curr_time;
  clock_gettime(CLOCK_MONOTONIC_RAW, &curr_time);
  srand(curr_time.tv_nsec);
}

// Called by test framework during Kill/Restart to update server pointer
void RaftServiceImpl::UpdateServer(siteid_t site_id, RaftServer* new_svr) {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  auto it = service_registry_.find(site_id);
  if (it != service_registry_.end()) {
    it->second->svr_.store(new_svr, std::memory_order_release);
    Log_info("[RAFT-SERVICE] UpdateServer: site %d -> %p", site_id, new_svr);
  } else {
    Log_warn("[RAFT-SERVICE] UpdateServer: site %d not found in registry", site_id);
  }
}

// Called by RPC handlers - lock-free atomic read
RaftServer* RaftServiceImpl::GetServer() {
  return svr_.load(std::memory_order_acquire);
}

// Called by test framework during Restart to get the original poll thread
// Returns a clone of the Arc so the original is preserved
rusty::Option<rusty::Arc<rrr::PollThread>> RaftServiceImpl::GetPollThread(siteid_t site_id) {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  auto it = service_registry_.find(site_id);
  if (it != service_registry_.end() && it->second->poll_thread_.is_some()) {
    // Clone the Arc to share ownership
    return rusty::Some(it->second->poll_thread_.as_ref().unwrap().clone());
  }
  return rusty::None;
}

// @safe - svr_ pointer is bounded (set in constructor), external calls marked @external
void RaftServiceImpl::HandleVote(const uint64_t& lst_log_idx,
                                    const ballot_t& lst_log_term,
                                    const siteid_t& can_id,
                                    const ballot_t& can_term,
                                    ballot_t* reply_term,
                                    bool_t *vote_granted,
                                    rrr::DeferredReply defer) {
  RaftServer* svr = GetServer();
  if (svr == nullptr) {
    // Server is killed, return failure
    *reply_term = 0;
    *vote_granted = false;
    defer.reply();
    return;
  }
  svr->OnRequestVote(lst_log_idx, lst_log_term, can_id, can_term,
                    reply_term, vote_granted,
                    [defer = std::move(defer)]() mutable { defer.reply(); });
}

// @safe - Handle VoteDurable RPC for speculative voting
// Received when a follower has durably persisted its vote to disk
void RaftServiceImpl::HandleVoteDurable(const ballot_t& term,
                                         const siteid_t& voter_id,
                                         bool_t* acknowledged,
                                         rrr::DeferredReply defer) {
  RaftServer* svr = GetServer();
  if (svr == nullptr) {
    // Server is killed, return failure
    *acknowledged = false;
    defer.reply();
    return;
  }
  svr->OnVoteDurable(term, voter_id, acknowledged,
                     [defer = std::move(defer)]() mutable { defer.reply(); });
}

// @safe - svr_ pointer is bounded, Fiber::create_run marked @external [safe]
void RaftServiceImpl::HandleAppendEntries(const uint64_t& slot,
                                        const ballot_t& ballot,
                                        const uint64_t& leaderCurrentTerm,
                                        const siteid_t& leaderSiteId,
                                        const uint64_t& leaderPrevLogIndex,
                                        const uint64_t& leaderPrevLogTerm,
                                        const uint64_t& leaderCommitIndex,
                                        const MarshallDeputy& md_cmd,
                                        const uint64_t& leaderNextLogTerm,
                                        uint64_t *followerAppendOK,
                                        uint64_t *followerCurrentTerm,
                                        uint64_t *followerLastLogIndex,
                                        uint64_t *followerAckType,
                                        rrr::DeferredReply defer) {
  RaftServer* svr = GetServer();
  if (svr == nullptr) {
    // Server is killed, return failure
    *followerAppendOK = 0;
    *followerCurrentTerm = 0;
    *followerLastLogIndex = 0;
    *followerAckType = 0;  // Memory
    defer.reply();
    return;
  }

  // Set ackType to Memory - this response is sent before fsync
  *followerAckType = 0;  // Memory

  Fiber::create_run([=, defer = std::move(defer)]() mutable {
    svr->OnAppendEntries(slot,
                            ballot,
                            leaderCurrentTerm,
                            leaderSiteId,
                            leaderPrevLogIndex,
                            leaderPrevLogTerm,
                            leaderCommitIndex,
                            const_cast<MarshallDeputy&>(md_cmd).inner(),
                            leaderNextLogTerm,
                            followerAppendOK,
                            followerCurrentTerm,
                            followerLastLogIndex,
                            [defer = std::move(defer)]() mutable { defer.reply(); });
  });
}

// @safe - svr_ pointer is bounded, Fiber::create_run marked @external [safe]
void RaftServiceImpl::HandleEmptyAppendEntries(const uint64_t& slot,
                                             const ballot_t& ballot,
                                             const uint64_t& leaderCurrentTerm,
                                             const siteid_t& leaderSiteId,
                                             const uint64_t& leaderPrevLogIndex,
                                             const uint64_t& leaderPrevLogTerm,
                                             const uint64_t& leaderCommitIndex,
                                             const bool_t& trigger_election_now,
                                             uint64_t *followerAppendOK,
                                             uint64_t *followerCurrentTerm,
                                             uint64_t *followerLastLogIndex,
                                             uint64_t *followerAckType,
                                             rrr::DeferredReply defer) {
  Log_debug("RaftServiceImpl: HandleEmptyAppendEntries answering leader %d", leaderSiteId);
  RaftServer* svr = GetServer();
  if (svr == nullptr) {
    // Server is killed, return failure
    *followerAppendOK = 0;
    *followerCurrentTerm = 0;
    *followerLastLogIndex = 0;
    *followerAckType = 0;  // Memory
    defer.reply();
    return;
  }

  // Set ackType to Memory - this response is sent before fsync
  *followerAckType = 0;  // Memory

  std::shared_ptr<Marshallable> cmd = nullptr;
  Fiber::create_run([=, defer = std::move(defer)]() mutable {
    svr->OnAppendEntries(slot,
                            ballot,
                            leaderCurrentTerm,
                            leaderSiteId,
                            leaderPrevLogIndex,
                            leaderPrevLogTerm,
                            leaderCommitIndex,
                            cmd,
                            0,
                            followerAppendOK,
                            followerCurrentTerm,
                            followerLastLogIndex,
                            [defer = std::move(defer)]() mutable { defer.reply(); },
                            trigger_election_now);
  });
}

// @safe - Handle AppendEntriesDurable RPC for speculative commits
// Received when a follower has durably persisted log entries to disk
void RaftServiceImpl::HandleAppendEntriesDurable(const ballot_t& term,
                                                  const siteid_t& follower_id,
                                                  const uint64_t& lastLogIndex,
                                                  bool_t* acknowledged,
                                                  rrr::DeferredReply defer) {
  RaftServer* svr = GetServer();
  if (svr == nullptr) {
    // Server is killed, return failure
    *acknowledged = false;
    defer.reply();
    return;
  }
  svr->OnAppendEntriesDurable(term, follower_id, lastLogIndex, acknowledged,
                              [defer = std::move(defer)]() mutable { defer.reply(); });
}

// @safe - svr_ pointer is bounded, external calls marked @external [safe]
void RaftServiceImpl::HandleTimeoutNow(const uint64_t& leaderTerm,
                                        const siteid_t& leaderSiteId,
                                        uint64_t* followerTerm,
                                        bool_t* success,
                                        rrr::DeferredReply defer) {
  RaftServer* svr = GetServer();
  if (svr == nullptr) {
    // Server is killed, return failure
    *followerTerm = 0;
    *success = false;
    defer.reply();
    return;
  }
  svr->OnTimeoutNow(leaderTerm, leaderSiteId, followerTerm, success,
                     [defer = std::move(defer)]() mutable { defer.reply(); });
}

// @safe
void RaftServiceImpl::HandleNotifyRestart(const siteid_t& restartedSiteId,
                                          bool_t* acknowledged,
                                          rrr::DeferredReply defer) {
  Log_info("[NOTIFY-RESTART] Received restart notification from site %d", restartedSiteId);

  RaftServer* svr = GetServer();
  if (svr == nullptr) {
    // Server is killed, return failure
    *acknowledged = false;
    defer.reply();
    return;
  }

  // Reconnect our client connection to the restarted site
  auto commo = svr->commo();
  if (commo != nullptr) {
    bool success = commo->ReconnectToSite(restartedSiteId, svr->partition_id_);
    *acknowledged = success;
    Log_info("[NOTIFY-RESTART] Reconnected to site %d: %s", restartedSiteId, success ? "success" : "failed");
  } else {
    *acknowledged = false;
    Log_warn("[NOTIFY-RESTART] commo is null, cannot reconnect to site %d", restartedSiteId);
  }

  // ==================================================================
  // SPECULATIVE REPLICATION: Invalidate speculative state for peer
  // ==================================================================
  // When a peer restarts, it loses its in-memory state (memory vote,
  // memory-acked entries). The leader must update its speculative state.
  svr->OnPeerRestart(restartedSiteId);

  defer.reply();
}

// @safe - Typed RPC dispatcher for AddServer
void RaftServiceImpl::AddServer(const RaftService::RpcAddServerRequest& req,
                                RaftService::RpcAddServerResponse& resp,
                                rrr::DeferredReply defer) {
  this->AddServer(req.term,
                  req.new_server_id,
                  req.new_server_addr,
                  &resp.success,
                  &resp.error_msg,
                  &resp.leader_hint,
                  std::move(defer));
}

// @safe - Typed RPC dispatcher for RemoveServer
void RaftServiceImpl::RemoveServer(const RaftService::RpcRemoveServerRequest& req,
                                   RaftService::RpcRemoveServerResponse& resp,
                                   rrr::DeferredReply defer) {
  this->RemoveServer(req.term,
                     req.server_id,
                     &resp.success,
                     &resp.error_msg,
                     &resp.leader_hint,
                     std::move(defer));
}

// @safe - svr_ pointer is bounded, delegates to RaftServer::OnAddServer
void RaftServiceImpl::HandleAddServer(const uint64_t& term,
                                       const uint64_t& new_server_id,
                                       const std::string& new_server_addr,
                                       bool_t* success,
                                       std::string* error_msg,
                                       uint64_t* leader_hint,
                                       rrr::DeferredReply defer) {
  RaftServer* svr = GetServer();
  if (svr == nullptr) {
    *success = false;
    *error_msg = "server down";
    *leader_hint = 0;
    defer.reply();
    return;
  }
  svr->OnAddServer(term, new_server_id, new_server_addr,
                   success, error_msg, leader_hint,
                   std::move(defer));
}

// @safe - svr_ pointer is bounded, delegates to RaftServer::OnRemoveServer
void RaftServiceImpl::HandleRemoveServer(const uint64_t& term,
                                          const uint64_t& server_id,
                                          bool_t* success,
                                          std::string* error_msg,
                                          uint64_t* leader_hint,
                                          rrr::DeferredReply defer) {
  RaftServer* svr = GetServer();
  if (svr == nullptr) {
    *success = false;
    *error_msg = "server down";
    *leader_hint = 0;
    defer.reply();
    return;
  }
  svr->OnRemoveServer(term, server_id,
                      success, error_msg, leader_hint,
                      std::move(defer));
}

} // namespace janus;
