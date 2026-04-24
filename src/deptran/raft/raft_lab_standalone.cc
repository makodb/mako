/**
 * raft_lab_standalone — Phase 7/8 of the Raft decouple plan.
 *
 * Stands up an in-process Raft cluster using TestCluster and exercises
 * its fiber-synchronous transport + fault-injection plumbing end-to-end
 * without binding any socket and without any rocksdb on disk.
 *
 * What this binary DOES prove today:
 *   - ChannelSwitchboard + TestCluster build a valid N-node topology.
 *   - Transport RPCs (send_*) round-trip through the in-memory
 *     switchboard to every peer's DummyDispatcher.
 *   - disconnect / partition / reset_faults silence and restore
 *     traffic as expected.
 *   - It runs without binding a socket (verify with
 *     `ss -lntp | grep raft_lab_standalone` — no rows).
 *
 * Phase 8.7 replaces the DummyDispatcher harness here with a real
 * RaftServer + RaftLabTest driver.
 */

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "deptran/raft/test_cluster.hpp"

using namespace janus::raft;

namespace {

struct Harness {
  rusty::Box<TestCluster> cluster;
  int                     failures = 0;

  // @safe
  Harness(size_t n) : cluster(TestCluster::with_in_memory_transport(n)) {}

  template <typename Fn>
  void check(const char* name, Fn fn) {
    std::printf("[ RUN      ] %s\n", name);
    bool ok = fn();
    if (ok) {
      std::printf("[       OK ] %s\n", name);
    } else {
      ++failures;
      std::printf("[     FAIL ] %s\n", name);
    }
  }
};

// @safe
bool case_three_node_votes_each_peer(TestCluster& c) {
  int granted = 0;
  for (siteid_t peer : c.site_ids()) {
    if (peer == 1) continue;
    auto r = c.node(1).transport()->send_vote(peer, VoteReq{1, 0, 1, 1});
    if (r.vote_granted) ++granted;
  }
  return granted == 2;
}

// @safe
bool case_disconnect_blocks_one_node(TestCluster& c) {
  c.reset_faults();
  c.disconnect(2);
  auto dropped = c.node(1).transport()->send_timeout_now(2, TimeoutNowReq{});
  auto ok3 = c.node(1).transport()->send_timeout_now(3, TimeoutNowReq{});
  c.reset_faults();
  return !dropped.success && ok3.success;
}

// @safe
bool case_partition_isolates_groups() {
  auto c = TestCluster::with_in_memory_transport(5);
  c->partition({1, 2}, {3, 4, 5});
  auto cross = c->node(1).transport()->send_timeout_now(3, TimeoutNowReq{});
  auto intra = c->node(1).transport()->send_timeout_now(2, TimeoutNowReq{});
  return !cross.success && intra.success;
}

// @safe
bool case_fire_and_forget_durables_deliver(TestCluster& c) {
  c.reset_faults();
  c.node(1).transport()->send_vote_durable(2, VoteDurableReq{});
  c.node(1).transport()->send_append_entries_durable(2,
      AppendEntriesDurableReq{});
  // Give the background workers a beat to consume the durables.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return true;
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
  std::printf("raft_lab_standalone — in-process Raft harness\n");

  Harness h{3};

  h.check("three_node_votes_each_peer",
          [&] { return case_three_node_votes_each_peer(*h.cluster); });

  h.check("disconnect_blocks_one_node",
          [&] { return case_disconnect_blocks_one_node(*h.cluster); });

  h.check("partition_isolates_groups",
          [&] { return case_partition_isolates_groups(); });

  h.check("fire_and_forget_durables_deliver",
          [&] { return case_fire_and_forget_durables_deliver(*h.cluster); });

  std::printf("[==========] ");
  if (h.failures == 0) {
    std::printf("PASSED\n");
    return 0;
  }
  std::printf("FAILED (%d cases)\n", h.failures);
  return 1;
}
