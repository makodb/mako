#pragma once

/**
 * @file raft_node.hpp
 * @brief Phase 6 of the decouple plan — single-node facade that owns a
 *        transport and snapshot-manager and exposes a
 *        DispatcherProxy to the cluster. Intentionally a SKELETON: it
 *        holds the wiring but does not yet drive the full RaftServer
 *        state machine, because RaftServer is still coupled to
 *        rrr::PollThread / rrr::Fiber. That integration is the
 *        remaining Phase 6.5 (deferred in the same spirit as Phase 2.5).
 *
 * What this file provides right now:
 *   - RaftNode type holding:
 *       siteid_t id, TransportProxy, SnapshotManager&,
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

class DummyDispatcher : public DispatcherBase {
 public:
  // @safe
  explicit DummyDispatcher(siteid_t self) : self_(self) {}

  VoteReply handle_vote(VoteReq req) override {
    VoteReply r{};
    r.max_ballot   = req.current_term;
    r.vote_granted = true;
    return r;
  }
  AppendEntriesReply handle_append_entries(AppendEntriesReq) override {
    AppendEntriesReply r{}; r.follower_append_ok = 1; return r;
  }
  EmptyAppendEntriesReply handle_empty_append_entries(EmptyAppendEntriesReq) override {
    EmptyAppendEntriesReply r{}; r.follower_append_ok = 1; return r;
  }
  InstallSnapshotReply handle_install_snapshot(InstallSnapshotReq) override {
    InstallSnapshotReply r{}; r.term_out = 0; return r;
  }

  siteid_t self_site_id() const { return self_; }

 private:
  siteid_t self_{0};
};

// ---------------------------------------------------------------------------
// RaftNode — owns transport + snapshot-manager + dispatcher for one site.
// ---------------------------------------------------------------------------

class RaftNode {
 public:
  // @unsafe { snap_manager is a non-owning reference;
  //           its lifetime is managed by TestCluster (phase 6) or
  //           by the production wiring (later). }
  RaftNode(siteid_t id,
           TransportProxy transport,
           SnapshotManager* snap_manager)
      : id_(id),
        transport_(std::move(transport)),
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
  SnapshotManager* snapshot_manager(){ return snap_manager_; }

 private:
  siteid_t                      id_{0};
  TransportProxy                transport_;
  SnapshotManager*              snap_manager_{nullptr};
  DispatcherProxy               dispatcher_;

  bool     is_leader_{false};
  slotid_t commit_index_{0};
  ballot_t current_term_{0};
};

}  // namespace raft
}  // namespace janus
