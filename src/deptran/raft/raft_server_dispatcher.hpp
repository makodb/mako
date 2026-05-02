#pragma once

#include "dispatcher.hpp"
#include "server.h"

namespace janus {
namespace raft {

/**
 * RaftServerDispatcher adapts RaftServer's OnX(out-params) RPC handlers
 * into DispatcherFacade's value-returning handle_* shape.
 */
class RaftServerDispatcher {
 public:
  explicit RaftServerDispatcher(::janus::RaftServer* svr) : svr_(svr) {}

  VoteReply handle_vote(VoteReq req) {
    VoteReply resp{};
    if (ServerUnavailable()) {
      resp.max_ballot = req.current_term;
      resp.vote_granted = false;
      return resp;
    }
    bool_t vote_granted = false;
    svr_->OnRequestVote(req.last_log_idx, req.last_log_term,
                        req.candidate_site_id, req.current_term,
                        &resp.max_ballot, &vote_granted);
    resp.vote_granted = vote_granted;
    return resp;
  }

  VoteDurableReply handle_vote_durable(VoteDurableReq req) {
    VoteDurableReply resp{};
    if (ServerUnavailable()) {
      resp.acknowledged = false;
      return resp;
    }
    bool_t acknowledged = false;
    svr_->OnVoteDurable(req.term, req.voter_id, &acknowledged);
    resp.acknowledged = acknowledged;
    return resp;
  }

  AppendEntriesReply handle_append_entries(AppendEntriesReq req) {
    AppendEntriesReply resp{};
    if (ServerUnavailable()) {
      resp.follower_append_ok = 0;
      resp.follower_current_term = 0;
      resp.follower_last_log_index = 0;
      resp.follower_ack_type = 0;
      return resp;
    }

    resp.follower_ack_type = 0;  // Memory ack, response sent before fsync.
    auto cmd = req.cmd.inner();
    svr_->OnAppendEntries(req.slot, req.ballot, req.leader_current_term,
                          req.leader_site_id, req.leader_prev_log_index,
                          req.leader_prev_log_term, req.leader_commit_index,
                          cmd, req.leader_next_log_term,
                          &resp.follower_append_ok,
                          &resp.follower_current_term,
                          &resp.follower_last_log_index);
    return resp;
  }

  EmptyAppendEntriesReply handle_empty_append_entries(EmptyAppendEntriesReq req) {
    EmptyAppendEntriesReply resp{};
    if (ServerUnavailable()) {
      resp.follower_append_ok = 0;
      resp.follower_current_term = 0;
      resp.follower_last_log_index = 0;
      resp.follower_ack_type = 0;
      return resp;
    }

    resp.follower_ack_type = 0;
    std::shared_ptr<Marshallable> cmd = nullptr;
    svr_->OnAppendEntries(req.slot, req.ballot, req.leader_current_term,
                          req.leader_site_id, req.leader_prev_log_index,
                          req.leader_prev_log_term, req.leader_commit_index,
                          cmd, 0,
                          &resp.follower_append_ok,
                          &resp.follower_current_term,
                          &resp.follower_last_log_index,
                          req.trigger_election_now);
    return resp;
  }

  AppendEntriesDurableReply handle_append_entries_durable(AppendEntriesDurableReq req) {
    AppendEntriesDurableReply resp{};
    if (ServerUnavailable()) {
      resp.acknowledged = false;
      return resp;
    }
    bool_t acknowledged = false;
    svr_->OnAppendEntriesDurable(req.term, req.follower_id,
                                 req.last_log_index, &acknowledged);
    resp.acknowledged = acknowledged;
    return resp;
  }

  TimeoutNowReply handle_timeout_now(TimeoutNowReq req) {
    TimeoutNowReply resp{};
    if (ServerUnavailable()) {
      resp.follower_term = 0;
      resp.success = false;
      return resp;
    }
    bool_t success = false;
    svr_->OnTimeoutNow(req.leader_term, req.leader_site_id,
                       &resp.follower_term, &success);
    resp.success = success;
    return resp;
  }

  NotifyRestartReply handle_notify_restart(NotifyRestartReq req) {
    NotifyRestartReply resp{};
    if (ServerUnavailable()) {
      resp.acknowledged = false;
      return resp;
    }

    auto commo = svr_->commo();
    if (commo != nullptr) {
      resp.acknowledged =
          commo->ReconnectToSite(req.restarted_site_id, svr_->partition_id_);
    } else {
      resp.acknowledged = false;
    }
    svr_->OnPeerRestart(req.restarted_site_id);
    return resp;
  }

  InstallSnapshotReply handle_install_snapshot(InstallSnapshotReq req) {
    InstallSnapshotReply resp{};
    if (ServerUnavailable()) {
      resp.term_out = 0;
      return resp;
    }
    svr_->OnInstallSnapshot(req.term, req.leader_id,
                            req.last_included_index, req.last_included_term,
                            req.data, &resp.term_out);
    return resp;
  }

  AddServerReply handle_add_server(AddServerReq req) {
    AddServerReply resp{};
    if (ServerUnavailable()) {
      resp.success = false;
      resp.error_msg = "server down";
      resp.leader_hint = 0;
      return resp;
    }

    bool_t success = false;
    svr_->OnAddServer(req.term, req.new_server_id, req.new_server_addr,
                      &success, &resp.error_msg, &resp.leader_hint);
    resp.success = success;
    return resp;
  }

  RemoveServerReply handle_remove_server(RemoveServerReq req) {
    RemoveServerReply resp{};
    if (ServerUnavailable()) {
      resp.success = false;
      resp.error_msg = "server down";
      resp.leader_hint = 0;
      return resp;
    }

    bool_t success = false;
    svr_->OnRemoveServer(req.term, req.server_id,
                         &success, &resp.error_msg, &resp.leader_hint);
    resp.success = success;
    return resp;
  }

 private:
  bool ServerUnavailable() const {
    return svr_ == nullptr || svr_->IsDisconnected();
  }

  ::janus::RaftServer* svr_{nullptr};
};

inline DispatcherProxy make_raft_server_dispatcher(::janus::RaftServer* svr) {
  return pro::make_proxy<DispatcherFacade, RaftServerDispatcher>(
      RaftServerDispatcher(svr));
}

}  // namespace raft
}  // namespace janus
