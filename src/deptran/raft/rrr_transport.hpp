#pragma once

/**
 * @file rrr_transport.hpp
 * @brief Phase 2 of docs/dev/raft_decouple_plan.md — the production
 *        transport adapter that wraps RaftCommo / RaftProxy and exposes
 *        TransportFacade's callback-shaped API.
 *
 * Design note: the current RaftCommo return shape (shared_ptr<
 * SendAppendEntriesResults> / shared_ptr<RaftVoteQuorumEvent>) embeds a
 * QuorumEvent + FutureAttr callback slot. The TransportFacade however
 * exposes pure callback-shaped methods. For the MVP, this adapter
 * implements the fire-and-forget RPCs directly (send_timeout_now,
 * send_vote_durable, send_append_entries_durable, send_notify_restart,
 * self_site_id). The quorum-returning RPCs
 * (send_append_entries, send_empty_append_entries, broadcast_vote,
 * send_install_snapshot) are stubbed with `verify(0)` until RaftCommo is
 * refactored to accept callback parameters directly. This keeps the
 * facade compilable and callable from the test-side adapter without
 * regressing the production hot path, which will continue to call
 * RaftCommo directly from RaftServer until phase 2.5 completes the
 * refactor on the server.cc side.
 *
 * Rusty-safety:
 *  - No inheritance (the adapter is a plain struct; polymorphism comes
 *    from the TransportProxy facade elsewhere).
 *  - Non-owning raw pointer to RaftCommo (@unsafe). RaftCommo lives on
 *    RaftServer, which outlives every adapter built on top of it.
 *    rusty::Arc<RaftCommo> is unusable here because Arc<T>::operator->
 *    yields const access but RaftCommo's Send* methods are non-const.
 *  - No std::shared_ptr / std::function in new fields — callbacks are
 *    rusty::Function.
 */

#include <cstdint>

#include <rusty/function.hpp>

// transport.hpp pulls in <proxy/proxy.h>; include it before `import rrr;`
// so the proxy library's template machinery parses cleanly.
#include "transport.hpp"
#include "messages.hpp"

import rrr;

#include "../constants.h"
#include "commo.h"

namespace janus {
namespace raft {

// Wraps the in-tree RaftCommo so the proxy library can bind it via
// TransportFacade. Intentionally a plain struct (no inheritance).
class RrrTransportAdapter {
 public:
  // @unsafe { non-owning raw pointer; caller must ensure commo outlives this }
  RrrTransportAdapter(RaftCommo* commo, siteid_t self, parid_t par)
      : commo_(commo), self_(self), par_(par) {}

  // ------------------------------------------------------------------
  // TransportFacade conventions
  // ------------------------------------------------------------------

  // @safe - identity read
  siteid_t self_site_id() const { return self_; }

  // @unsafe { RaftCommo::SendTimeoutNow calls std::function }
  void send_timeout_now(siteid_t dst,
                        TimeoutNowReq req,
                        OnTimeoutNowReply on_reply) {
    // Bridge rusty::Function -> std::function through a shared holder so
    // the closure survives the async boundary. The holder owns the
    // rusty::Function and is kept alive by the lambda capture.
    struct Holder { OnTimeoutNowReply cb; siteid_t dst; };
    auto holder = std::make_shared<Holder>(Holder{std::move(on_reply), dst});
    // @unsafe { std::function bridge }
    commo_->SendTimeoutNow(
        dst, par_, req.leader_term, req.leader_site_id,
        [holder](bool success, uint64_t follower_term) {
          TimeoutNowReply r{};
          r.follower_term = follower_term;
          r.success = success;
          holder->cb(holder->dst, std::move(r));
        });
  }

  // @safe - forward fire-and-forget
  void send_vote_durable(siteid_t candidate, VoteDurableReq req) {
    commo_->SendVoteDurable(candidate, par_, req.term, req.voter_id);
  }

  // @safe - forward fire-and-forget
  void send_append_entries_durable(siteid_t leader, AppendEntriesDurableReq req) {
    commo_->SendAppendEntriesDurable(
        leader, par_, req.term, req.follower_id, req.last_log_index);
  }

  // @safe - forward fire-and-forget
  void send_notify_restart(siteid_t self, parid_t par) {
    commo_->SendNotifyRestart(self, par);
  }

  // ------------------------------------------------------------------
  // Quorum-returning RPCs — implemented in phase 2.5 against the *Cb
  // variants RaftCommo grew. Each bridges rusty::Function into
  // std::function via a shared holder that keeps the rusty::Function
  // alive across the async boundary.
  // ------------------------------------------------------------------

  // @unsafe { std::function bridge }
  void send_append_entries(siteid_t dst,
                           AppendEntriesReq req,
                           OnAppendEntriesReply on_reply) {
    // Holder keeps the rusty::Function alive until the reply fires.
    struct Holder { OnAppendEntriesReply cb; };
    auto holder = std::make_shared<Holder>(Holder{std::move(on_reply)});
    // @unsafe { MarshallDeputy::inner() returns a std::shared_ptr }
    auto cmd = req.cmd.inner();
    commo_->SendAppendEntriesCb(
        dst, par_,
        req.slot, req.ballot,
        /*isLeader=*/true,
        req.leader_site_id,
        req.leader_current_term,
        req.leader_prev_log_index,
        req.leader_prev_log_term,
        req.leader_commit_index,
        cmd,
        req.leader_next_log_term,
        /*trigger_election_now=*/false,
        [holder](siteid_t from, AppendEntriesReply r) {
          holder->cb(from, std::move(r));
        });
  }

  // @unsafe { std::function bridge; cmd=null triggers heartbeat path }
  void send_empty_append_entries(siteid_t dst,
                                 EmptyAppendEntriesReq req,
                                 OnAppendEntriesReply on_reply) {
    struct Holder { OnAppendEntriesReply cb; };
    auto holder = std::make_shared<Holder>(Holder{std::move(on_reply)});
    commo_->SendAppendEntriesCb(
        dst, par_,
        req.slot, req.ballot,
        /*isLeader=*/true,
        req.leader_site_id,
        req.leader_current_term,
        req.leader_prev_log_index,
        req.leader_prev_log_term,
        req.leader_commit_index,
        /*cmd=*/nullptr,
        /*cmdLogTerm=*/0,
        req.trigger_election_now,
        [holder](siteid_t from, AppendEntriesReply r) {
          holder->cb(from, std::move(r));
        });
  }

  // @unsafe { std::function bridge }
  void broadcast_vote(parid_t par, VoteReq req, OnVoteReply on_reply) {
    struct Holder { OnVoteReply cb; };
    auto holder = std::make_shared<Holder>(Holder{std::move(on_reply)});
    commo_->BroadcastVoteCb(
        par,
        req.last_log_idx, req.last_log_term,
        req.candidate_site_id, req.current_term,
        [holder](siteid_t from, VoteReply r) {
          holder->cb(from, std::move(r));
        });
  }

  // @unsafe { std::function bridge; existing SendInstallSnapshot already
  //           takes a std::function, we just adapt its reply shape. }
  void send_install_snapshot(siteid_t dst,
                             InstallSnapshotReq req,
                             OnInstallSnapshotReply on_reply) {
    struct Holder { OnInstallSnapshotReply cb; siteid_t dst; };
    auto holder = std::make_shared<Holder>(
        Holder{std::move(on_reply), dst});
    commo_->SendInstallSnapshot(
        dst, par_,
        req.term, req.leader_id,
        req.last_included_index, req.last_included_term,
        req.data,
        [holder](uint64_t follower_term) {
          InstallSnapshotReply r{};
          r.term_out = follower_term;
          holder->cb(holder->dst, std::move(r));
        });
  }

 private:
  RaftCommo* commo_{nullptr};
  siteid_t   self_{0};
  parid_t    par_{0};
};

// @unsafe { factory takes a non-owning RaftCommo* }
inline TransportProxy make_rrr_transport(RaftCommo* commo,
                                         siteid_t  self,
                                         parid_t   par) {
  return pro::make_proxy<TransportFacade, RrrTransportAdapter>(
      commo, self, par);
}

}  // namespace raft
}  // namespace janus
