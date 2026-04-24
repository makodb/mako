// Phase 8.0 smoke test: stand up a 3-node in-memory raft cluster, send
// some RPCs through it fiber-synchronously, and verify fault injection
// silences the right traffic.
//
// Each node runs a background drainer thread inside TestCluster, so
// senders just call `transport()->send_x(dst, req)` and receive the
// reply as a return value.

#include <gtest/gtest.h>

#include "deptran/raft/test_cluster.hpp"

using namespace janus::raft;

TEST(RaftTestClusterTest, BuildAndSendAVote) {
  auto c = TestCluster::with_in_memory_transport(3);
  EXPECT_EQ(c->size(), 3u);
  EXPECT_EQ(c->site_ids().size(), 3u);

  // Node 1 votes peer 2; DummyDispatcher replies vote_granted=true.
  auto r = c->node(1).transport()->send_vote(2, VoteReq{1, 0, 1, 1});
  EXPECT_TRUE(r.vote_granted);
}

TEST(RaftTestClusterTest, DisconnectStopsTraffic) {
  auto c = TestCluster::with_in_memory_transport(3);
  c->disconnect(2);

  // 1→2 is dropped at the switchboard; reply channel closes, adapter
  // falls back to default TimeoutNowReply (success=false).
  auto dropped = c->node(1).transport()->send_timeout_now(2, TimeoutNowReq{});
  EXPECT_FALSE(dropped.success);
  // 1→3 still works (not on the drop list).
  auto ok3 = c->node(1).transport()->send_timeout_now(3, TimeoutNowReq{});
  EXPECT_TRUE(ok3.success);

  c->reset_faults();
  auto ok2 = c->node(1).transport()->send_timeout_now(2, TimeoutNowReq{});
  EXPECT_TRUE(ok2.success);
}

TEST(RaftTestClusterTest, PartitionIsolatesGroups) {
  auto c = TestCluster::with_in_memory_transport(5);
  c->partition({1, 2}, {3, 4, 5});

  // 1→3: across partition, dropped.
  auto cross = c->node(1).transport()->send_timeout_now(3, TimeoutNowReq{});
  EXPECT_FALSE(cross.success);
  // 1→2: same partition, delivered.
  auto intra = c->node(1).transport()->send_timeout_now(2, TimeoutNowReq{});
  EXPECT_TRUE(intra.success);
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
