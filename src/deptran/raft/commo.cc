#include <stdint.h>

#include "commo.h"

#include "../rcc_rpc.h"
#include "macros.h"

import std;

namespace janus {

namespace {

void CompleteAppendEntriesResponse(
    const shared_ptr<AppendEntriesResponse>& response) {
  if (!response->completed.exchange(true, std::memory_order_release)) {
    response->event.as_ref().unwrap()->set(1);
  }
}

}  // namespace

RaftCommo::RaftCommo(rusty::Option<rusty::Arc<rrr::PollThread>> poll)
    : Communicator(std::move(poll)) {}

shared_ptr<AppendEntriesResponse> RaftCommo::SendAppendEntries2(
    siteid_t site_id,
    parid_t par_id,
    slotid_t slot_id,
    ballot_t ballot,
    bool isLeader,
    siteid_t leader_site_id,
    uint64_t currentTerm,
    uint64_t prevLogIndex,
    uint64_t prevLogTerm,
    uint64_t commitIndex,
    const janus::Command& cmd,
    uint64_t cmdLogTerm) {
  (void)isLeader;
  auto response = std::make_shared<AppendEntriesResponse>();
  response->event = create_sp_int_event(1);
  auto peer = PeerForSite(par_id, site_id);
  if (!peer) {
    CompleteAppendEntriesResponse(response);
    return response;
  }

  FutureAttr attr;
  attr.callback = rrr::FutureCallback::from_callable(
      [response, site_id](rusty::Arc<Future> future) {
        if (commo_future_failed(future->get_error_code())) {
          Log_debug("[APPEND_RPC] Error response from site {}, error_code={}",
                    site_id, future->get_error_code());
          CompleteAppendEntriesResponse(response);
          return;
        }
        rrr::deserialize_from(future->get_reply(), response->status);
        rrr::deserialize_from(future->get_reply(), response->term);
        rrr::deserialize_from(future->get_reply(), response->last_log_index);
        Log_debug("[APPEND_RPC] Success response from site {}: status={}, "
                  "term={}, lastLogIndex={}",
                  site_id, response->status, response->term,
                  response->last_log_index);
        CompleteAppendEntriesResponse(response);
      });

  peer->WithClient([&](rrr::Client* client) {
    RaftProxy proxy(client);
    if (commo_append_entries_empty_from_cmd(cmd.has_value())) {
      Log_debug("Heartbeat AppendEntries to site {} prevLogIndex={}",
                site_id, prevLogIndex);
      RaftProxy::RpcEmptyAppendEntriesRequest req{};
      req.slot = slot_id;
      req.ballot = ballot;
      req.leaderCurrentTerm = currentTerm;
      req.leaderSiteId = leader_site_id;
      req.leaderPrevLogIndex = prevLogIndex;
      req.leaderPrevLogTerm = prevLogTerm;
      req.leaderCommitIndex = commitIndex;
      auto result = proxy.async_EmptyAppendEntries(req, attr);
      _RPC_COUNT();
      if (result.is_ok()) {
        Future::safe_release(result.unwrap().raw_future());
      } else {
        CompleteAppendEntriesResponse(response);
      }
    } else {
      Log_debug("AppendEntries to site {} for log index {}",
                site_id, prevLogIndex + 1);
      RaftProxy::RpcAppendEntriesRequest req{};
      req.slot = slot_id;
      req.ballot = ballot;
      req.leaderCurrentTerm = currentTerm;
      req.leaderSiteId = leader_site_id;
      req.leaderPrevLogIndex = prevLogIndex;
      req.leaderPrevLogTerm = prevLogTerm;
      req.leaderCommitIndex = commitIndex;
      req.cmd = cmd;
      req.leaderNextLogTerm = cmdLogTerm;
      auto result = proxy.async_AppendEntries(req, attr);
      _RPC_COUNT();
      if (result.is_ok()) {
        Future::safe_release(result.unwrap().raw_future());
      } else {
        CompleteAppendEntriesResponse(response);
      }
    }
  });
  return response;
}

shared_ptr<RaftVoteQuorumEvent> RaftCommo::BroadcastVote(
    parid_t par_id,
    slotid_t lst_log_idx,
    ballot_t lst_log_term,
    siteid_t self_id,
    ballot_t cur_term) {
  const int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto event = std::make_shared<RaftVoteQuorumEvent>(n, n / 2);
  for (const auto& peer : PeersForPartition(par_id)) {
    const auto site_id = peer->site_id();
    if (site_id == self_id) {
      continue;
    }

    FutureAttr attr;
    attr.callback = rrr::FutureCallback::from_callable(
        [event, site_id](rusty::Arc<Future> future) {
          if (commo_future_failed(future->get_error_code())) {
            Log_debug("[VOTE_RPC] Error response from site {}, error_code={}",
                      site_id, future->get_error_code());
            return;
          }
          ballot_t term = 0;
          bool_t vote = false;
          rrr::deserialize_from(future->get_reply(), term);
          rrr::deserialize_from(future->get_reply(), vote);
          event->FeedResponse(vote, term);
        });
    RaftProxy::RpcVoteRequest req{};
    req.lst_log_idx = lst_log_idx;
    req.lst_log_term = lst_log_term;
    req.site_id = self_id;
    req.cur_term = cur_term;
    peer->WithClient([&](rrr::Client* client) {
      RaftProxy proxy(client);
      auto result = proxy.async_Vote(req, attr);
      _RPC_COUNT();
      if (result.is_ok()) {
        Future::safe_release(result.unwrap().raw_future());
      }
    });
  }
  return event;
}

void RaftCommo::SendInstallSnapshot(
    siteid_t site_id,
    parid_t par_id,
    uint64_t term,
    uint64_t leader_id,
    uint64_t last_included_index,
    uint64_t last_included_term,
    const std::string& data,
    std::function<void(uint64_t)> callback) {
  auto peer = PeerForSite(par_id, site_id);
  if (!peer) {
    if (callback) {
      callback(0);
    }
    return;
  }

  FutureAttr attr;
  attr.callback = rrr::FutureCallback::from_callable(
      [callback, site_id](rusty::Arc<Future> future) {
        if (commo_future_failed(future->get_error_code())) {
          Log_debug("[INSTALL-SNAPSHOT-RPC] Failed to send to site {} - error {}",
                    site_id, future->get_error_code());
          if (callback) {
            callback(0);
          }
          return;
        }
        uint64_t follower_term = 0;
        rrr::deserialize_from(future->get_reply(), follower_term);
        if (callback) {
          callback(follower_term);
        }
      });
  RaftProxy::RpcInstallSnapshotRequest req{};
  req.term = term;
  req.leader_id = leader_id;
  req.last_included_index = last_included_index;
  req.last_included_term = last_included_term;
  req.data = data;
  peer->WithClient([&](rrr::Client* client) {
    RaftProxy proxy(client);
    auto result = proxy.async_InstallSnapshot(req, attr);
    _RPC_COUNT();
    if (result.is_ok()) {
      Future::safe_release(result.unwrap().raw_future());
    }
  });
}

void RaftCommo::SendAppendEntriesCb(
    siteid_t site_id,
    parid_t par_id,
    slotid_t slot_id,
    ballot_t ballot,
    bool isLeader,
    siteid_t leader_site_id,
    uint64_t currentTerm,
    uint64_t prevLogIndex,
    uint64_t prevLogTerm,
    uint64_t commitIndex,
    const janus::Command& cmd,
    uint64_t cmdLogTerm,
    std::function<void(siteid_t, raft::AppendEntriesReply)> on_reply) {
  (void)isLeader;
  auto peer = PeerForSite(par_id, site_id);
  if (!peer) {
    return;
  }

  auto cmd_keep = cmd;
  FutureAttr attr;
  attr.callback = rrr::FutureCallback::from_callable(
      [on_reply, cmd_keep, site_id](rusty::Arc<Future> future) {
        if (commo_future_failed(future->get_error_code())) {
          Log_debug("[APPEND_RPC_CB] Error from site {} code={}",
                    site_id, future->get_error_code());
          return;
        }
        raft::AppendEntriesReply reply{};
        rrr::deserialize_from(future->get_reply(), reply.follower_append_ok);
        rrr::deserialize_from(future->get_reply(), reply.follower_current_term);
        rrr::deserialize_from(future->get_reply(),
                              reply.follower_last_log_index);
        on_reply(site_id, reply);
      });

  peer->WithClient([&](rrr::Client* client) {
    RaftProxy proxy(client);
    if (commo_append_entries_empty_from_cmd(cmd.has_value())) {
      RaftProxy::RpcEmptyAppendEntriesRequest req{};
      req.slot = slot_id;
      req.ballot = ballot;
      req.leaderCurrentTerm = currentTerm;
      req.leaderSiteId = leader_site_id;
      req.leaderPrevLogIndex = prevLogIndex;
      req.leaderPrevLogTerm = prevLogTerm;
      req.leaderCommitIndex = commitIndex;
      auto result = proxy.async_EmptyAppendEntries(req, attr);
      _RPC_COUNT();
      if (result.is_ok()) {
        Future::safe_release(result.unwrap().raw_future());
      }
    } else {
      RaftProxy::RpcAppendEntriesRequest req{};
      req.slot = slot_id;
      req.ballot = ballot;
      req.leaderCurrentTerm = currentTerm;
      req.leaderSiteId = leader_site_id;
      req.leaderPrevLogIndex = prevLogIndex;
      req.leaderPrevLogTerm = prevLogTerm;
      req.leaderCommitIndex = commitIndex;
      req.cmd = cmd;
      req.leaderNextLogTerm = cmdLogTerm;
      auto result = proxy.async_AppendEntries(req, attr);
      _RPC_COUNT();
      if (result.is_ok()) {
        Future::safe_release(result.unwrap().raw_future());
      }
    }
  });
}

void RaftCommo::BroadcastVoteCb(
    parid_t par_id,
    slotid_t lst_log_idx,
    ballot_t lst_log_term,
    siteid_t self_id,
    ballot_t cur_term,
    std::function<void(siteid_t, raft::VoteReply)> on_reply) {
  for (const auto& peer : PeersForPartition(par_id)) {
    const auto site_id = peer->site_id();
    if (site_id == self_id) {
      continue;
    }
    FutureAttr attr;
    attr.callback = rrr::FutureCallback::from_callable(
        [on_reply, site_id](rusty::Arc<Future> future) {
          if (commo_future_failed(future->get_error_code())) {
            Log_debug("[VOTE_RPC_CB] Error from site {} code={}",
                      site_id, future->get_error_code());
            return;
          }
          raft::VoteReply reply{};
          ballot_t term = 0;
          bool_t vote = false;
          rrr::deserialize_from(future->get_reply(), term);
          rrr::deserialize_from(future->get_reply(), vote);
          reply.max_ballot = term;
          reply.vote_granted = vote;
          on_reply(site_id, reply);
        });
    RaftProxy::RpcVoteRequest req{};
    req.lst_log_idx = lst_log_idx;
    req.lst_log_term = lst_log_term;
    req.site_id = self_id;
    req.cur_term = cur_term;
    peer->WithClient([&](rrr::Client* client) {
      RaftProxy proxy(client);
      auto result = proxy.async_Vote(req, attr);
      _RPC_COUNT();
      if (result.is_ok()) {
        Future::safe_release(result.unwrap().raw_future());
      }
    });
  }
}

}  // namespace janus
