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
 *       and a RaftServer-backed DispatcherProxy produced by the node.
 *   - Inspection accessors (is_leader, current_term, commit_index) —
 *     placeholder implementations backed by in-node fields so tests
 *     can exercise the cluster plumbing end-to-end.
 *   - A `DummyDispatcher` compatibility type kept while Phase 8.5
 *     migration is in progress.
 *
 * The point of keeping this skeleton now is to let Phase 7 wire up
 * raft_lab_standalone without a circular dependency on the RaftServer
 * refactor.
 */

#include <cstdint>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/option.hpp>

#include "channel_transport.hpp"
#include "dispatcher.hpp"
#include "log_storage.hpp"
#include "messages.hpp"
#include "raft_server_dispatcher.hpp"
#include "snapshot_manager.hpp"
#include "transport.hpp"

#include "../constants.h"

namespace janus {

class RaftServer;

namespace raft {

// ---------------------------------------------------------------------------
// DummyDispatcher — placeholder that accepts every RPC and responds.
// Phase 6.5 will swap this for a RaftServer-backed dispatcher.
// ---------------------------------------------------------------------------

class DummyDispatcher {
 public:
  // @safe
  DummyDispatcher(siteid_t self) : self_(self) {}

  VoteReply handle_vote(VoteReq req) {
    VoteReply r{};
    r.max_ballot   = req.current_term;
    r.vote_granted = true;
    return r;
  }
  VoteDurableReply handle_vote_durable(VoteDurableReq) {
    VoteDurableReply r{}; r.acknowledged = true; return r;
  }
  AppendEntriesReply handle_append_entries(AppendEntriesReq) {
    AppendEntriesReply r{}; r.follower_append_ok = 1; return r;
  }
  EmptyAppendEntriesReply handle_empty_append_entries(EmptyAppendEntriesReq) {
    EmptyAppendEntriesReply r{}; r.follower_append_ok = 1; return r;
  }
  AppendEntriesDurableReply handle_append_entries_durable(AppendEntriesDurableReq) {
    AppendEntriesDurableReply r{}; r.acknowledged = true; return r;
  }
  TimeoutNowReply handle_timeout_now(TimeoutNowReq) {
    TimeoutNowReply r{}; r.success = true; return r;
  }
  NotifyRestartReply handle_notify_restart(NotifyRestartReq) {
    NotifyRestartReply r{}; r.acknowledged = true; return r;
  }
  InstallSnapshotReply handle_install_snapshot(InstallSnapshotReq) {
    InstallSnapshotReply r{}; r.term_out = 0; return r;
  }
  AddServerReply handle_add_server(AddServerReq) {
    AddServerReply r{};
    r.success = true;
    r.error_msg = "";
    r.leader_hint = self_;
    return r;
  }
  RemoveServerReply handle_remove_server(RemoveServerReq) {
    RemoveServerReply r{};
    r.success = true;
    r.error_msg = "";
    r.leader_hint = self_;
    return r;
  }

  siteid_t self_site_id() const { return self_; }

 private:
  siteid_t self_{0};
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
           SnapshotManager* snap_manager,
           ChannelSwitchboard* transport_sw,
           parid_t partition_id,
           const std::vector<siteid_t>& cluster_sites)
      : id_(id),
        partition_id_(partition_id),
        transport_(std::move(transport)),
        log_storage_(log_storage),
        snap_manager_(snap_manager),
        server_(rusty::None),
        dispatcher_(make_raft_server_dispatcher(nullptr)) {
    // @unsafe { test harness owns storage/snapshot; server stores
    //           non-owning shared_ptr aliases. }
    auto* raw_server = new ::janus::RaftServer(
        id_, partition_id_, static_cast<locid_t>(id_));
    server_ = rusty::Some(rusty::Box<::janus::RaftServer>(raw_server));

    if (log_storage_ != nullptr) {
      raw_server->SetLogStorage(
          std::shared_ptr<LogStorage>(log_storage_, [](LogStorage*) {}));
    }
    if (snap_manager_ != nullptr) {
      raw_server->SetSnapshotManager(std::shared_ptr<SnapshotManager>(
          snap_manager_, [](SnapshotManager*) {}));
    }
    if (transport_sw != nullptr) {
      raw_server->transport() =
          make_channel_transport(transport_sw, id_, partition_id_);
    }
    raw_server->BootstrapCurrentConfigForTest(
        std::set<siteid_t>(cluster_sites.begin(), cluster_sites.end()));
    dispatcher_ = make_raft_server_dispatcher(raw_server);
  }

  // @safe
  siteid_t id() const { return id_; }

  // DispatcherProxy is move-only; callers that want to hold onto the
  // dispatcher should take it once and stash it (e.g. in
  // ChannelNodeWorker).
  // @safe
  DispatcherProxy take_dispatcher() {
    return make_raft_server_dispatcher(server());
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
  ::janus::RaftServer* server() {
    if (server_.is_none()) return nullptr;
    return server_.as_ref().unwrap().get();
  }
  // @safe
  const ::janus::RaftServer* server() const {
    if (server_.is_none()) return nullptr;
    return server_.as_ref().unwrap().get();
  }
  // @safe
  bool has_server() const { return server_.is_some(); }

  // @safe
  LogStorage*      log_storage()     { return log_storage_; }
  SnapshotManager* snapshot_manager(){ return snap_manager_; }
  // @safe
  size_t server_config_size() const {
    if (server_.is_none()) return 0;
    return server_.as_ref().unwrap().get()->GetCurrentConfig().size();
  }
  // @safe
  bool server_config_contains(siteid_t site) const {
    if (server_.is_none()) return false;
    const auto& cfg = server_.as_ref().unwrap().get()->GetCurrentConfig();
    return cfg.count(site) > 0;
  }

 private:
  siteid_t                      id_{0};
  parid_t                       partition_id_{0};
  TransportProxy                transport_;
  LogStorage*                   log_storage_{nullptr};
  SnapshotManager*              snap_manager_{nullptr};
  rusty::Option<rusty::Box<::janus::RaftServer>> server_;
  DispatcherProxy               dispatcher_;

  bool     is_leader_{false};
  slotid_t commit_index_{0};
  ballot_t current_term_{0};
};

}  // namespace raft
}  // namespace janus
