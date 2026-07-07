
#include <stdint.h>

#include "commo.h"
#include "../rcc/graph.h"
#include "../rcc/graph_marshaler.h"
#include "../command.h"
#include "../procedure.h"
#include "../command_marshaler.h"
#include "../rcc_rpc.h"
#include "macros.h"

import std;

// @external: {
//   Log_info: [safe, (...) -> void]
//   Log_debug: [safe, (...) -> void]
//   Log_warn: [safe, (...) -> void]
//   verify: [safe, (...) -> void]
//   Config::GetConfig: [safe, () -> *]
//   Reactor::create_sp_event: [safe, (...) -> owned]
//   std::make_shared: [safe, (...) -> owned]
//   operator bool: [safe, (&'a) -> bool]
// }

namespace janus
{

  // @safe
  RaftCommo::RaftCommo(rusty::Option<rusty::Arc<PollThread>> poll) : Communicator(std::move(poll))
  {
    //  verify(poll != nullptr);
  }

  // @unsafe - C-style casts in @unsafe blocks, external calls marked @external [safe]
  // Returns shared_ptr<AppendEntriesResponse> - callback captures this to ensure memory validity
  shared_ptr<AppendEntriesResponse>
  RaftCommo::SendAppendEntries2(siteid_t site_id,
                                parid_t par_id,
                                slotid_t slot_id,
                                ballot_t ballot,
                                bool isLeader,
                                siteid_t leader_site_id,
                                uint64_t currentTerm,
                                uint64_t prevLogIndex,
                                uint64_t prevLogTerm,
                                uint64_t commitIndex,
                                const janus::Command &cmd,
                                uint64_t cmdLogTerm)
  {
    // Allocate response data with shared_ptr - callback captures this to keep memory valid
    auto response = std::make_shared<AppendEntriesResponse>();
    response->event = Reactor::create_sp_event<IntEvent>();

    auto proxies = rpc_par_proxies_[par_id];
    vector<rusty::Arc<Future>> fus;
    WAN_WAIT;
    for (auto &p : proxies)
    {
      if (p.first != site_id)
        continue;
      Log_debug("[RPC-SEND] Sending AppendEntries to site %d via proxy %p", site_id, p.second);
      auto follower_id = p.first;
      RaftProxy *proxy;
      // @unsafe
      {
        proxy = (RaftProxy *)p.second;
      }
      FutureAttr fuattr;
      // Capture response shared_ptr - ensures memory stays valid even after caller releases
      fuattr.callback = [response, site_id](rusty::Arc<Future> fu)
      {
        if (fu->get_error_code() != 0)
        {
          // Don't reconnect here - rely on NotifyRestart mechanism instead
          Log_debug("[APPEND_RPC] Error response from site %d, error_code=%d", site_id, fu->get_error_code());
          return;
        }
        fu->get_reply() >> response->status >> response->term >> response->last_log_index >> response->ack_type;
        Log_debug("[APPEND_RPC] Success response from site %d: status=%lu, term=%lu, lastLogIndex=%lu, ackType=%lu",
                  site_id, response->status, response->term, response->last_log_index, response->ack_type);
        response->event->set(1);
      };

      if (!cmd.has_value())
      {
        // send a heartbeat AppendEntries
        Log_debug("Heartbeat AppendEntries to site %d prevLogIndex=%ld", site_id, prevLogIndex);
        RaftProxy::RpcEmptyAppendEntriesRequest req{};
        req.slot = slot_id;
        req.ballot = ballot;
        req.leaderCurrentTerm = currentTerm;
        req.leaderSiteId = leader_site_id;
        req.leaderPrevLogIndex = prevLogIndex;
        req.leaderPrevLogTerm = prevLogTerm;
        req.leaderCommitIndex = commitIndex;
        req.trigger_election_now = false;
        auto f = proxy->async_EmptyAppendEntries(req, fuattr);
        _RPC_COUNT();
        if (f.is_ok())
        {
          Future::safe_release(f.unwrap().raw_future());
        }
      }
      else
      {
        // send a regular AppendEntries
        verify(cmd.has_value());

        Log_debug("AppendEntries to site %d for log index %d", site_id, prevLogIndex + 1);
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
        auto f = proxy->async_AppendEntries(req, fuattr);
        _RPC_COUNT();
        if (f.is_ok())
        {
          Future::safe_release(f.unwrap().raw_future());
        }
      }
    }
    return response;
  }

  // @unsafe - C-style casts in @unsafe blocks, external calls marked @external [safe]
  shared_ptr<SendAppendEntriesResults>
  RaftCommo::SendAppendEntries(siteid_t site_id,
                               parid_t par_id,
                               slotid_t slot_id,
                               ballot_t ballot,
                               bool isLeader,
                               siteid_t leader_site_id,
                               uint64_t currentTerm,
                               uint64_t prevLogIndex,
                               uint64_t prevLogTerm,
                               uint64_t commitIndex,
                               const janus::Command &cmd,
                               uint64_t cmdLogTerm,
                               bool trigger_election_now)
  {
    // verify(par_id == 0);
    shared_ptr<SendAppendEntriesResults> res;
    // @unsafe
    {
      res = shared_ptr<SendAppendEntriesResults>(new SendAppendEntriesResults());
    }
    auto proxies = rpc_par_proxies_[par_id];
    vector<rusty::Arc<Future>> fus;
    WAN_WAIT;
    for (auto &p : proxies)
    {
      if (p.first != site_id)
        continue;
      auto follower_id = p.first;
      RaftProxy *proxy;
      // @unsafe
      {
        proxy = (RaftProxy *)p.second;
      }
      FutureAttr fuattr;
      fuattr.callback = [res, cmd, site_id](rusty::Arc<Future> fu)
      {
        if (fu->get_error_code() != 0)
        {
          // Don't reconnect here - rely on NotifyRestart mechanism instead
          Log_debug("[APPEND_RPC] Error response from site %d, error_code=%d", site_id, fu->get_error_code());
          return;
        }
        // std::lock_guard<std::recursive_mutex> lk(res->mtx);
        fu->get_reply() >> res->ok;
        fu->get_reply() >> res->followerTerm;
        fu->get_reply() >> res->followerLastLogIndex;
        fu->get_reply() >> res->followerAckType;
        res->empty = !cmd.has_value();
        // false, 0, 0, 0 is the return value reserved to simulate a lost RPC.
        // only set res->done if it's not a lost RPC
        if (res->ok == false && res->followerTerm == 0 && res->followerLastLogIndex == 0)
        {
          res->done = false;
        }
        else
        {
          res->done = true;
        }
      };

      if (!cmd.has_value())
      {
        // send a heartbeat AppendEntries
        Log_debug("Heartbeat AppendEntries to site %d prevLogIndex=%ld trigger_election=%d",
                  site_id, prevLogIndex, trigger_election_now);
        RaftProxy::RpcEmptyAppendEntriesRequest req{};
        req.slot = slot_id;
        req.ballot = ballot;
        req.leaderCurrentTerm = currentTerm;
        req.leaderSiteId = leader_site_id;
        req.leaderPrevLogIndex = prevLogIndex;
        req.leaderPrevLogTerm = prevLogTerm;
        req.leaderCommitIndex = commitIndex;
        req.trigger_election_now = trigger_election_now;
        auto f = proxy->async_EmptyAppendEntries(req, fuattr);
        _RPC_COUNT();
        if (f.is_ok())
        {
          Future::safe_release(f.unwrap().raw_future());
        }
      }
      else
      {
        // send a regular AppendEntries
        verify(cmd.has_value());

        Log_debug("AppendEntries to site %d for log index %d", site_id, prevLogIndex + 1);
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
        auto f = proxy->async_AppendEntries(req, fuattr);
        _RPC_COUNT();
        if (f.is_ok())
        {
          Future::safe_release(f.unwrap().raw_future());
        }
      }
    }
    return res;
  }

  // @unsafe - C-style casts in @unsafe blocks, external calls marked @external [safe]
  shared_ptr<RaftVoteQuorumEvent>
  RaftCommo::BroadcastVote(parid_t par_id,
                           slotid_t lst_log_idx,
                           ballot_t lst_log_term,
                           siteid_t self_id,
                           ballot_t cur_term)
  {
    int n = 0;
    // @unsafe
    {
      n = Config::GetConfig()->GetPartitionSize(par_id);
    }
    auto e = Reactor::create_sp_event<RaftVoteQuorumEvent>(n, n / 2);
    auto proxies = rpc_par_proxies_[par_id];
    WAN_WAIT;
    for (auto &p : proxies)
    {
      auto site_id = p.first;
      if (site_id == self_id)
      {
        continue;
      }
      RaftProxy *proxy;
      // @unsafe
      {
        proxy = (RaftProxy *)p.second;
      }
      FutureAttr fuattr;
      fuattr.callback = [e, site_id](rusty::Arc<Future> fu)
      {
        if (fu->get_error_code() != 0)
        {
          // Don't reconnect here - rely on NotifyRestart mechanism instead
          Log_debug("[VOTE_RPC] Error response from site %d, error_code=%d", site_id, fu->get_error_code());
          return;
        }
        ballot_t term = 0;
        bool_t vote = false;
        fu->get_reply() >> term;
        fu->get_reply() >> vote;
        // SPECULATIVE VOTING: Track which site voted yes
        e->FeedResponse(vote, term, site_id);
      };
      RaftProxy::RpcVoteRequest req{};
      req.lst_log_idx = lst_log_idx;
      req.lst_log_term = lst_log_term;
      req.site_id = self_id;
      req.cur_term = cur_term;
      auto f = proxy->async_Vote(req, fuattr);
      _RPC_COUNT();
      if (f.is_ok())
      {
        Future::safe_release(f.unwrap().raw_future());
      }
    }
    return std::move(e);
  }

  // ============================================================================
  // TimeoutNow RPC - Leadership Transfer Protocol
  // ============================================================================

  /**
   * SendTimeoutNow - Send TimeoutNow RPC to target replica
   *
   * Instructs target replica to start election immediately.
   * Part of leadership transfer protocol.
   *
   * Edge Cases Handled:
   * - RPC fails (network error) → callback(false, 0)
   * - RPC succeeds but target rejects → callback(false, follower_term)
   * - RPC succeeds and target starts election → callback(true, follower_term)
   */
  // @unsafe - C-style casts in @unsafe blocks, external calls marked @external [safe]
  // @unsafe - async RPC boundary; uses legacy rrr proxy casts internally
  void RaftCommo::SendTimeoutNow(
      siteid_t site_id,
      parid_t par_id,
      uint64_t leader_term,
      siteid_t leader_site_id,
      rusty::Function<void(bool, uint64_t)> callback) {
    auto proxies = rpc_par_proxies_[par_id];

    // FutureAttr::callback wants a copyable/const-callable wrapper.
    // rusty::Function is move-only and non-const-callable, so keep it behind
    // std::shared_ptr at this legacy async boundary.
    auto callback_ptr =
        std::make_shared<rusty::Function<void(bool, uint64_t)>>(std::move(callback));

    for (auto& p : proxies) {
      if (p.first != site_id) {
        continue;
      }

      RaftProxy* proxy;
      // @unsafe
      {
        proxy = (RaftProxy*)p.second;
      }

      FutureAttr fuattr;

      fuattr.callback = [callback_ptr, site_id](rusty::Arc<Future> fu) {
        if (fu->get_error_code() != 0) {
          Log_debug("[TIMEOUT-NOW-RPC] Failed to send TimeoutNow - network error (code=%d)",
                    fu->get_error_code());

          if (*callback_ptr) {
            (*callback_ptr)(false, 0);
          }
          return;
        }

        uint64_t follower_term = 0;
        bool_t success = false;

        fu->get_reply() >> follower_term;
        fu->get_reply() >> success;

        Log_info("[TIMEOUT-NOW-RPC] TimeoutNow RPC completed: success=%d, follower_term=%lu",
                (int)success, follower_term);

        if (*callback_ptr) {
          (*callback_ptr)(success, follower_term);
        }
      };

      Log_info("[TIMEOUT-NOW-RPC] Sending TimeoutNow to site %d (term=%lu)",
              site_id, leader_term);

      RaftProxy::RpcTimeoutNowRequest req{};
      req.leaderTerm = leader_term;
      req.leaderSiteId = leader_site_id;

      auto f = proxy->async_TimeoutNow(req, fuattr);
      _RPC_COUNT();

      if (f.is_ok()) {
        Future::safe_release(f.unwrap().raw_future());
      }

      return;
    }

    Log_warn("[TIMEOUT-NOW-RPC] Failed to send TimeoutNow - site %d not found in proxies",
            site_id);

    if (*callback_ptr) {
      (*callback_ptr)(false, 0);
    }
  }

    // ============================================================================
    // NotifyRestart RPC - Reconnection Protocol
    // ============================================================================

    // ============================================================================
    // VoteDurable RPC - Speculative Voting Protocol
    // ============================================================================

    /**
     * SendVoteDurable - Send VoteDurable RPC to candidate after vote is persisted
     *
     * Called after a follower has durably persisted its vote to disk.
     * This notifies the candidate that this vote is now durable.
     */
    // @unsafe
    void RaftCommo::SendVoteDurable(siteid_t candidate_id,
                                    parid_t par_id,
                                    ballot_t term,
                                    siteid_t voter_id)
    {
      auto &proxies = rpc_par_proxies_[par_id];

      // Find the proxy for the candidate
      RaftProxy *proxy = nullptr;
      for (auto &p : proxies)
      {
        if (p.first == candidate_id)
        {
          // @unsafe
          {
            proxy = (RaftProxy *)p.second;
          }
          break;
        }
      }

      if (proxy == nullptr)
      {
        Log_warn("[SPEC-RAFT] SendVoteDurable: No proxy found for candidate %d", candidate_id);
        return;
      }
      FutureAttr fuattr;
      fuattr.callback = [candidate_id, term, voter_id](rusty::Arc<Future> fu)
      {
        if (fu->get_error_code() != 0)
        {
          Log_debug("[SPEC-RAFT] VoteDurable RPC to %d failed with error %d",
                    candidate_id, fu->get_error_code());
          return;
        }
        bool_t ack = false;
        fu->get_reply() >> ack;
        Log_debug("[SPEC-RAFT] VoteDurable RPC to %d completed, ack=%d", candidate_id, ack);
      };

      Log_info("[SPEC-RAFT] Sending VoteDurable to candidate %d (term=%lu, voter=%d)",
               candidate_id, term, voter_id);

      RaftProxy::RpcVoteDurableRequest req{};
      req.term = term;
      req.voter_id = voter_id;
      auto f = proxy->async_VoteDurable(req, fuattr);
      _RPC_COUNT();
      if (f.is_ok())
      {
        Future::safe_release(f.unwrap().raw_future());
      }
    }

    // ============================================================================
    // AppendEntriesDurable RPC - Speculative Commit Protocol
    // ============================================================================

    /**
     * SendAppendEntriesDurable - Send durable ack to leader after log fsync
     *
     * Called after a follower has durably persisted log entries to disk.
     * This notifies the leader that entries up to lastLogIndex are now durable.
     */
    // @unsafe
    void RaftCommo::SendAppendEntriesDurable(siteid_t leader_id,
                                             parid_t par_id,
                                             ballot_t term,
                                             siteid_t follower_id,
                                             uint64_t lastLogIndex)
    {
      auto &proxies = rpc_par_proxies_[par_id];

      // Find the proxy for the leader
      RaftProxy *proxy = nullptr;
      for (auto &p : proxies)
      {
        if (p.first == leader_id)
        {
          // @unsafe
          {
            proxy = (RaftProxy *)p.second;
          }
          break;
        }
      }

      if (proxy == nullptr)
      {
        Log_warn("[SPEC-RAFT] SendAppendEntriesDurable: No proxy found for leader %d", leader_id);
        return;
      }

      FutureAttr fuattr;
      fuattr.callback = [leader_id, term, follower_id, lastLogIndex](rusty::Arc<Future> fu)
      {
        if (fu->get_error_code() != 0)
        {
          Log_debug("[SPEC-RAFT] AppendEntriesDurable RPC to %d failed with error %d",
                    leader_id, fu->get_error_code());
          return;
        }
        bool_t ack = false;
        fu->get_reply() >> ack;
        Log_debug("[SPEC-RAFT] AppendEntriesDurable RPC to %d completed, ack=%d", leader_id, ack);
      };

      Log_info("[SPEC-RAFT] Sending AppendEntriesDurable to leader %d (term=%lu, follower=%d, lastIdx=%lu)",
               leader_id, term, follower_id, lastLogIndex);

      RaftProxy::RpcAppendEntriesDurableRequest req{};
      req.term = term;
      req.follower_id = follower_id;
      req.lastLogIndex = lastLogIndex;
      auto f = proxy->async_AppendEntriesDurable(req, fuattr);
      _RPC_COUNT();
      if (f.is_ok())
      {
        Future::safe_release(f.unwrap().raw_future());
      }
    }

    // ============================================================================
    // NotifyRestart RPC - Recovery Protocol
    // ============================================================================

    /**
     * SendNotifyRestart - Broadcast restart notification to all peers
     *
     * Called after a server restarts to tell all other servers to reconnect
     * their client connections to this server.
     *
     * Status update logic:
     * - acknowledged=true  → ACKNOWLEDGED (peer reconnected to us)
     * - acknowledged=false → DOWN (peer is down, will reconnect when it restarts)
     * - error/timeout      → PENDING (should retry)
     */
    // @unsafe
    void RaftCommo::SendNotifyRestart(siteid_t self_id, parid_t par_id)
    {
      auto proxies = rpc_par_proxies_[par_id];

      // Store self info for retry mechanism
      self_site_id_ = self_id;
      self_par_id_ = par_id;

      Log_info("[NOTIFY-RESTART] Broadcasting restart notification from site %d to %zu peers",
               self_id, proxies.size());

      // Initialize all peers as PENDING
      {
        std::lock_guard<std::mutex> lock(notify_restart_mtx_);
        notify_restart_status_.clear();
        for (auto &p : proxies)
        {
          if (p.first != self_id)
          {
            notify_restart_status_[p.first] = NotifyRestartStatus::PENDING;
          }
        }
      }

      for (auto &p : proxies)
      {
        auto site_id = p.first;
        if (site_id == self_id)
        {
          continue; // Don't notify self
        }

        RaftProxy *proxy;
        // @unsafe
        {
          proxy = (RaftProxy *)p.second;
        }
        FutureAttr fuattr;

        // Capture 'this' to update status map
        fuattr.callback = [this, site_id](rusty::Arc<Future> fu)
        {
          if (fu->get_error_code() != 0)
          {
            // Error/timeout - keep PENDING for retry
            Log_warn("[NOTIFY-RESTART] Failed to notify site %d - error code %d (will retry)",
                     site_id, fu->get_error_code());
            // Status remains PENDING (already set)
            return;
          }

          bool_t acknowledged = false;
          fu->get_reply() >> acknowledged;

          {
            std::lock_guard<std::mutex> lock(notify_restart_mtx_);
            if (acknowledged)
            {
              // Peer reconnected to us
              notify_restart_status_[site_id] = NotifyRestartStatus::ACKNOWLEDGED;
              Log_info("[NOTIFY-RESTART] Site %d ACKNOWLEDGED - reconnected to us", site_id);
            }
            else
            {
              // Peer responded "I'm down" - no retry needed
              notify_restart_status_[site_id] = NotifyRestartStatus::DOWN;
              Log_info("[NOTIFY-RESTART] Site %d is DOWN - will reconnect when it restarts", site_id);
            }
          }
        };

        Log_info("[NOTIFY-RESTART] Sending NotifyRestart to site %d", site_id);
        RaftProxy::RpcNotifyRestartRequest req{};
        req.restartedSiteId = self_id;
        auto f = proxy->async_NotifyRestart(req, fuattr);
        _RPC_COUNT();
        if (f.is_ok())
        {
          Future::safe_release(f.unwrap().raw_future());
        }
      }
    }

    /**
     * RetryPendingNotifyRestart - Retry NotifyRestart for peers still in PENDING state
     */
    void RaftCommo::RetryPendingNotifyRestart()
    {
      std::vector<siteid_t> pending_sites;

      // Get list of pending sites
      {
        std::lock_guard<std::mutex> lock(notify_restart_mtx_);
        for (auto &pair : notify_restart_status_)
        {
          if (pair.second == NotifyRestartStatus::PENDING)
          {
            pending_sites.push_back(pair.first);
          }
        }
      }

      if (pending_sites.empty())
      {
        Log_debug("[NOTIFY-RESTART] No pending sites to retry");
        return;
      }

      Log_info("[NOTIFY-RESTART] Retrying NotifyRestart for %zu pending sites", pending_sites.size());

      auto proxies = rpc_par_proxies_[self_par_id_];

      for (siteid_t site_id : pending_sites)
      {
        // Find proxy for this site
        RaftProxy *proxy = nullptr;
        for (auto &p : proxies)
        {
          if (p.first == site_id)
          {
            proxy = (RaftProxy *)p.second;
            break;
          }
        }

        if (proxy == nullptr)
        {
          Log_warn("[NOTIFY-RESTART] No proxy found for site %d", site_id);
          continue;
        }

        FutureAttr fuattr;
        fuattr.callback = [this, site_id](rusty::Arc<Future> fu)
        {
          if (fu->get_error_code() != 0)
          {
            Log_warn("[NOTIFY-RESTART] Retry failed for site %d - error code %d (will retry again)",
                     site_id, fu->get_error_code());
            return;
          }

          bool_t acknowledged = false;
          fu->get_reply() >> acknowledged;

          {
            std::lock_guard<std::mutex> lock(notify_restart_mtx_);
            if (acknowledged)
            {
              notify_restart_status_[site_id] = NotifyRestartStatus::ACKNOWLEDGED;
              Log_info("[NOTIFY-RESTART] Retry: Site %d ACKNOWLEDGED", site_id);
            }
            else
            {
              notify_restart_status_[site_id] = NotifyRestartStatus::DOWN;
              Log_info("[NOTIFY-RESTART] Retry: Site %d is DOWN", site_id);
            }
          }
        };

        Log_info("[NOTIFY-RESTART] Retrying NotifyRestart to site %d", site_id);
        RaftProxy::RpcNotifyRestartRequest req{};
        req.restartedSiteId = self_site_id_;
        auto f = proxy->async_NotifyRestart(req, fuattr);
        _RPC_COUNT();
        if (f.is_ok())
        {
          Future::safe_release(f.unwrap().raw_future());
        }
      }
    }

    /**
     * GetNotifyRestartStatus - Get the current status for a peer
     */
    NotifyRestartStatus RaftCommo::GetNotifyRestartStatus(siteid_t site_id)
    {
      std::lock_guard<std::mutex> lock(notify_restart_mtx_);
      auto it = notify_restart_status_.find(site_id);
      if (it == notify_restart_status_.end())
      {
        return NotifyRestartStatus::PENDING;
      }
      return it->second;
    }

    /**
     * HasPendingNotifyRestart - Check if any peers still need notification
     */
    bool RaftCommo::HasPendingNotifyRestart()
    {
      std::lock_guard<std::mutex> lock(notify_restart_mtx_);
      for (auto &pair : notify_restart_status_)
      {
        if (pair.second == NotifyRestartStatus::PENDING)
        {
          return true;
        }
      }
      return false;
    }

    // ============================================================================
    // InstallSnapshot RPC - Snapshot Transfer Protocol
    // ============================================================================

    /**
     * SendInstallSnapshot - Send snapshot to a follower that is too far behind
     *
     * Sends the full snapshot in one RPC. The follower replaces its state
     * machine state and discards old log entries covered by the snapshot.
     */
    // @unsafe - C-style cast, std::function
    void RaftCommo::SendInstallSnapshot(siteid_t site_id,
                                        parid_t par_id,
                                        uint64_t term,
                                        uint64_t leader_id,
                                        uint64_t last_included_index,
                                        uint64_t last_included_term,
                                        const std::string &data,
                                        std::function<void(uint64_t follower_term)> callback)
    {
      auto proxies = rpc_par_proxies_[par_id];

      // Find the target proxy
      for (auto &p : proxies)
      {
        if (p.first != site_id)
        {
          continue;
        }

        RaftProxy *proxy;
        // @unsafe
        {
          proxy = (RaftProxy *)p.second;
        }
        FutureAttr fuattr;

        fuattr.callback = [callback, site_id](rusty::Arc<Future> fu)
        {
          if (fu->get_error_code() != 0)
          {
            Log_debug("[INSTALL-SNAPSHOT-RPC] Failed to send InstallSnapshot to site %d - error code %d",
                      site_id, fu->get_error_code());
            if (callback)
            {
              callback(0);
            }
            return;
          }

          uint64_t follower_term = 0;
          fu->get_reply() >> follower_term;

          Log_info("[INSTALL-SNAPSHOT-RPC] InstallSnapshot response from site %d: term=%lu",
                   site_id, follower_term);

          if (callback)
          {
            callback(follower_term);
          }
        };

        Log_info("[INSTALL-SNAPSHOT-RPC] Sending InstallSnapshot to site %d (term=%lu, lastIdx=%lu, lastTerm=%lu, dataSize=%zu)",
                 site_id, term, last_included_index, last_included_term, data.size());

        RaftProxy::RpcInstallSnapshotRequest req{};
        req.term = term;
        req.leader_id = leader_id;
        req.last_included_index = last_included_index;
        req.last_included_term = last_included_term;
        req.data = data;
        auto f = proxy->async_InstallSnapshot(req, fuattr);
        _RPC_COUNT();
        if (f.is_ok())
        {
          Future::safe_release(f.unwrap().raw_future());
        }

        return; // Found and sent to target
      }

      // Target not found in proxy list
      Log_warn("[INSTALL-SNAPSHOT-RPC] Failed to send InstallSnapshot - site %d not found in proxies",
               site_id);
      if (callback)
      {
        callback(0);
      }
    }

    // ============================================================================
    // callback-shaped quorum RPCs
    // ============================================================================

    // @unsafe - C-style casts, std::function captures
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
        const janus::Command &cmd,
        uint64_t cmdLogTerm,
        bool trigger_election_now,
        std::function<void(siteid_t, raft::AppendEntriesReply)> on_reply)
    {
      auto proxies = rpc_par_proxies_[par_id];
      WAN_WAIT;
      for (auto &p : proxies)
      {
        if (p.first != site_id)
          continue;
        auto follower_id = p.first;
        RaftProxy *proxy;
        // @unsafe
        {
          proxy = (RaftProxy *)p.second;
        }
        FutureAttr fuattr;
        auto cmd_keep = cmd; // keep alive across the async boundary
        fuattr.callback = [on_reply, cmd_keep, follower_id](rusty::Arc<Future> fu)
        {
          if (fu->get_error_code() != 0)
          {
            Log_debug("[APPEND_RPC_CB] Error from site %d code=%d",
                      follower_id, fu->get_error_code());
            return;
          }
          raft::AppendEntriesReply r{};
          fu->get_reply() >> r.follower_append_ok;
          fu->get_reply() >> r.follower_current_term;
          fu->get_reply() >> r.follower_last_log_index;
          fu->get_reply() >> r.follower_ack_type;
          on_reply(follower_id, r);
        };

        if (!cmd.has_value())
        {
          RaftProxy::RpcEmptyAppendEntriesRequest req{};
          req.slot = slot_id;
          req.ballot = ballot;
          req.leaderCurrentTerm = currentTerm;
          req.leaderSiteId = leader_site_id;
          req.leaderPrevLogIndex = prevLogIndex;
          req.leaderPrevLogTerm = prevLogTerm;
          req.leaderCommitIndex = commitIndex;
          req.trigger_election_now = trigger_election_now;
          auto f = proxy->async_EmptyAppendEntries(req, fuattr);
          _RPC_COUNT();
          if (f.is_ok())
          {
            Future::safe_release(f.unwrap().raw_future());
          }
        }
        else
        {
          verify(cmd.has_value());
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
          auto f = proxy->async_AppendEntries(req, fuattr);
          _RPC_COUNT();
          if (f.is_ok())
          {
            Future::safe_release(f.unwrap().raw_future());
          }
        }
        return;
      }
    }

    // @unsafe - C-style casts, std::function captures
    void RaftCommo::BroadcastVoteCb(
        parid_t par_id,
        slotid_t lst_log_idx,
        ballot_t lst_log_term,
        siteid_t self_id,
        ballot_t cur_term,
        std::function<void(siteid_t, raft::VoteReply)> on_reply)
    {
      auto proxies = rpc_par_proxies_[par_id];
      WAN_WAIT;
      for (auto &p : proxies)
      {
        auto site_id = p.first;
        if (site_id == self_id)
          continue;
        RaftProxy *proxy;
        // @unsafe
        {
          proxy = (RaftProxy *)p.second;
        }
        FutureAttr fuattr;
        fuattr.callback = [on_reply, site_id](rusty::Arc<Future> fu)
        {
          if (fu->get_error_code() != 0)
          {
            Log_debug("[VOTE_RPC_CB] Error from site %d code=%d",
                      site_id, fu->get_error_code());
            return;
          }
          raft::VoteReply r{};
          ballot_t term = 0;
          bool_t vote = false;
          fu->get_reply() >> term;
          fu->get_reply() >> vote;
          r.max_ballot = term;
          r.vote_granted = vote;
          on_reply(site_id, r);
        };
        RaftProxy::RpcVoteRequest req{};
        req.lst_log_idx = lst_log_idx;
        req.lst_log_term = lst_log_term;
        req.site_id = self_id;
        req.cur_term = cur_term;
        auto f = proxy->async_Vote(req, fuattr);
        _RPC_COUNT();
        if (f.is_ok())
        {
          Future::safe_release(f.unwrap().raw_future());
        }
      }
    }

  } // namespace janus
