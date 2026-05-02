#pragma once

/**
 * @file test_cluster.hpp
 * @brief Phase 6 — in-process raft cluster harness. Wires N RaftNodes
 *        together through a ChannelSwitchboard and exposes the fault-
 *        injection controls the lab tests need (kill / restart /
 *        disconnect / partition).
 *
 * Current scope: full in-process wiring for real frame-less RaftServer
 * nodes:
 *   - in-memory transport + per-node dispatcher workers
 *   - per-node poll thread runtime startup for heartbeat/election/apply
 *   - fault injection controls (disconnect/partition/reset)
 * Behavioral coverage continues to expand in Phase 8.5 test leaves.
 */

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <utility>
#include <vector>

#include <rusty/arc.hpp>
#include <rusty/box.hpp>

#include "rrr/rrr.hpp"
#include "channel_transport.hpp"
#include "memory_log_storage.hpp"
#include "memory_snapshot_manager.hpp"
#include "raft_node.hpp"

#include "../constants.h"

namespace janus {
namespace raft {

class TestCluster {
 public:
  // @safe - builds an N-site cluster wired through an internal
  // ChannelSwitchboard. Each site gets its own InMemoryLogStorage +
  // MemorySnapshotManager. Site IDs are 1..N.
  // @unsafe { direct `new` because Box::make() requires a copy }
  static rusty::Box<TestCluster> with_in_memory_transport(size_t n) {
    rusty::Box<TestCluster> c(new TestCluster());
    c->build(n);
    return c;
  }

  // @safe - accessors
  size_t size() const { return nodes_.size(); }
  RaftNode* node_or_null(siteid_t id) {
    for (auto& n : nodes_) {
      if (n->id() == id) return n.get();
    }
    return nullptr;
  }
  const RaftNode* node_or_null(siteid_t id) const {
    for (const auto& n : nodes_) {
      if (n->id() == id) return n.get();
    }
    return nullptr;
  }
  RaftNode& node(siteid_t id) {
    auto* n = node_or_null(id);
    verify(n != nullptr);
    return *n;
  }
  const RaftNode& node(siteid_t id) const {
    auto* n = node_or_null(id);
    verify(n != nullptr);
    return *n;
  }
  ChannelSwitchboard& switchboard() { return sw_; }

  // @safe - returns the full site-id list.
  const std::vector<siteid_t>& site_ids() const { return site_ids_; }

  // @safe - expose switchboard RPC attempt counters for Raft test harnesses.
  uint64_t rpc_count(siteid_t s) const { return sw_.rpc_count(s); }
  uint64_t rpc_total() const { return sw_.rpc_total(); }

  // ------------------------------------------------------------------
  // Fault injection
  // ------------------------------------------------------------------

  // @safe - stops all traffic from `s` to every peer and vice versa.
  void disconnect(siteid_t s) {
    for (auto peer : site_ids_) {
      if (peer == s) continue;
      sw_.drop_direction(s, peer);
      sw_.drop_direction(peer, s);
    }
  }

  // @safe - restores traffic between `s` and every peer, preserving all
  // unrelated fault injections (other drops, partitions).
  void reconnect(siteid_t s) {
    for (auto peer : site_ids_) {
      if (peer == s) continue;
      sw_.undrop_direction(s, peer);
      sw_.undrop_direction(peer, s);
    }
  }

  // @safe - splits sites into two groups that cannot exchange
  // messages across the boundary. Sites outside both groups remain
  // fully connected (via current switchboard semantics).
  void partition(std::vector<siteid_t> a, std::vector<siteid_t> b) {
    sw_.partition({std::move(a), std::move(b)});
  }

  // @safe - clear all fault injections.
  void reset_faults() { sw_.reset_faults(); }

  // @safe - pretends to kill a node by flipping its is_leader /
  // dispatcher state. A full impl destroys+recreates the node.
  void kill(siteid_t s) {
    node(s).force_leader(false);
    disconnect(s);
  }

  // @safe - re-attaches a node that was previously killed.
  void restart(siteid_t s) {
    reconnect(s);
  }

  // ------------------------------------------------------------------
  // Each node has a background worker thread draining its channel
  // (Phase 8.0 — fiber-synchronous senders block on the reply channel,
  // so they need a live consumer on the far side).
  // ------------------------------------------------------------------

  ~TestCluster() {
    for (auto& poll : poll_threads_) {
      poll->shutdown();
    }
    poll_threads_.clear();

    // Drop all senders FIRST so every worker's step_blocking() recv()
    // returns Err and the loop exits. Then join the threads so the
    // detached members (workers_/receivers) aren't destroyed while a
    // worker is mid-recv on them.
    stop_.store(true, std::memory_order_release);
    sw_.clear();  // drops all senders
    for (auto& t : worker_threads_) {
      if (t.joinable()) t.join();
    }
  }

 private:
  // @safe - builds the cluster
  void build(size_t n) {
    // TestCluster does not wire the full production communicator path used by
    // Jetpack recovery RPCs.
    ::setenv("MAKO_DISABLE_JETPACK", "1", 1);
    // Align test-cluster runs with the Raft lab heartbeat cadence.
    ::setenv("MAKO_RAFT_HEARTBEAT_INTERVAL_US", "100000", 1);

    site_ids_.reserve(n);
    for (size_t i = 0; i < n; ++i) site_ids_.push_back(static_cast<siteid_t>(i + 1));

    // Register every site with the switchboard; stash receivers
    // for the workers we spin up below.
    std::vector<rusty::sync::mpsc::Receiver<Envelope>> receivers;
    receivers.reserve(n);
    for (auto id : site_ids_) {
      receivers.push_back(sw_.register_site(id));
    }

    // Build the per-site storage + node + worker trio.
    for (size_t i = 0; i < n; ++i) {
      auto id = site_ids_[i];
      // @unsafe { direct `new` because Mutex-containing types are not
      //           copy-constructible, so Box::make() does not apply }
      logs_.emplace_back(new InMemoryLogStorage());
      snaps_.emplace_back(new MemorySnapshotManager());

      TransportProxy tr = make_channel_transport(&sw_, id, /*par=*/0);
      rusty::Box<RaftNode> node(new RaftNode(
          id, std::move(tr), logs_.back().get(), snaps_.back().get(),
          &sw_, /*partition_id=*/0, site_ids_));
      if (node->server() != nullptr) {
        // TestCluster uses a no-op learner action so apply threads can run
        // safely without full deptran scheduler wiring.
        node->server()->RegLearnerAction(
            [](int /*slot*/, MarshallDeputy /*md*/) -> int { return 0; });
        // Raft lab tests expect plain Raft elections, without automatic
        // preferred-leader transfer churn.
        node->server()->SetPreferredLeader(INVALID_SITEID);
        node->server()->BootstrapReplicationStateForTest();
      }
      auto poll_thread = rrr::PollThread::create();
      if (node->server() != nullptr) {
        auto* server = node->server();
        auto start_runtime_job = rusty::Arc<rrr::OneTimeJob>::new_(
            rrr::OneTimeJob([server]() {
              server->StartInProcessTestRuntimeForTest();
            }));
        poll_thread->add(rusty::Arc<rrr::Job>(start_runtime_job));
      }

      rusty::Box<ChannelNodeWorker> worker(new ChannelNodeWorker(
          std::move(receivers[i]), node->take_dispatcher()));

      nodes_.push_back(std::move(node));
      workers_.push_back(std::move(worker));
      poll_threads_.push_back(std::move(poll_thread));
    }

    // Spawn one background drainer per node.
    // @unsafe { std::thread at test-harness boundary }
    for (auto& w : workers_) {
      ChannelNodeWorker* wptr = w.get();
      worker_threads_.emplace_back(std::thread([wptr]() {
        while (wptr->step_blocking()) {}
      }));
    }
  }

  // Declaration order matters: members are destroyed in REVERSE order,
  // so sw_ must come LAST (destroyed first) to drop its Senders and
  // let every worker's step_blocking() recv() return Err before the
  // workers' Receivers are destroyed. Otherwise detached worker
  // threads UAF on their receivers.
  std::atomic<bool>                              stop_{false};
  std::vector<std::thread>                       worker_threads_;
  std::vector<rusty::Box<ChannelNodeWorker>>     workers_;
  std::vector<rusty::Arc<rrr::PollThread>>       poll_threads_;
  std::vector<rusty::Box<RaftNode>>              nodes_;
  std::vector<rusty::Box<MemorySnapshotManager>> snaps_;
  std::vector<rusty::Box<InMemoryLogStorage>>    logs_;
  std::vector<siteid_t>                          site_ids_;
  ChannelSwitchboard                             sw_;
};

}  // namespace raft
}  // namespace janus
