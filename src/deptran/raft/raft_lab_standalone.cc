/**
 * raft_lab_standalone — Phase 7 of the Raft decouple plan.
 *
 * Stands up an in-process Raft cluster using TestCluster (Phase 6)
 * and exercises its transport + fault-injection plumbing end-to-end
 * without binding any socket and without any rocksdb on disk.
 *
 * What this binary DOES prove today:
 *   - The Phase 4 ChannelSwitchboard + Phase 6 TestCluster build a
 *     valid N-node topology.
 *   - Transport RPCs (send_*, broadcast_vote) round-trip through the
 *     in-memory switchboard to every peer's DummyDispatcher.
 *   - disconnect / partition / reset_faults silence and restore
 *     traffic as expected.
 *   - It runs without binding a socket (verify with
 *     `ss -lntp | grep raft_lab_standalone` — no rows).
 *
 * What this binary DOES NOT yet prove (tracked under Phase 6.5 in
 * docs/dev/raft_decouple_plan.md):
 *   - Actual Raft leader election / log replication semantics. The
 *     DummyDispatcher always says "yes" to vote and "ok" to append,
 *     so the binary cannot host the full RaftLabTest suite until
 *     RaftServer itself is decoupled from rrr::PollThread /
 *     rrr::Fiber.
 */

#include <cstdio>
#include <cstdlib>
#include <string>

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
bool case_three_node_broadcast_reaches_every_peer(TestCluster& c) {
  int replies = 0;
  c.node(1).transport()->broadcast_vote(
      0, VoteReq{1, 0, 1, 1},
      [&](siteid_t, VoteReply r) {
        if (r.vote_granted) ++replies;
      });
  c.step_until_quiesce();
  return replies == 2;  // nodes 2 and 3 reply; node 1 excludes self
}

// @safe
bool case_disconnect_blocks_one_node(TestCluster& c) {
  c.reset_faults();
  c.disconnect(2);
  int replies = 0;
  c.node(1).transport()->send_timeout_now(2, TimeoutNowReq{},
      [&](siteid_t, TimeoutNowReply) { ++replies; });
  c.node(1).transport()->send_timeout_now(3, TimeoutNowReq{},
      [&](siteid_t, TimeoutNowReply) { ++replies; });
  c.step_until_quiesce();
  c.reset_faults();
  return replies == 1;  // only site 3 answered
}

// @safe
bool case_partition_isolates_groups() {
  auto c = TestCluster::with_in_memory_transport(5);
  c->partition({1, 2}, {3, 4, 5});
  int cross = 0;
  int intra = 0;
  c->node(1).transport()->send_timeout_now(3, TimeoutNowReq{},
      [&](siteid_t, TimeoutNowReply) { ++cross; });
  c->node(1).transport()->send_timeout_now(2, TimeoutNowReq{},
      [&](siteid_t, TimeoutNowReply) { ++intra; });
  c->step_until_quiesce();
  return cross == 0 && intra == 1;
}

// @safe
bool case_fire_and_forget_durables_deliver(TestCluster& c) {
  c.reset_faults();
  c.node(1).transport()->send_vote_durable(2, VoteDurableReq{});
  c.node(1).transport()->send_append_entries_durable(2,
      AppendEntriesDurableReq{});
  // Returns the total number of messages the cluster delivered this
  // round; two fire-and-forget RPCs should show up.
  return c.step_until_quiesce() >= 2;
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
  std::printf("raft_lab_standalone — in-process Raft harness (Phase 7)\n");

  Harness h{3};

  h.check("three_node_broadcast_reaches_every_peer",
          [&] { return case_three_node_broadcast_reaches_every_peer(*h.cluster); });

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
