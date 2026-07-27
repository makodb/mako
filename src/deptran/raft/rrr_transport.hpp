#pragma once

/**
 * @file rrr_transport.hpp
 * @brief Production TransportBase adapter that wraps RaftCommo /
 *        RaftProxy. Reply-expecting send_* calls block the calling
 *        fiber on an rrr::IntEvent until the reply lands.
 *
 * Rusty-safety:
 *  - Polymorphism via TransportBase virtual dispatch.
 *  - Non-owning raw pointer to RaftCommo (@unsafe): RaftCommo lives on
 *    RaftServer, which outlives every adapter built on top of it.
 *    rusty::Arc<RaftCommo> is unusable because Arc<T>::operator-> yields
 *    const access but RaftCommo's Send* methods are non-const.
 *  - The reply-slot + IntEvent pair is allocated via std::make_shared
 *    at the rrr boundary. `RaftCommo::Send*Cb` takes rusty::Function,
 *    and each call builds a one-shot lambda that captures the shared
 *    reply slot and wakeup event. One @unsafe boundary per method.
 */

#include <cstdint>
#include <memory>
#include <utility>

#include "transport.hpp"
#include "messages.hpp"

#include "rrr/rrr.hpp"

#include "../constants.h"
#include "commo.h"

namespace janus {
namespace raft {

// @safe - thin wrapper; unsafe work is isolated to the RaftCommo RPC call.
inline void rrr_transport_send_vote_durable_cpp(RaftCommo* commo,
                                                siteid_t candidate,
                                                parid_t par,
                                                VoteDurableReq req) {
  // @unsafe - enters RaftCommo rrr/RaftProxy RPC boundary.
  {
    commo->SendVoteDurable(candidate, par, req.term, req.voter_id);
  }
}

// @safe - thin wrapper; unsafe work is isolated to the RaftCommo RPC call.
inline void rrr_transport_send_append_entries_durable_cpp(
    RaftCommo* commo,
    siteid_t leader,
    parid_t par,
    AppendEntriesDurableReq req) {
  // @unsafe - enters RaftCommo rrr/RaftProxy RPC boundary.
  {
    commo->SendAppendEntriesDurable(
        leader, par, req.term, req.follower_id, req.last_log_index);
  }
}

// @safe - thin wrapper; unsafe work is isolated to the RaftCommo RPC call.
inline void rrr_transport_send_notify_restart_cpp(RaftCommo* commo,
                                                  siteid_t self,
                                                  parid_t par) {
  // @unsafe - enters RaftCommo rrr/RaftProxy RPC boundary.
  {
    commo->SendNotifyRestart(self, par);
  }
}

// @unsafe - bridges synchronous TransportBase API to RaftCommo's async
// rusty::Function callback API using shared reply slot + IntEvent.
inline AppendEntriesReply rrr_transport_send_append_entries_cpp(
    RaftCommo* commo,
    siteid_t dst,
    parid_t par,
    AppendEntriesReq req) {
  auto slot  = std::make_shared<AppendEntriesReply>();
  auto ready = Reactor::create_sp_event<IntEvent>();
  commo->SendAppendEntriesCb(
      dst, par, req.slot, req.ballot,
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

// @unsafe - bridges synchronous TransportBase API to RaftCommo's async
// rusty::Function callback API using shared reply slot + IntEvent.
inline EmptyAppendEntriesReply rrr_transport_send_empty_append_entries_cpp(
    RaftCommo* commo,
    siteid_t dst,
    parid_t par,
    EmptyAppendEntriesReq req) {
  auto slot  = std::make_shared<AppendEntriesReply>();
  auto ready = Reactor::create_sp_event<IntEvent>();
  commo->SendAppendEntriesCb(
      dst, par, req.slot, req.ballot,
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

// @unsafe - bridges per-peer send_vote onto BroadcastVoteCb fanout.
// BroadcastVoteCb uses rusty::Function and may fire once per peer reply;
// this adapter filters on `from == dst` and wakes this call's IntEvent.
inline VoteReply rrr_transport_send_vote_cpp(RaftCommo* commo,
                                             siteid_t dst,
                                             parid_t par,
                                             VoteReq req) {
  auto slot  = std::make_shared<VoteReply>();
  auto ready = Reactor::create_sp_event<IntEvent>();
  commo->BroadcastVoteCb(
      par, req.last_log_idx, req.last_log_term,
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

// @unsafe - bridges synchronous TransportBase API to SendTimeoutNow's
// rusty::Function callback using shared reply slot + IntEvent.
inline TimeoutNowReply rrr_transport_send_timeout_now_cpp(RaftCommo* commo,
                                                          siteid_t dst,
                                                          parid_t par,
                                                          TimeoutNowReq req) {
  auto slot  = std::make_shared<TimeoutNowReply>();
  auto ready = Reactor::create_sp_event<IntEvent>();
  commo->SendTimeoutNow(
      dst, par, req.leader_term, req.leader_site_id,
      [slot, ready](bool success, uint64_t follower_term) {
        slot->success = success;
        slot->follower_term = follower_term;
        ready->set(1);
      });
  ready->wait();
  return *slot;
}

// @unsafe - bridges synchronous TransportBase API to SendInstallSnapshot's
// rusty::Function callback using shared reply slot + IntEvent.
inline InstallSnapshotReply rrr_transport_send_install_snapshot_cpp(
    RaftCommo* commo,
    siteid_t dst,
    parid_t par,
    InstallSnapshotReq req) {
  auto slot  = std::make_shared<InstallSnapshotReply>();
  auto ready = Reactor::create_sp_event<IntEvent>();
  commo->SendInstallSnapshot(
      dst, par, req.term, req.leader_id,
      req.last_included_index, req.last_included_term, req.data,
      [slot, ready](uint64_t follower_term) {
        slot->term_out = follower_term;
        ready->set(1);
      });
  ready->wait();
  return *slot;
}

#if RUSTYCPP_RUST
pub struct RrrTransportAdapterCore {
    commo_: *mut RaftCommo,
    self_: u16,
    par_: u32,
}

impl RrrTransportAdapterCore {
    // @unsafe - Stores a non-owning RaftCommo pointer.
    fn new(commo: *mut RaftCommo, self_site: u16, par: u32)
        -> RrrTransportAdapterCore {
        RrrTransportAdapterCore {
            commo_: commo,
            self_: self_site,
            par_: par,
        }
    }

    // @safe
    fn self_site_id(&self) -> u16 {
        self.self_
    }

    // @unsafe - Delegates to RaftCommo RPC boundary.
    fn send_vote_durable(&mut self, candidate: u16, req: VoteDurableReq) {
        unsafe {
            rrr_transport_send_vote_durable_cpp(self.commo_, candidate,
                                                self.par_, req)
        }
    }

    // @unsafe - Delegates to RaftCommo RPC boundary.
    fn send_append_entries_durable(&mut self, leader: u16,
                                   req: AppendEntriesDurableReq) {
        unsafe {
            rrr_transport_send_append_entries_durable_cpp(self.commo_, leader,
                                                          self.par_, req)
        }
    }

    // @unsafe - Delegates to RaftCommo RPC boundary.
    fn send_notify_restart(&mut self, self_site: u16, par: u32) {
        unsafe {
            rrr_transport_send_notify_restart_cpp(self.commo_, self_site, par)
        }
    }

    // @unsafe - Delegates to RaftCommo RPC boundary.
    fn send_append_entries(&mut self, dst: u16, req: AppendEntriesReq)
        -> AppendEntriesReply {
        unsafe {
            rrr_transport_send_append_entries_cpp(self.commo_, dst,
                                                  self.par_, req)
        }
    }

    // @unsafe - Delegates to RaftCommo RPC boundary.
    fn send_empty_append_entries(&mut self, dst: u16, req: EmptyAppendEntriesReq)
        -> EmptyAppendEntriesReply {
        unsafe {
            rrr_transport_send_empty_append_entries_cpp(self.commo_, dst,
                                                        self.par_, req)
        }
    }

    // @unsafe - Delegates to RaftCommo RPC boundary.
    fn send_vote(&mut self, dst: u16, req: VoteReq) -> VoteReply {
        unsafe {
            rrr_transport_send_vote_cpp(self.commo_, dst, self.par_, req)
        }
    }

    // @unsafe - Delegates to RaftCommo RPC boundary.
    fn send_timeout_now(&mut self, dst: u16, req: TimeoutNowReq) -> TimeoutNowReply {
        unsafe {
            rrr_transport_send_timeout_now_cpp(self.commo_, dst, self.par_, req)
        }
    }

    // @unsafe - Delegates to RaftCommo RPC boundary.
    fn send_install_snapshot(&mut self, dst: u16, req: InstallSnapshotReq)
        -> InstallSnapshotReply {
        unsafe {
            rrr_transport_send_install_snapshot_cpp(self.commo_, dst,
                                                    self.par_, req)
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=rrr_transport.1 version=1 rust_sha256=c11260f640c4ccb4bd7f8470b94a49df92596da2b29176b98ebd94bf7c4c48e8*/
struct RrrTransportAdapterCore;

struct RrrTransportAdapterCore {
    RaftCommo* commo_;
    uint16_t self_;
    uint32_t par_;

    static RrrTransportAdapterCore new_(RaftCommo* commo, uint16_t self_site, uint32_t par);
    uint16_t self_site_id() const;
    void send_vote_durable(uint16_t candidate, VoteDurableReq req);
    void send_append_entries_durable(uint16_t leader, AppendEntriesDurableReq req);
    void send_notify_restart(uint16_t self_site, uint32_t par);
    AppendEntriesReply send_append_entries(uint16_t dst, AppendEntriesReq req);
    EmptyAppendEntriesReply send_empty_append_entries(uint16_t dst, EmptyAppendEntriesReq req);
    VoteReply send_vote(uint16_t dst, VoteReq req);
    TimeoutNowReply send_timeout_now(uint16_t dst, TimeoutNowReq req);
    InstallSnapshotReply send_install_snapshot(uint16_t dst, InstallSnapshotReq req);
};


inline RrrTransportAdapterCore RrrTransportAdapterCore::new_(RaftCommo* commo, uint16_t self_site, uint32_t par) {
    return RrrTransportAdapterCore{.commo_ = commo, .self_ = std::move(self_site), .par_ = std::move(par)};
}

inline uint16_t RrrTransportAdapterCore::self_site_id() const {
    return this->self_;
}

inline void RrrTransportAdapterCore::send_vote_durable(uint16_t candidate, VoteDurableReq req) {
    // @unsafe
    {
        rrr_transport_send_vote_durable_cpp(this->commo_, std::move(candidate), this->par_, std::move(req));
    }
}

inline void RrrTransportAdapterCore::send_append_entries_durable(uint16_t leader, AppendEntriesDurableReq req) {
    // @unsafe
    {
        rrr_transport_send_append_entries_durable_cpp(this->commo_, std::move(leader), this->par_, std::move(req));
    }
}

inline void RrrTransportAdapterCore::send_notify_restart(uint16_t self_site, uint32_t par) {
    // @unsafe
    {
        rrr_transport_send_notify_restart_cpp(this->commo_, std::move(self_site), std::move(par));
    }
}

inline AppendEntriesReply RrrTransportAdapterCore::send_append_entries(uint16_t dst, AppendEntriesReq req) {
    // @unsafe
    {
        return rrr_transport_send_append_entries_cpp(this->commo_, std::move(dst), this->par_, std::move(req));
    }
}

inline EmptyAppendEntriesReply RrrTransportAdapterCore::send_empty_append_entries(uint16_t dst, EmptyAppendEntriesReq req) {
    // @unsafe
    {
        return rrr_transport_send_empty_append_entries_cpp(this->commo_, std::move(dst), this->par_, std::move(req));
    }
}

inline VoteReply RrrTransportAdapterCore::send_vote(uint16_t dst, VoteReq req) {
    // @unsafe
    {
        return rrr_transport_send_vote_cpp(this->commo_, std::move(dst), this->par_, std::move(req));
    }
}

inline TimeoutNowReply RrrTransportAdapterCore::send_timeout_now(uint16_t dst, TimeoutNowReq req) {
    // @unsafe
    {
        return rrr_transport_send_timeout_now_cpp(this->commo_, std::move(dst), this->par_, std::move(req));
    }
}

inline InstallSnapshotReply RrrTransportAdapterCore::send_install_snapshot(uint16_t dst, InstallSnapshotReq req) {
    // @unsafe
    {
        return rrr_transport_send_install_snapshot_cpp(this->commo_, std::move(dst), this->par_, std::move(req));
    }
}
/*RUSTYCPP:GEN-END id=rrr_transport.1*/

class RrrTransportAdapter : public TransportBase {
 public:
  // @unsafe { non-owning raw pointer; caller must ensure commo outlives this }
  RrrTransportAdapter(RaftCommo* commo, siteid_t self, parid_t par)
      : core_(RrrTransportAdapterCore::new_(commo, self, par)) {}

  // @safe - identity read
  siteid_t self_site_id() const override { return core_.self_site_id(); }

  // ------------------------------------------------------------------
  // Fire-and-forget RPCs — forwarded directly.
  // ------------------------------------------------------------------

  // @safe - thin wrapper; unsafe work is isolated to the RaftCommo RPC call.
  void send_vote_durable(siteid_t candidate, VoteDurableReq req) override {
    core_.send_vote_durable(candidate, std::move(req));
  }

  // @safe - thin wrapper; unsafe work is isolated to the RaftCommo RPC call.
  void send_append_entries_durable(siteid_t leader, AppendEntriesDurableReq req) override {
    core_.send_append_entries_durable(leader, std::move(req));
  }

  // @safe - thin wrapper; unsafe work is isolated to the RaftCommo RPC call.
  void send_notify_restart(siteid_t self, parid_t par) override {
    core_.send_notify_restart(self, par);
  }
  // ------------------------------------------------------------------
  // Reply-expecting RPCs — fiber-synchronous.
  // Each method registers an IntEvent + reply slot with RaftCommo's
  // callback-shaped Send* variant, then blocks the calling fiber on
  // the event via Wait(). The callback stores the reply into the slot
  // and sets the event, waking the fiber.
  // ------------------------------------------------------------------

  // @unsafe - bridges synchronous TransportBase API to RaftCommo's async
  // rusty::Function callback API using shared reply slot + IntEvent.
  AppendEntriesReply send_append_entries(siteid_t dst, AppendEntriesReq req) override {
    return core_.send_append_entries(dst, std::move(req));
  }

  // @unsafe - bridges synchronous TransportBase API to RaftCommo's async
  // rusty::Function callback API using shared reply slot + IntEvent.
  EmptyAppendEntriesReply send_empty_append_entries(siteid_t dst,
                                                    EmptyAppendEntriesReq req) override {
    return core_.send_empty_append_entries(dst, std::move(req));
  }

  // @unsafe - bridges per-peer send_vote onto BroadcastVoteCb fanout.
  // BroadcastVoteCb uses rusty::Function and may fire once per peer reply;
  // this adapter filters on `from == dst` and wakes this call's IntEvent.
  VoteReply send_vote(siteid_t dst, VoteReq req) override {
    return core_.send_vote(dst, std::move(req));
  }

  // @unsafe - bridges synchronous TransportBase API to SendTimeoutNow's
  // rusty::Function callback using shared reply slot + IntEvent.
  TimeoutNowReply send_timeout_now(siteid_t dst, TimeoutNowReq req) override {
    return core_.send_timeout_now(dst, std::move(req));
  }

  // @unsafe - bridges synchronous TransportBase API to SendInstallSnapshot's
  // rusty::Function callback using shared reply slot + IntEvent.
  InstallSnapshotReply send_install_snapshot(siteid_t dst, InstallSnapshotReq req) override {
    return core_.send_install_snapshot(dst, std::move(req));
  }

 private:
  RrrTransportAdapterCore core_;
};

// @unsafe { factory takes a non-owning RaftCommo* }
inline TransportProxy make_rrr_transport(RaftCommo* commo,
                                         siteid_t  self,
                                         parid_t   par) {
  return rusty::make_box<RrrTransportAdapter>(commo, self, par);
}

}  // namespace raft
}  // namespace janus
