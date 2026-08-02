#pragma once

/**
 * @file rrr_transport.hpp
 * @brief Production TransportBase adapter that wraps RaftCommo /
 *        RaftProxy. Fiber-synchronous since Phase 8.0 — each
 *        reply-expecting send_* blocks the calling fiber on an
 *        rrr::IntEvent until the reply lands.
 *
 * Rusty-safety:
 *  - Polymorphism via TransportBase virtual dispatch.
 *  - Non-owning raw pointer to RaftCommo (@unsafe): RaftCommo lives on
 *    RaftServer, which outlives every adapter built on top of it.
 *    rusty::Arc<RaftCommo> is unusable because Arc<T>::operator-> yields
 *    const access but RaftCommo's Send* methods are non-const.
 *  - The reply-slot + IntEvent pair is allocated via std::make_shared
 *    at the rrr boundary; `RaftCommo::Send*Cb` takes a std::function
 *    that captures it. One @unsafe boundary per method.
 */

#include <cstdint>

#include "transport.hpp"
#include "messages.hpp"

#include "rrr/rrr.hpp"

#include "../constants.h"
#include "commo.h"

namespace janus {
namespace raft {

class RrrTransportAdapter : public TransportBase {
 public:
  // @unsafe { non-owning raw pointer; caller must ensure commo outlives this }
  RrrTransportAdapter(RaftCommo* commo, siteid_t self, parid_t par)
      : commo_(commo), self_(self), par_(par) {}

  // @safe - identity read
  siteid_t self_site_id() const override { return self_; }

  // ------------------------------------------------------------------
  // Fire-and-forget RPCs — forwarded directly.
  // ------------------------------------------------------------------

  // @safe
  void send_vote_durable(siteid_t candidate, VoteDurableReq req) override {
    commo_->SendVoteDurable(candidate, par_, req.term, req.voter_id);
  }
  // @safe
  void send_append_entries_durable(siteid_t leader, AppendEntriesDurableReq req) override {
    commo_->SendAppendEntriesDurable(
        leader, par_, req.term, req.follower_id, req.last_log_index);
  }
  // @safe
  void send_notify_restart(siteid_t self, parid_t par) override {
    commo_->SendNotifyRestart(self, par);
  }

  // ------------------------------------------------------------------
  // Reply-expecting RPCs — fiber-synchronous.
  // Each method registers an IntEvent + reply slot with RaftCommo's
  // callback-shaped Send* variant, then blocks the calling fiber on
  // the event via Wait(). The callback stores the reply into the slot
  // and sets the event, waking the fiber.
  // ------------------------------------------------------------------

  // @unsafe { std::function bridge at rrr boundary }
  AppendEntriesReply send_append_entries(siteid_t dst, AppendEntriesReq req) override {
    auto slot  = std::make_shared<AppendEntriesReply>();
    auto ready = reactor_create_sp_event<IntEvent>();
    commo_->SendAppendEntriesCb(
        dst, par_, req.slot, req.ballot,
        /*isLeader=*/true,
        req.leader_site_id, req.leader_current_term,
        req.leader_prev_log_index, req.leader_prev_log_term,
        req.leader_commit_index, req.cmd, req.leader_next_log_term,
        /*trigger_election_now=*/false,
        [slot, ready](siteid_t, AppendEntriesReply r) {
          *slot = std::move(r);
          ready->set(1);
        });
    ready->wait();  // yields fiber until reply arrives or timeout
    return *slot;
  }

  // @unsafe { std::function bridge at rrr boundary }
  EmptyAppendEntriesReply send_empty_append_entries(siteid_t dst,
                                                    EmptyAppendEntriesReq req) override {
    auto slot  = std::make_shared<AppendEntriesReply>();
    auto ready = reactor_create_sp_event<IntEvent>();
    commo_->SendAppendEntriesCb(
        dst, par_, req.slot, req.ballot,
        /*isLeader=*/true,
        req.leader_site_id, req.leader_current_term,
        req.leader_prev_log_index, req.leader_prev_log_term,
        req.leader_commit_index, janus::Command{}, /*cmdLogTerm=*/0,
        req.trigger_election_now,
        [slot, ready](siteid_t, AppendEntriesReply r) {
          *slot = std::move(r);
          ready->set(1);
        });
    ready->wait();
    EmptyAppendEntriesReply out{};
    out.follower_append_ok = slot->follower_append_ok;
    out.follower_current_term = slot->follower_current_term;
    out.follower_last_log_index = slot->follower_last_log_index;
    out.follower_ack_type = slot->follower_ack_type;
    return out;
  }

  // @unsafe { std::function + shared_ptr bridge at rrr boundary.
  //            BroadcastVoteCb fires the callback once per peer reply;
  //            send_vote is per-peer, so we filter on `from == dst`
  //            and park on a dedicated IntEvent. }
  VoteReply send_vote(siteid_t dst, VoteReq req) override {
    auto slot  = std::make_shared<VoteReply>();
    auto ready = reactor_create_sp_event<IntEvent>();
    commo_->BroadcastVoteCb(
        par_, req.last_log_idx, req.last_log_term,
        req.candidate_site_id, req.current_term,
        [slot, ready, dst](siteid_t from, VoteReply r) {
          if (from == dst) {
            *slot = std::move(r);
            ready->set(1);
          }
        });
    ready->wait();
    return *slot;
  }

  // @unsafe { std::function bridge }
  TimeoutNowReply send_timeout_now(siteid_t dst, TimeoutNowReq req) override {
    auto slot  = std::make_shared<TimeoutNowReply>();
    auto ready = reactor_create_sp_event<IntEvent>();
    commo_->SendTimeoutNow(
        dst, par_, req.leader_term, req.leader_site_id,
        [slot, ready](bool success, uint64_t follower_term) {
          slot->success = success;
          slot->follower_term = follower_term;
          ready->set(1);
        });
    ready->wait();
    return *slot;
  }

  // @unsafe { std::function bridge; SendInstallSnapshot already takes
  //           a std::function — we just wire it into a slot+event. }
  InstallSnapshotReply send_install_snapshot(siteid_t dst, InstallSnapshotReq req) override {
    auto slot  = std::make_shared<InstallSnapshotReply>();
    auto ready = reactor_create_sp_event<IntEvent>();
    commo_->SendInstallSnapshot(
        dst, par_, req.term, req.leader_id,
        req.last_included_index, req.last_included_term, req.data,
        [slot, ready](uint64_t follower_term) {
          slot->term_out = follower_term;
          ready->set(1);
        });
    ready->wait();
    return *slot;
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
  return rusty::make_box<RrrTransportAdapter>(commo, self, par);
}

}  // namespace raft
}  // namespace janus
