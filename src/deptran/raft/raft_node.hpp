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
#include <vector>

#include <rusty/box.hpp>
#include <rusty/move.hpp>

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
/*RUSTYCPP:GEN-BEGIN id=raft_node.dummy_dispatcher_core version=1 rust_sha256=007abf3dd95e2a16974e50914d9863580efa2a33a3b950c795288d0396af58f2*/
struct DummyDispatcherCore;

struct DummyDispatcherCore {
    uint16_t self_;

    static DummyDispatcherCore new_(uint16_t self_site);
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


inline DummyDispatcherCore DummyDispatcherCore::new_(uint16_t self_site) {
    return DummyDispatcherCore{.self_ = std::move(self_site)};
}

inline VoteReply DummyDispatcherCore::handle_vote(VoteReq req) const {
    return VoteReply{.max_ballot = std::move(req.current_term), .vote_granted = true};
}

inline VoteDurableReply DummyDispatcherCore::handle_vote_durable(VoteDurableReq _req) const {
    return VoteDurableReply{.acknowledged = true};
}

inline AppendEntriesReply DummyDispatcherCore::handle_append_entries(AppendEntriesReq _req) const {
    return AppendEntriesReply{.follower_append_ok = 1, .follower_current_term = 0, .follower_last_log_index = 0, .follower_ack_type = 0};
}

inline EmptyAppendEntriesReply DummyDispatcherCore::handle_empty_append_entries(EmptyAppendEntriesReq _req) const {
    return EmptyAppendEntriesReply{.follower_append_ok = 1, .follower_current_term = 0, .follower_last_log_index = 0, .follower_ack_type = 0};
}

inline AppendEntriesDurableReply DummyDispatcherCore::handle_append_entries_durable(AppendEntriesDurableReq _req) const {
    return AppendEntriesDurableReply{.acknowledged = true};
}

inline TimeoutNowReply DummyDispatcherCore::handle_timeout_now(TimeoutNowReq _req) const {
    return TimeoutNowReply{.follower_term = 0, .success = true};
}

inline NotifyRestartReply DummyDispatcherCore::handle_notify_restart(NotifyRestartReq _req) const {
    return NotifyRestartReply{.acknowledged = true};
}

inline InstallSnapshotReply DummyDispatcherCore::handle_install_snapshot(InstallSnapshotReq _req) const {
    return InstallSnapshotReply{.term_out = 0};
}

inline uint16_t DummyDispatcherCore::self_site_id() const {
    return this->self_;
}
/*RUSTYCPP:GEN-END id=raft_node.dummy_dispatcher_core*/

class DummyDispatcher : public DispatcherBase {
 public:
  // @safe
  explicit DummyDispatcher(siteid_t self)
      : core_(DummyDispatcherCore::new_(self)) {}

  VoteReply handle_vote(VoteReq req) override {
    return core_.handle_vote(rusty::move(req));
  }
  VoteDurableReply handle_vote_durable(VoteDurableReq req) override {
    return core_.handle_vote_durable(rusty::move(req));
  }
  AppendEntriesReply handle_append_entries(AppendEntriesReq req) override {
    return core_.handle_append_entries(rusty::move(req));
  }
  EmptyAppendEntriesReply handle_empty_append_entries(EmptyAppendEntriesReq req) override {
    return core_.handle_empty_append_entries(rusty::move(req));
  }
  AppendEntriesDurableReply handle_append_entries_durable(AppendEntriesDurableReq req) override {
    return core_.handle_append_entries_durable(rusty::move(req));
  }
  TimeoutNowReply handle_timeout_now(TimeoutNowReq req) override {
    return core_.handle_timeout_now(rusty::move(req));
  }
  NotifyRestartReply handle_notify_restart(NotifyRestartReq req) override {
    return core_.handle_notify_restart(rusty::move(req));
  }
  InstallSnapshotReply handle_install_snapshot(InstallSnapshotReq req) override {
    return core_.handle_install_snapshot(rusty::move(req));
  }

  siteid_t self_site_id() const { return core_.self_site_id(); }

 private:
  DummyDispatcherCore core_;
};

#if RUSTYCPP_RUST
pub struct RaftNodeStateCore {
    id_: u16,
    is_leader_: rusty::Cell<bool>,
    commit_index_: rusty::Cell<u64>,
    current_term_: rusty::Cell<u64>,
}

impl RaftNodeStateCore {
    // @safe
    fn new(id: u16) -> RaftNodeStateCore {
        RaftNodeStateCore {
            id_: id,
            is_leader_: rusty::Cell::<bool>::new_(false),
            commit_index_: rusty::Cell::<u64>::new_(0),
            current_term_: rusty::Cell::<u64>::new_(0),
        }
    }

    // @safe
    fn id(&self) -> u16 {
        self.id_
    }

    // @safe
    fn is_leader(&self) -> bool {
        self.is_leader_.get()
    }

    // @safe
    fn set_is_leader(&mut self, value: bool) {
        self.is_leader_.set(value)
    }

    // @safe
    fn commit_index(&self) -> u64 {
        self.commit_index_.get()
    }

    // @safe
    fn set_commit_index(&mut self, value: u64) {
        self.commit_index_.set(value)
    }

    // @safe
    fn current_term(&self) -> u64 {
        self.current_term_.get()
    }

    // @safe
    fn set_current_term(&mut self, value: u64) {
        self.current_term_.set(value)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_node.2 version=1 rust_sha256=a17dd16282342a82fc08decde755f495313988dc342d12136327ed2826fdad07*/
struct RaftNodeStateCore;

struct RaftNodeStateCore {
    uint16_t id_;
    rusty::Cell<bool> is_leader_;
    rusty::Cell<uint64_t> commit_index_;
    rusty::Cell<uint64_t> current_term_;

    static RaftNodeStateCore new_(uint16_t id);
    uint16_t id() const;
    bool is_leader() const;
    void set_is_leader(bool value);
    uint64_t commit_index() const;
    void set_commit_index(uint64_t value);
    uint64_t current_term() const;
    void set_current_term(uint64_t value);
};


inline RaftNodeStateCore RaftNodeStateCore::new_(uint16_t id) {
    return RaftNodeStateCore{.id_ = std::move(id), .is_leader_ = rusty::Cell<bool>::new_(false), .commit_index_ = rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)), .current_term_ = rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0))};
}

inline uint16_t RaftNodeStateCore::id() const {
    return this->id_;
}

inline bool RaftNodeStateCore::is_leader() const {
    return this->is_leader_.get();
}

inline void RaftNodeStateCore::set_is_leader(bool value) {
    this->is_leader_.set(std::move(value));
}

inline uint64_t RaftNodeStateCore::commit_index() const {
    return this->commit_index_.get();
}

inline void RaftNodeStateCore::set_commit_index(uint64_t value) {
    this->commit_index_.set(std::move(value));
}

inline uint64_t RaftNodeStateCore::current_term() const {
    return this->current_term_.get();
}

inline void RaftNodeStateCore::set_current_term(uint64_t value) {
    this->current_term_.set(std::move(value));
}
/*RUSTYCPP:GEN-END id=raft_node.2*/
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
      : state_core_(RaftNodeStateCore::new_(id)),
        transport_(rusty::move(transport)),
        log_storage_(log_storage),
        snap_manager_(snap_manager),
        dispatcher_(rusty::make_box<DummyDispatcher>(id)) {}

  // @safe
  siteid_t id() const { return state_core_.id(); }

  // DispatcherProxy is move-only; callers that want to hold onto the
  // dispatcher should take it once and stash it (e.g. in
  // ChannelNodeWorker). Phase 6.5 replaces DummyDispatcher with a
  // real impl.
  // @safe
  DispatcherProxy take_dispatcher() {
    // Build a fresh DummyDispatcher so the node can still keep its own
    // view after handing one out. DummyDispatcher is stateless beyond
    // self_, so a fresh instance is semantically equivalent.
    return rusty::make_box<DummyDispatcher>(state_core_.id());
  }

  // Inspection accessors. These are placeholders backed by simple
  // in-node fields so test-cluster plumbing can be exercised; they
  // will be replaced by delegation to a real RaftServer in Phase 6.5.
  // @safe
  bool      is_leader()      const { return state_core_.is_leader(); }
  slotid_t  commit_index()   const { return state_core_.commit_index(); }
  ballot_t  current_term()   const { return state_core_.current_term(); }

  // @safe - manual state injection used by the Phase 6 tests
  void force_leader(bool b)             { state_core_.set_is_leader(b); }
  void set_commit_index(slotid_t s)     { state_core_.set_commit_index(s); }
  void set_current_term(ballot_t t)     { state_core_.set_current_term(t); }

  // @safe - borrow the transport for sending RPCs
  TransportProxy& transport() { return transport_; }

  // @safe
  LogStorage*      log_storage()     { return log_storage_; }
  SnapshotManager* snapshot_manager(){ return snap_manager_; }

 private:
  RaftNodeStateCore             state_core_;
  TransportProxy                transport_;
  LogStorage*                   log_storage_{nullptr};
  SnapshotManager*              snap_manager_{nullptr};
  DispatcherProxy               dispatcher_;
};

}  // namespace raft
}  // namespace janus
