#pragma once

/**
 * @file raft_node.hpp
 * @brief In-process single-node facade used by TestCluster. Owns transport,
 *        storage/snapshot backends, and a real frame-less RaftServer and
 *        exposes a DispatcherProxy for in-memory RPC routing.
 *
 * What this file provides right now:
 *   - RaftNode type holding:
 *       siteid_t id, TransportProxy, LogStorage&, SnapshotManager&,
 *       and a RaftServer-backed DispatcherProxy produced by the node.
 *   - Inspection accessors delegated directly to owned server state.
 *   - Test-only mutators for harness control.
 *
 * Runtime loops (heartbeat/election/apply) are started by TestCluster on
 * per-node poll threads via RaftServer test-runtime entrypoints.
 */

#include <cstdint>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <rusty/box.hpp>
#include <rusty/option.hpp>

#include "channel_transport.hpp"
#include "dispatcher.hpp"
#include "log_storage.hpp"
#include "messages.hpp"
#include "raft_server_dispatcher.hpp"
#include "snapshot_manager.hpp"
#include "storage_proxy_wiring.hpp"
#include "transport.hpp"

#include "../constants.h"

namespace janus {

class RaftServer;

namespace raft {

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
        server_(rusty::None) {
    // @unsafe { test harness owns storage/snapshot; server stores
    //           non-owning shared_ptr aliases. }
    auto* raw_server = new ::janus::RaftServer(
        id_, partition_id_, static_cast<locid_t>(id_));
    server_ = rusty::Some(rusty::Box<::janus::RaftServer>(raw_server));

    if (log_storage_ != nullptr) {
      raw_server->SetLogStorage(make_non_owning_log_storage(log_storage_));
    }
    if (snap_manager_ != nullptr) {
      raw_server->SetSnapshotManager(
          make_non_owning_snapshot_manager(snap_manager_));
    }
    if (transport_sw != nullptr) {
      raw_server->transport() =
          make_channel_transport(transport_sw, id_, partition_id_);
    }
    raw_server->BootstrapCurrentConfigForTest(
        std::set<siteid_t>(cluster_sites.begin(), cluster_sites.end()));
    // Keep in-process harness timing aligned with RaftLab defaults even when
    // core objects are compiled in non-RAFT_TEST_CORO mode.
    raw_server->SetHeartbeatInterval(100000);
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
  // delegated reads from the owned RaftServer.
  // @safe
  bool      is_leader() {
    auto* s = server();
    if (s == nullptr) return false;
    return s->IsLeader();
  }
  // @safe
  slotid_t  commit_index() const {
    auto* s = server();
    if (s == nullptr) return 0;
    return s->commitIndex;
  }
  // @safe
  ballot_t  current_term() const {
    auto* s = server();
    if (s == nullptr) return 0;
    return s->currentTerm;
  }

  // @safe - test-only state injection for harness checks
  void force_leader(bool b) {
    auto* s = server();
    if (s != nullptr) s->setIsLeader(b);
  }
  // @safe
  void set_commit_index(slotid_t s) {
    auto* svr = server();
    if (svr != nullptr) svr->commitIndex = s;
  }
  // @safe
  void set_current_term(ballot_t t) {
    auto* svr = server();
    if (svr != nullptr) svr->currentTerm = t;
  }

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
};

}  // namespace raft
}  // namespace janus
