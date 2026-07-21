#pragma once

/**
 * @file raft_node.hpp
 * @brief Phase 6 of the decouple plan — single-node facade that owns a
 *        transport, storage, and snapshot-manager and exposes a
 *        DispatcherProxy to the cluster. Intentionally a SKELETON: it
 *        holds the wiring but does not yet drive the full RaftServer
 *        state machine, because RaftServer is still coupled to
 *        rrr::PollThread / rrr::Fiber. That integration is the
 *        remaining Phase 6.5 (deferred in the same spirit as Phase 2.5).
 *
 * What this file provides right now:
 *   - RaftNode type holding:
 *       siteid_t id, TransportProxy, LogStorage&, SnapshotManager&,
 *       a simple DispatcherProxy produced by the node itself.
 *   - Inspection accessors (is_leader, current_term, commit_index) —
 *     placeholder implementations backed by in-node fields so tests
 *     can exercise the cluster plumbing end-to-end.
 *   - A `DummyDispatcher` inner type that satisfies DispatcherBase
 *     by accepting every RPC and firing a vacuous reply. Phase 6.5
 *     replaces it with a real RaftServer-backed dispatcher.
 *
 * The point of keeping this skeleton now is to let Phase 7 wire up
 * raft_lab_standalone without a circular dependency on the RaftServer
 * refactor.
 */

#include <cstdint>
#include <utility>
#include <vector>

#include <rusty/box.hpp>

#include "channel_transport.hpp"
#include "dispatcher.hpp"
#include "log_storage.hpp"
#include "messages.hpp"
#include "snapshot_manager.hpp"
#include "transport.hpp"

#include "../constants.h"

namespace janus {
namespace raft {

// ---------------------------------------------------------------------------
// DummyDispatcher — placeholder that accepts every RPC and responds.
// Phase 6.5 will swap this for a RaftServer-backed dispatcher.
// ---------------------------------------------------------------------------

#if RUSTYCPP_RUST
pub struct DummyDispatcherCore {
    self_: u16,
}

impl DummyDispatcherCore {
    // @safe
    #[cpp_ctor]
    fn new(self_site: u16) -> DummyDispatcherCore {
        DummyDispatcherCore {
            self_: self_site,
        }
    }

    // @safe
    fn handle_vote(&self, req: VoteReq) -> VoteReply {
        VoteReply {
            max_ballot: req.current_term,
            vote_granted: true,
        }
    }

    // @safe
    fn handle_vote_durable(&self, _req: VoteDurableReq) -> VoteDurableReply {
        VoteDurableReply {
            acknowledged: true,
        }
    }

    // @safe
    fn handle_append_entries(&self, _req: AppendEntriesReq) -> AppendEntriesReply {
        AppendEntriesReply {
            follower_append_ok: 1,
            follower_current_term: 0,
            follower_last_log_index: 0,
            follower_ack_type: 0,
        }
    }

    // @safe
    fn handle_empty_append_entries(&self, _req: EmptyAppendEntriesReq)
        -> EmptyAppendEntriesReply {
        EmptyAppendEntriesReply {
            follower_append_ok: 1,
            follower_current_term: 0,
            follower_last_log_index: 0,
            follower_ack_type: 0,
        }
    }

    // @safe
    fn handle_append_entries_durable(&self, _req: AppendEntriesDurableReq)
        -> AppendEntriesDurableReply {
        AppendEntriesDurableReply {
            acknowledged: true,
        }
    }

    // @safe
    fn handle_timeout_now(&self, _req: TimeoutNowReq) -> TimeoutNowReply {
        TimeoutNowReply {
            follower_term: 0,
            success: true,
        }
    }

    // @safe
    fn handle_notify_restart(&self, _req: NotifyRestartReq) -> NotifyRestartReply {
        NotifyRestartReply {
            acknowledged: true,
        }
    }

    // @safe
    fn handle_install_snapshot(&self, _req: InstallSnapshotReq) -> InstallSnapshotReply {
        InstallSnapshotReply {
            term_out: 0,
        }
    }

    // @safe
    fn self_site_id(&self) -> u16 {
        self.self_
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_node.dummy_dispatcher_core version=1 rust_sha256=3d36dd28ed5954a025b28f2a612dd4ee954ed00f4971c22b7e658608594d9e81*/
struct DummyDispatcherCore;

struct DummyDispatcherCore {
    uint16_t self_;

    DummyDispatcherCore(uint16_t self_site);
    VoteReply handle_vote(VoteReq req) const;
    VoteDurableReply handle_vote_durable(VoteDurableReq _req) const;
    AppendEntriesReply handle_append_entries(AppendEntriesReq _req) const;
    EmptyAppendEntriesReply handle_empty_append_entries(EmptyAppendEntriesReq _req) const;
    AppendEntriesDurableReply handle_append_entries_durable(AppendEntriesDurableReq _req) const;
    TimeoutNowReply handle_timeout_now(TimeoutNowReq _req) const;
    NotifyRestartReply handle_notify_restart(NotifyRestartReq _req) const;
    InstallSnapshotReply handle_install_snapshot(InstallSnapshotReq _req) const;
    uint16_t self_site_id() const;
};


DummyDispatcherCore::DummyDispatcherCore(uint16_t self_site)
    : self_(self_site)
{}

VoteReply DummyDispatcherCore::handle_vote(VoteReq req) const {
    return VoteReply{.max_ballot = std::move(req.current_term), .vote_granted = true};
}

VoteDurableReply DummyDispatcherCore::handle_vote_durable(VoteDurableReq _req) const {
    return VoteDurableReply{.acknowledged = true};
}

AppendEntriesReply DummyDispatcherCore::handle_append_entries(AppendEntriesReq _req) const {
    return AppendEntriesReply{.follower_append_ok = 1, .follower_current_term = 0, .follower_last_log_index = 0, .follower_ack_type = 0};
}

EmptyAppendEntriesReply DummyDispatcherCore::handle_empty_append_entries(EmptyAppendEntriesReq _req) const {
    return EmptyAppendEntriesReply{.follower_append_ok = 1, .follower_current_term = 0, .follower_last_log_index = 0, .follower_ack_type = 0};
}

AppendEntriesDurableReply DummyDispatcherCore::handle_append_entries_durable(AppendEntriesDurableReq _req) const {
    return AppendEntriesDurableReply{.acknowledged = true};
}

TimeoutNowReply DummyDispatcherCore::handle_timeout_now(TimeoutNowReq _req) const {
    return TimeoutNowReply{.follower_term = 0, .success = true};
}

NotifyRestartReply DummyDispatcherCore::handle_notify_restart(NotifyRestartReq _req) const {
    return NotifyRestartReply{.acknowledged = true};
}

InstallSnapshotReply DummyDispatcherCore::handle_install_snapshot(InstallSnapshotReq _req) const {
    return InstallSnapshotReply{.term_out = 0};
}

uint16_t DummyDispatcherCore::self_site_id() const {
    return this->self_;
}
/*RUSTYCPP:GEN-END id=raft_node.dummy_dispatcher_core*/

class DummyDispatcher : public DispatcherBase {
 public:
  // @safe
  explicit DummyDispatcher(siteid_t self) : core_(self) {}

  VoteReply handle_vote(VoteReq req) override {
    return core_.handle_vote(std::move(req));
  }
  VoteDurableReply handle_vote_durable(VoteDurableReq req) override {
    return core_.handle_vote_durable(std::move(req));
  }
  AppendEntriesReply handle_append_entries(AppendEntriesReq req) override {
    return core_.handle_append_entries(std::move(req));
  }
  EmptyAppendEntriesReply handle_empty_append_entries(EmptyAppendEntriesReq req) override {
    return core_.handle_empty_append_entries(std::move(req));
  }
  AppendEntriesDurableReply handle_append_entries_durable(AppendEntriesDurableReq req) override {
    return core_.handle_append_entries_durable(std::move(req));
  }
  TimeoutNowReply handle_timeout_now(TimeoutNowReq req) override {
    return core_.handle_timeout_now(std::move(req));
  }
  NotifyRestartReply handle_notify_restart(NotifyRestartReq req) override {
    return core_.handle_notify_restart(std::move(req));
  }
  InstallSnapshotReply handle_install_snapshot(InstallSnapshotReq req) override {
    return core_.handle_install_snapshot(std::move(req));
  }

  siteid_t self_site_id() const { return core_.self_site_id(); }

 private:
  DummyDispatcherCore core_;
};

// ---------------------------------------------------------------------------
// RaftNode — owns transport + storage + dispatcher for one site.
// ---------------------------------------------------------------------------

class RaftNode {
 public:
  // @unsafe { log_storage and snap_manager are non-owning references;
  //           their lifetimes are managed by TestCluster (phase 6) or
  //           by the production wiring (later). }
  RaftNode(siteid_t id,
           TransportProxy transport,
           LogStorage* log_storage,
           SnapshotManager* snap_manager)
      : id_(id),
        transport_(std::move(transport)),
        log_storage_(log_storage),
        snap_manager_(snap_manager),
        dispatcher_(rusty::make_box<DummyDispatcher>(id)) {}

  // @safe
  siteid_t id() const { return id_; }

  // DispatcherProxy is move-only; callers that want to hold onto the
  // dispatcher should take it once and stash it (e.g. in
  // ChannelNodeWorker). Phase 6.5 replaces DummyDispatcher with a
  // real impl.
  // @safe
  DispatcherProxy take_dispatcher() {
    // Build a fresh DummyDispatcher so the node can still keep its own
    // view after handing one out. DummyDispatcher is stateless beyond
    // self_, so a fresh instance is semantically equivalent.
    return rusty::make_box<DummyDispatcher>(id_);
  }

  // Inspection accessors. These are placeholders backed by simple
  // in-node fields so test-cluster plumbing can be exercised; they
  // will be replaced by delegation to a real RaftServer in Phase 6.5.
  // @safe
  bool      is_leader()      const { return is_leader_; }
  slotid_t  commit_index()   const { return commit_index_; }
  ballot_t  current_term()   const { return current_term_; }

  // @safe - manual state injection used by the Phase 6 tests
  void force_leader(bool b)             { is_leader_ = b; }
  void set_commit_index(slotid_t s)     { commit_index_ = s; }
  void set_current_term(ballot_t t)     { current_term_ = t; }

  // @safe - borrow the transport for sending RPCs
  TransportProxy& transport() { return transport_; }

  // @safe
  LogStorage*      log_storage()     { return log_storage_; }
  SnapshotManager* snapshot_manager(){ return snap_manager_; }

 private:
  siteid_t                      id_{0};
  TransportProxy                transport_;
  LogStorage*                   log_storage_{nullptr};
  SnapshotManager*              snap_manager_{nullptr};
  DispatcherProxy               dispatcher_;

  bool     is_leader_{false};
  slotid_t commit_index_{0};
  ballot_t current_term_{0};
};

}  // namespace raft
}  // namespace janus
