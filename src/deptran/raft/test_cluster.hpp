#pragma once

/**
 * @file test_cluster.hpp
 * @brief Phase 6 — in-process raft cluster harness. Wires N RaftNodes
 *        together through a ChannelSwitchboard and exposes the fault-
 *        injection controls the lab tests need (kill / restart /
 *        disconnect / partition).
 *
 * Current scope (matches raft_node.hpp): this is a SKELETON. The
 * cluster plumbing — per-node worker threads, transport fan-out, peer
 * discovery — is complete enough for Phase 7 to stand up a test
 * binary. Actual Raft state-machine semantics come online once Phase
 * 6.5 swaps DummyDispatcher for a RaftServer-backed impl.
 */

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/move.hpp>

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
  RaftNode& node(siteid_t id) {
    for (auto& n : nodes_) {
      if (n->id() == id) return *n;
    }
    // @unsafe { bogus id — abort via out-of-range dereference }
    return *nodes_.at(nodes_.size());  // throws std::out_of_range
  }
  ChannelSwitchboard& switchboard() { return sw_; }

  // @safe - returns the full site-id list.
  const std::vector<siteid_t>& site_ids() const { return site_ids_; }

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

  // @safe - splits sites into two groups that cannot exchange
  // messages across the boundary. Sites outside both groups remain
  // fully connected (via current switchboard semantics).
  void partition(std::vector<siteid_t> a, std::vector<siteid_t> b) {
    sw_.partition({rusty::move(a), rusty::move(b)});
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
    for (auto peer : site_ids_) {
      if (peer == s) continue;
      // A full impl would rebuild the node in place; the MVP just
      // clears its direction from the fault list. Since ChannelFaults
      // lacks a per-direction remove, we reset and re-apply — the
      // MVP cluster only supports one site being down at a time.
      (void)peer;
    }
    sw_.reset_faults();
  }

  // ------------------------------------------------------------------
  // Each node has a background worker thread draining its channel
  //.
  // ------------------------------------------------------------------

  ~TestCluster() {
    // Drop all senders FIRST so every worker's step_blocking() recv()
    // returns Err and the loop exits. Then join the threads so the
    // detached members (workers_/receivers) aren't destroyed while a
    // worker is mid-recv on them.
    stop_.store(true, std::memory_order_release);
    sw_ = ChannelSwitchboard{};  // move-assign empty; drops all senders
    for (auto& t : worker_threads_) {
      if (t.joinable()) t.join();
    }
  }

 private:
  // @safe - builds the cluster
  void build(size_t n) {
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
          id, rusty::move(tr), logs_.back().get(), snaps_.back().get()));

      rusty::Box<ChannelNodeWorker> worker(new ChannelNodeWorker(
          rusty::move(receivers[i]), node->take_dispatcher()));

      nodes_.push_back(rusty::move(node));
      workers_.push_back(rusty::move(worker));
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
  std::vector<rusty::Box<RaftNode>>              nodes_;
  std::vector<rusty::Box<MemorySnapshotManager>> snaps_;
  std::vector<rusty::Box<InMemoryLogStorage>>    logs_;
  std::vector<siteid_t>                          site_ids_;
  ChannelSwitchboard                             sw_;
};

}  // namespace raft
}  // namespace janus
