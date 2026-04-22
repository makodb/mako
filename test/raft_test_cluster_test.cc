// Phase 6 smoke test: stand up a 3-node in-memory raft cluster, send
// some RPCs through it, and verify fault injection silences the right
// traffic.

#include <gtest/gtest.h>

#include "deptran/raft/test_cluster.hpp"

using namespace janus::raft;

TEST(RaftTestClusterTest, BuildAndStepAVote) {
  auto c = TestCluster::with_in_memory_transport(3);
  EXPECT_EQ(c->size(), 3u);
  EXPECT_EQ(c->site_ids().size(), 3u);

  int replies = 0;
  // Node 1 broadcasts a vote; Nodes 2 and 3 receive (DummyDispatcher
  // replies vote_granted=true).
  c->node(1).transport()->broadcast_vote(
      /*par=*/0, VoteReq{1, 0, 1, 1},
      [&](siteid_t from, VoteReply r) {
        EXPECT_TRUE(r.vote_granted);
        (void)from;
        ++replies;
      });

  EXPECT_GT(c->step_until_quiesce(), 0u);
  EXPECT_EQ(replies, 2);
}

TEST(RaftTestClusterTest, DisconnectStopsTraffic) {
  auto c = TestCluster::with_in_memory_transport(3);
  c->disconnect(2);

  int replies = 0;
  c->node(1).transport()->send_timeout_now(2, TimeoutNowReq{},
      [&](siteid_t, TimeoutNowReply) { ++replies; });
  c->node(1).transport()->send_timeout_now(3, TimeoutNowReq{},
      [&](siteid_t, TimeoutNowReply) { ++replies; });
  c->step_until_quiesce();
  EXPECT_EQ(replies, 1);  // only site 3 answered

  c->reset_faults();
  c->node(1).transport()->send_timeout_now(2, TimeoutNowReq{},
      [&](siteid_t, TimeoutNowReply) { ++replies; });
  c->step_until_quiesce();
  EXPECT_EQ(replies, 2);  // site 2 answers after reconnect
}

TEST(RaftTestClusterTest, PartitionIsolatesGroups) {
  auto c = TestCluster::with_in_memory_transport(5);
  c->partition({1, 2}, {3, 4, 5});

  int replies = 0;
  // 1 -> 3 should drop (across partition).
  c->node(1).transport()->send_timeout_now(3, TimeoutNowReq{},
      [&](siteid_t, TimeoutNowReply) { ++replies; });
  // 1 -> 2 should work (same partition).
  c->node(1).transport()->send_timeout_now(2, TimeoutNowReq{},
      [&](siteid_t, TimeoutNowReply) { ++replies; });
  c->step_until_quiesce();
  EXPECT_EQ(replies, 1);
}

TEST(RaftTestClusterTest, InspectionAccessors) {
  auto c = TestCluster::with_in_memory_transport(3);
  c->node(1).force_leader(true);
  c->node(1).set_current_term(42);
  c->node(1).set_commit_index(10);

  EXPECT_TRUE(c->node(1).is_leader());
  EXPECT_EQ(c->node(1).current_term(), 42u);
  EXPECT_EQ(c->node(1).commit_index(), 10u);
  EXPECT_FALSE(c->node(2).is_leader());
}
