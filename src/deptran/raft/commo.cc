
#include <stdint.h>

#include "commo.h"
#include "../rcc/graph.h"
#include "../rcc/graph_marshaler.h"
#include "../command.h"
#include "../procedure.h"
#include "../command_marshaler.h"
#include "../rcc_rpc.h"
#include "macros.h"
#include <rusty/vec.hpp>

import std;
import rusty;

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
  namespace {

  // @unsafe - Compatibility bridge for the Raft Rust-DSL response shape.
  // Reactor registration is owned by an Arc after the upstream event
  // migration, while AppendEntriesResponse intentionally retains the branch's
  // shared_ptr<IntEvent> API. The shared_ptr's deleter owns the Arc and never
  // deletes the borrowed raw pointer directly.
  std::shared_ptr<IntEvent> commo_share_int_event(
      rusty::Arc<IntEvent> event) {
    IntEvent* event_ptr = event.as_ptr();
    return std::shared_ptr<IntEvent>(
        event_ptr,
        [event = std::move(event)](IntEvent*) mutable {
          // Dropping the captured Arc releases this ownership share.
          (void)event;
        });
  }

  }  // namespace

  // @safe
  RaftCommo::RaftCommo(rusty::Option<rusty::Arc<PollThread>> poll)
      : Communicator(std::move(poll)),
        notify_restart_core_(RaftCommoNotifyRestartCore::new_()),
        identity_core_(RaftCommoIdentityCore::new_())
  {
    //  verify(poll != nullptr);
  }

  // @unsafe - Legacy quorum RPC boundary: raw RaftProxy cast and async
  // FutureAttr callback. The returned shared_ptr is intentionally captured by
  // the callback so the response/event storage outlives this function.
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
    // Allocate response data with shared_ptr; the async callback captures it
    // and signals response->event when the legacy Future completes.
    auto response = std::make_shared<AppendEntriesResponse>(
        AppendEntriesResponse::defaults());
    response->event = commo_share_int_event(
        Reactor::create_sp_event<IntEvent>());

    auto proxies = rpc_par_proxies_[par_id];
    // vector<rusty::Arc<Future>> fus;
    WAN_WAIT;
    for (auto &p : proxies)
    {
      if (!commo_proxy_is_target(p.first, site_id))
        continue;
      Log_debug("[RPC-SEND] Sending AppendEntries to site {} via proxy {}",
                site_id, static_cast<void*>(p.second));
      auto follower_id = p.first;
      RaftProxy *proxy;
      // @unsafe
      {
        proxy = (RaftProxy *)p.second;
      }
      FutureAttr fuattr;
      // Capture response shared_ptr so FutureAttr can run after this function
      // returns without dangling response/event storage.
      fuattr.callback = [response, site_id](rusty::Arc<Future> fu)
      {
        if (commo_future_failed(fu->get_error_code()))
        {
          // Don't reconnect here - rely on NotifyRestart mechanism instead
          Log_debug("[APPEND_RPC] Error response from site {}, error_code={}", site_id, fu->get_error_code());
          return;
        }
        uint64_t status = 0;
        uint64_t term = 0;
        uint64_t last_log_index = 0;
        uint64_t ack_type = 0;
        rrr::deserialize_from(fu->get_reply(), status, term, last_log_index, ack_type);
        response->apply_reply(status, term, last_log_index, ack_type);
        Log_debug("[APPEND_RPC] Success response from site {}: status={}, term={}, lastLogIndex={}, ackType={}",
                  site_id, response->status, response->term, response->last_log_index, response->ack_type);
        response->event->set(1);
      };

      if (commo_should_send_empty_append_entries(cmd.has_value()))
      {
        // send a heartbeat AppendEntries
        Log_debug("Heartbeat AppendEntries to site {} prevLogIndex={}", site_id, prevLogIndex);
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
        if (commo_future_result_ok(f.is_ok()))
        {
          Future::safe_release(f.unwrap().raw_future());
        }
      }
      else
      {
        // send a regular AppendEntries
        verify(cmd.has_value());

        Log_debug("AppendEntries to site {} for log index {}", site_id, prevLogIndex + 1);
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
        if (commo_future_result_ok(f.is_ok()))
        {
          Future::safe_release(f.unwrap().raw_future());
        }
      }
    }
    return response;
  }

  // @unsafe - Legacy quorum RPC boundary. The returned shared result is the
  // rendezvous object observed by the caller while the async callback fills it.
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
    auto res = std::make_shared<SendAppendEntriesResults>(
        SendAppendEntriesResults::defaults());
    auto proxies = rpc_par_proxies_[par_id];
    // vector<rusty::Arc<Future>> fus;
    WAN_WAIT;
    for (auto &p : proxies)
    {
      if (!commo_proxy_is_target(p.first, site_id))
        continue;
      auto follower_id = p.first;
      RaftProxy *proxy;
      // @unsafe
      {
        proxy = (RaftProxy *)p.second;
      }
      FutureAttr fuattr;
      // Capture res by shared_ptr because the legacy FutureAttr callback may
      // run after SendAppendEntries returns to the heartbeat loop.
      fuattr.callback = [res, cmd, site_id](rusty::Arc<Future> fu)
      {
        if (commo_future_failed(fu->get_error_code()))
        {
          // Don't reconnect here - rely on NotifyRestart mechanism instead
          Log_debug("[APPEND_RPC] Error response from site {}, error_code={}", site_id, fu->get_error_code());
          return;
        }
        uint64_t ok = 0;
        uint64_t follower_term = 0;
        uint64_t follower_last_log_index = 0;
        uint64_t follower_ack_type = 0;
        rrr::deserialize_from(fu->get_reply(), ok);
        rrr::deserialize_from(fu->get_reply(), follower_term);
        rrr::deserialize_from(fu->get_reply(), follower_last_log_index);
        rrr::deserialize_from(fu->get_reply(), follower_ack_type);
        // false, 0, 0, 0 is the return value reserved to simulate a lost RPC.
        // only set res->done if it's not a lost RPC
        res->apply_reply(ok, follower_term, follower_last_log_index,
                         follower_ack_type, cmd.has_value());
      };

      if (commo_should_send_empty_append_entries(cmd.has_value()))
      {
        // send a heartbeat AppendEntries
        Log_debug("Heartbeat AppendEntries to site {} prevLogIndex={} trigger_election={}",
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
        if (commo_future_result_ok(f.is_ok()))
        {
          Future::safe_release(f.unwrap().raw_future());
        }
      }
      else
      {
        // send a regular AppendEntries
        verify(cmd.has_value());

        Log_debug("AppendEntries to site {} for log index {}", site_id, prevLogIndex + 1);
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
        if (commo_future_result_ok(f.is_ok()))
        {
          Future::safe_release(f.unwrap().raw_future());
        }
      }
    }
    return res;
  }

  // @unsafe - Legacy fanout RPC boundary. The quorum event is shared with each
  // async vote callback and with the caller waiting for quorum.
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
    auto e = std::make_shared<RaftVoteQuorumEvent>(n, n / 2);
    auto proxies = rpc_par_proxies_[par_id];
    WAN_WAIT;
    for (auto &p : proxies)
    {
      auto site_id = p.first;
      if (commo_proxy_is_self(site_id, self_id))
      {
        continue;
      }
      RaftProxy *proxy;
      // @unsafe
      {
        proxy = (RaftProxy *)p.second;
      }
      FutureAttr fuattr;
      // Capture the quorum event by shared_ptr so peer replies can arrive
      // after BroadcastVote returns.
      fuattr.callback = [e, site_id](rusty::Arc<Future> fu)
      {
        if (commo_future_failed(fu->get_error_code()))
        {
          // Don't reconnect here - rely on NotifyRestart mechanism instead
          Log_debug("[VOTE_RPC] Error response from site {}, error_code={}", site_id, fu->get_error_code());
          return;
        }
        ballot_t term = 0;
        bool_t vote = false;
        rrr::deserialize_from(fu->get_reply(), term);
        rrr::deserialize_from(fu->get_reply(), vote);
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
      if (commo_future_result_ok(f.is_ok()))
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
  // @unsafe - legacy RPC boundary: raw RaftProxy cast and async FutureAttr callback.
  // The public callback has been migrated to rusty::Function, but it is stored
  // behind std::shared_ptr because FutureAttr::callback requires a copyable,
  // const-callable callback wrapper.
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
      if (!commo_proxy_is_target(p.first, site_id)) {
        continue;
      }

      RaftProxy* proxy;
      // @unsafe - rpc_par_proxies_ stores legacy untyped proxy pointers.
      // The Raft transport layer guarantees this entry is a RaftProxy*.
      {
        proxy = (RaftProxy*)p.second;
      }

      FutureAttr fuattr;

      fuattr.callback = [callback_ptr, site_id](rusty::Arc<Future> fu) {
        if (commo_future_failed(fu->get_error_code())) {
          Log_debug("[TIMEOUT-NOW-RPC] Failed to send TimeoutNow - network error (code={})",
                    fu->get_error_code());

          if (commo_callback_is_set(static_cast<bool>(*callback_ptr))) {
            (*callback_ptr)(false, 0);
          }
          return;
        }

        uint64_t follower_term = 0;
        bool_t success = false;

        rrr::deserialize_from(fu->get_reply(), follower_term);
        rrr::deserialize_from(fu->get_reply(), success);

        Log_info("[TIMEOUT-NOW-RPC] TimeoutNow RPC completed: success={}, follower_term={}",
                (int)success, follower_term);

        if (commo_callback_is_set(static_cast<bool>(*callback_ptr))) {
          (*callback_ptr)(success, follower_term);
        }
      };

      Log_info("[TIMEOUT-NOW-RPC] Sending TimeoutNow to site {} (term={})",
              site_id, leader_term);

      RaftProxy::RpcTimeoutNowRequest req{};
      req.leaderTerm = leader_term;
      req.leaderSiteId = leader_site_id;

      auto f = proxy->async_TimeoutNow(req, fuattr);
      _RPC_COUNT();

      if (commo_future_result_ok(f.is_ok())) {
        Future::safe_release(f.unwrap().raw_future());
      }

      return;
    }

    Log_warn("[TIMEOUT-NOW-RPC] Failed to send TimeoutNow - site {} not found in proxies",
            site_id);

    if (commo_callback_is_set(static_cast<bool>(*callback_ptr))) {
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
        if (commo_proxy_is_target(p.first, candidate_id))
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
        Log_warn("[SPEC-RAFT] SendVoteDurable: No proxy found for candidate {}", candidate_id);
        return;
      }
      FutureAttr fuattr;
      fuattr.callback = [candidate_id, term, voter_id](rusty::Arc<Future> fu)
      {
        if (commo_future_failed(fu->get_error_code()))
        {
          Log_debug("[SPEC-RAFT] VoteDurable RPC to {} failed with error {}",
                    candidate_id, fu->get_error_code());
          return;
        }
        bool_t ack = false;
        rrr::deserialize_from(fu->get_reply(), ack);
        Log_debug("[SPEC-RAFT] VoteDurable RPC to {} completed, ack={}", candidate_id, ack);
      };

      Log_info("[SPEC-RAFT] Sending VoteDurable to candidate {} (term={}, voter={})",
               candidate_id, term, voter_id);

      RaftProxy::RpcVoteDurableRequest req{};
      req.term = term;
      req.voter_id = voter_id;
      auto f = proxy->async_VoteDurable(req, fuattr);
      _RPC_COUNT();
      if (commo_future_result_ok(f.is_ok()))
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
    // @unsafe - Fire-and-forget async RPC. Callback captures only values; no
    // RaftCommo state is touched after this function returns.
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
        if (commo_proxy_is_target(p.first, leader_id))
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
        Log_warn("[SPEC-RAFT] SendAppendEntriesDurable: No proxy found for leader {}", leader_id);
        return;
      }

      FutureAttr fuattr;
      // Value-only capture keeps this completion callback independent of
      // RaftCommo lifetime.
      fuattr.callback = [leader_id, term, follower_id, lastLogIndex](rusty::Arc<Future> fu)
      {
        if (commo_future_failed(fu->get_error_code()))
        {
          Log_debug("[SPEC-RAFT] AppendEntriesDurable RPC to {} failed with error {}",
                    leader_id, fu->get_error_code());
          return;
        }
        bool_t ack = false;
        rrr::deserialize_from(fu->get_reply(), ack);
        Log_debug("[SPEC-RAFT] AppendEntriesDurable RPC to {} completed, ack={}", leader_id, ack);
      };

      Log_info("[SPEC-RAFT] Sending AppendEntriesDurable to leader {} (term={}, follower={}, lastIdx={})",
               leader_id, term, follower_id, lastLogIndex);

      RaftProxy::RpcAppendEntriesDurableRequest req{};
      req.term = term;
      req.follower_id = follower_id;
      req.lastLogIndex = lastLogIndex;
      auto f = proxy->async_AppendEntriesDurable(req, fuattr);
      _RPC_COUNT();
      if (commo_future_result_ok(f.is_ok()))
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
    // @unsafe - Broadcasts async restart notifications and updates
    // notify_restart_statuses(). Captures `this`, so RaftCommo must outlive any
    // in-flight FutureAttr callbacks on this path.
    void RaftCommo::SendNotifyRestart(siteid_t self_id, parid_t par_id)
    {
      auto proxies = rpc_par_proxies_[par_id];

      // Store self info for retry mechanism
      identity_core_.set_self_site_id(self_id);
      identity_core_.set_self_par_id(par_id);

      Log_info("[NOTIFY-RESTART] Broadcasting restart notification from site {} to {} peers",
               self_id, proxies.size());

      // Initialize all peers as PENDING
      {
        std::lock_guard<std::mutex> lock(notify_restart_mtx_);
        notify_restart_statuses().clear();
        for (auto &p : proxies)
        {
          if (commo_should_track_notify_restart_peer(p.first, self_id))
          {
            notify_restart_statuses()[p.first] = NotifyRestartStatus::PENDING;
          }
        }
      }

      for (auto &p : proxies)
      {
        auto site_id = p.first;
        if (commo_proxy_is_self(site_id, self_id))
        {
          continue; // Don't notify self
        }

        RaftProxy *proxy;
        // @unsafe
        {
          proxy = (RaftProxy *)p.second;
        }
        FutureAttr fuattr;

        // Capture `this` to update notify_restart_statuses(). The status map is
        // protected by notify_restart_mtx_, but lifetime is still manual.
        fuattr.callback = [this, site_id](rusty::Arc<Future> fu)
        {
          if (commo_future_failed(fu->get_error_code()))
          {
            // Error/timeout - keep PENDING for retry
            Log_warn("[NOTIFY-RESTART] Failed to notify site {} - error code {} (will retry)",
                     site_id, fu->get_error_code());
            // Status remains PENDING (already set)
            return;
          }

          bool_t acknowledged = false;
          rrr::deserialize_from(fu->get_reply(), acknowledged);

          {
            std::lock_guard<std::mutex> lock(notify_restart_mtx_);
            if (acknowledged)
            {
              // Peer reconnected to us
              notify_restart_statuses()[site_id] = NotifyRestartStatus::ACKNOWLEDGED;
              Log_info("[NOTIFY-RESTART] Site {} ACKNOWLEDGED - reconnected to us", site_id);
            }
            else
            {
              // Peer responded "I'm down" - no retry needed
              notify_restart_statuses()[site_id] = NotifyRestartStatus::DOWN;
              Log_info("[NOTIFY-RESTART] Site {} is DOWN - will reconnect when it restarts", site_id);
            }
          }
        };

        Log_info("[NOTIFY-RESTART] Sending NotifyRestart to site {}", site_id);
        RaftProxy::RpcNotifyRestartRequest req{};
        req.restartedSiteId = self_id;
        auto f = proxy->async_NotifyRestart(req, fuattr);
        _RPC_COUNT();
        if (commo_future_result_ok(f.is_ok()))
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
      rusty::Vec<siteid_t> pending_sites;

      // Get list of pending sites
      {
        std::lock_guard<std::mutex> lock(notify_restart_mtx_);
        for (auto &pair : notify_restart_statuses())
        {
          if (commo_notify_restart_is_pending(pair.second))
          {
            pending_sites.push(pair.first);
          }
        }
      }

      if (!commo_retry_has_pending_sites(
              static_cast<uint64_t>(pending_sites.size())))
      {
        Log_debug("[NOTIFY-RESTART] No pending sites to retry");
        return;
      }

      Log_info("[NOTIFY-RESTART] Retrying NotifyRestart for {} pending sites", pending_sites.size());

      auto proxies = rpc_par_proxies_[identity_core_.self_par_id()];

      for (siteid_t site_id : pending_sites)
      {
        // Find proxy for this site
        RaftProxy *proxy = nullptr;
        for (auto &p : proxies)
        {
          if (commo_proxy_is_target(p.first, site_id))
          {
            proxy = (RaftProxy *)p.second;
            break;
          }
        }

        if (proxy == nullptr)
        {
          Log_warn("[NOTIFY-RESTART] No proxy found for site {}", site_id);
          continue;
        }

        FutureAttr fuattr;
        // Retry callbacks have the same lifetime contract as SendNotifyRestart:
        // they capture `this` and must finish before RaftCommo destruction.
        fuattr.callback = [this, site_id](rusty::Arc<Future> fu)
        {
          if (commo_future_failed(fu->get_error_code()))
          {
            Log_warn("[NOTIFY-RESTART] Retry failed for site {} - error code {} (will retry again)",
                     site_id, fu->get_error_code());
            return;
          }

          bool_t acknowledged = false;
          rrr::deserialize_from(fu->get_reply(), acknowledged);

          {
            std::lock_guard<std::mutex> lock(notify_restart_mtx_);
            if (acknowledged)
            {
              notify_restart_statuses()[site_id] = NotifyRestartStatus::ACKNOWLEDGED;
              Log_info("[NOTIFY-RESTART] Retry: Site {} ACKNOWLEDGED", site_id);
            }
            else
            {
              notify_restart_statuses()[site_id] = NotifyRestartStatus::DOWN;
              Log_info("[NOTIFY-RESTART] Retry: Site {} is DOWN", site_id);
            }
          }
        };

        Log_info("[NOTIFY-RESTART] Retrying NotifyRestart to site {}", site_id);
        RaftProxy::RpcNotifyRestartRequest req{};
        req.restartedSiteId = identity_core_.self_site_id();
        auto f = proxy->async_NotifyRestart(req, fuattr);
        _RPC_COUNT();
        if (commo_future_result_ok(f.is_ok()))
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
      auto it = notify_restart_statuses().find(site_id);
      if (it == notify_restart_statuses().end())
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
      for (auto &pair : notify_restart_statuses())
      {
        if (commo_notify_restart_is_pending(pair.second))
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
    // @unsafe - legacy RPC boundary: raw RaftProxy cast and async FutureAttr callback.
    // Single-target RPC. The rusty::Function callback is moved into shared_ptr
    // so the legacy async callback can copy it and keep it alive until completion.
    void RaftCommo::SendInstallSnapshot(siteid_t site_id,
                                        parid_t par_id,
                                        uint64_t term,
                                        uint64_t leader_id,
                                        uint64_t last_included_index,
                                        uint64_t last_included_term,
                                        const std::string &data,
                                        rusty::Function<void(uint64_t)> callback)
    {
      auto proxies = rpc_par_proxies_[par_id];


      // @safe - Moves the user callback exactly once into shared ownership.
      // Needed because rusty::Function is move-only, while FutureAttr callback storage
      // expects a copyable lambda.
      auto callback_ptr =
          std::make_shared<rusty::Function<void(uint64_t)>>(std::move(callback));


      // Find the target proxy
      for (auto &p : proxies)
      {
        if (!commo_proxy_is_target(p.first, site_id))
        {
          continue;
        }

        RaftProxy *proxy;
        // @unsafe - legacy proxy table stores void/base proxy pointers;
        // this RPC path expects the entry to be a RaftProxy*.
        {
          proxy = (RaftProxy *)p.second;
        }
        FutureAttr fuattr;

        // @unsafe - callback runs later through the legacy RPC runtime.
        // Captures only copyable values/shared ownership.
        fuattr.callback = [callback_ptr, site_id](rusty::Arc<Future> fu)
        {
          if (commo_future_failed(fu->get_error_code()))
          {
            Log_debug("[INSTALL-SNAPSHOT-RPC] Failed to send InstallSnapshot to site {} - error code {}",
                      site_id, fu->get_error_code());
            if (commo_callback_is_set(static_cast<bool>(*callback_ptr)))
            {
              (*callback_ptr)(0);
            }
            return;
          }

          uint64_t follower_term = 0;
          rrr::deserialize_from(fu->get_reply(), follower_term);

          Log_info("[INSTALL-SNAPSHOT-RPC] InstallSnapshot response from site {}: term={}",
                   site_id, follower_term);

          if (commo_callback_is_set(static_cast<bool>(*callback_ptr)))
          {
            (*callback_ptr)(follower_term);
          }
        };

        Log_info("[INSTALL-SNAPSHOT-RPC] Sending InstallSnapshot to site {} (term={}, lastIdx={}, lastTerm={}, dataSize={})",
                 site_id, term, last_included_index, last_included_term, data.size());

        RaftProxy::RpcInstallSnapshotRequest req{};
        req.term = term;
        req.leader_id = leader_id;
        req.last_included_index = last_included_index;
        req.last_included_term = last_included_term;
        req.data = data;
        auto f = proxy->async_InstallSnapshot(req, fuattr);
        _RPC_COUNT();
        if (commo_future_result_ok(f.is_ok()))
        {
          Future::safe_release(f.unwrap().raw_future());
        }

        return; // Found and sent to target
      }

      // Target not found in proxy list
      Log_warn("[INSTALL-SNAPSHOT-RPC] Failed to send InstallSnapshot - site {} not found in proxies",
               site_id);
      if (commo_callback_is_set(static_cast<bool>(*callback_ptr)))
      {
        (*callback_ptr)(0);
      }
    }

    // ============================================================================
    // callback-shaped quorum RPCs
    // ============================================================================

    // @unsafe - legacy RPC boundary: raw RaftProxy cast and async FutureAttr callback.
    // Single-target callback-shaped AppendEntries RPC. The rusty::Function callback
    // is moved into shared_ptr so the async lambda remains copyable.
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
        rusty::Function<void(siteid_t, raft::AppendEntriesReply)> on_reply)
    {
      auto proxies = rpc_par_proxies_[par_id];
      WAN_WAIT;
      // @safe - Moves the move-only rusty::Function into shared ownership.
      // FutureAttr::callback needs a copyable lambda, so the lambda captures the
      // shared_ptr instead of copying the Function.
      auto on_reply_ptr = std::make_shared<rusty::Function<void(siteid_t, raft::AppendEntriesReply)>>(std::move(on_reply));

      for (auto &p : proxies)
      {
        if (!commo_proxy_is_target(p.first, site_id))
          continue;
        auto follower_id = p.first;
        RaftProxy *proxy;
        // @unsafe - legacy proxy table stores untyped proxy pointers;
        // this path assumes the selected proxy is a RaftProxy*.
        {
          proxy = (RaftProxy *)p.second;
        }
        FutureAttr fuattr;
        // @safe - Keep a value copy of cmd alive across the async boundary.
        // The request itself is sent immediately, but the callback may run later.
        auto cmd_keep = cmd; // keep alive across the async boundary

        // @unsafe - callback is invoked by the legacy RPC runtime after this function returns.
        fuattr.callback = [on_reply_ptr, cmd_keep, follower_id](rusty::Arc<Future> fu)
        {
          if (commo_future_failed(fu->get_error_code()))
          {
            Log_debug("[APPEND_RPC_CB] Error from site {} code={}",
                      follower_id, fu->get_error_code());
            return;
          }
          uint64_t ok = 0;
          uint64_t term = 0;
          uint64_t last_log_index = 0;
          uint64_t ack_type = 0;
          rrr::deserialize_from(fu->get_reply(), ok);
          rrr::deserialize_from(fu->get_reply(), term);
          rrr::deserialize_from(fu->get_reply(), last_log_index);
          rrr::deserialize_from(fu->get_reply(), ack_type);
          raft::AppendEntriesReply r = commo_make_append_entries_reply(
              ok, term, last_log_index, ack_type);
          if (commo_callback_is_set(static_cast<bool>(*on_reply_ptr))) {
            (*on_reply_ptr)(follower_id, r);
          }
        };

        if (commo_should_send_empty_append_entries(cmd.has_value()))
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
          if (commo_future_result_ok(f.is_ok()))
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
          if (commo_future_result_ok(f.is_ok()))
          {
            Future::safe_release(f.unwrap().raw_future());
          }
        }
        return;
      }
    }

    // @unsafe - legacy RPC boundary with fanout: raw RaftProxy casts and async
    // FutureAttr callbacks. The same rusty::Function callback is shared across
    // multiple peer replies through shared_ptr.
    void RaftCommo::BroadcastVoteCb(
        parid_t par_id,
        slotid_t lst_log_idx,
        ballot_t lst_log_term,
        siteid_t self_id,
        ballot_t cur_term,
        rusty::Function<void(siteid_t, raft::VoteReply)> on_reply)
    {
      auto proxies = rpc_par_proxies_[par_id];
      WAN_WAIT;

      // @safe - BroadcastVoteCb fans out to many peers, so the move-only
      // rusty::Function cannot be moved into each lambda. shared_ptr gives each
      // async callback shared access to the same reply handler.
      auto on_reply_ptr = std::make_shared<rusty::Function<void(siteid_t, raft::VoteReply)>>(std::move(on_reply));
      for (auto &p : proxies)
      {
        auto site_id = p.first;
        if (commo_proxy_is_self(site_id, self_id))
          continue;
        RaftProxy *proxy;
        // @unsafe - legacy proxy table stores untyped proxy pointers;
        // each peer entry is expected to be a RaftProxy*.
        {
          proxy = (RaftProxy *)p.second;
        }
        FutureAttr fuattr;
        // @unsafe - callback is invoked asynchronously by the legacy RPC runtime.
        // Captures only site_id and shared ownership of the reply handler.
        fuattr.callback = [on_reply_ptr, site_id](rusty::Arc<Future> fu)
        {
          if (commo_future_failed(fu->get_error_code()))
          {
            Log_debug("[VOTE_RPC_CB] Error from site {} code={}",
                      site_id, fu->get_error_code());
            return;
          }
          ballot_t term = 0;
          bool_t vote = false;
          rrr::deserialize_from(fu->get_reply(), term);
          rrr::deserialize_from(fu->get_reply(), vote);
          raft::VoteReply r = commo_make_vote_reply(term, vote);
          if (commo_callback_is_set(static_cast<bool>(*on_reply_ptr)))
          {
            (*on_reply_ptr)(site_id, r);
          }
        };
        RaftProxy::RpcVoteRequest req{};
        req.lst_log_idx = lst_log_idx;
        req.lst_log_term = lst_log_term;
        req.site_id = self_id;
        req.cur_term = cur_term;
        auto f = proxy->async_Vote(req, fuattr);
        _RPC_COUNT();
        if (commo_future_result_ok(f.is_ok()))
        {
          Future::safe_release(f.unwrap().raw_future());
        }
      }
    }

  } // namespace janus
