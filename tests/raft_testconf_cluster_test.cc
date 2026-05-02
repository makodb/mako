#include <gtest/gtest.h>

#include "deptran/raft/test_cluster.hpp"
#include "deptran/raft/testconf.h"

#ifndef RAFT_TEST_CORO

TEST(RaftTestConfigClusterTest, RequiresRaftTestCoro) {
  GTEST_SKIP() << "RAFT_TEST_CORO is required for RaftTestConfig tests";
}

#else

namespace {

TEST(RaftTestConfigClusterTest, OneLeaderAndAgreementWithTestClusterBackend) {
  auto cluster = janus::raft::TestCluster::with_in_memory_transport(NSERVERS);
  janus::RaftTestConfig cfg(*cluster);
  cfg.SetLearnerAction();

  const int leader = cfg.OneLeader();
  ASSERT_GE(leader, 0);

  const uint64_t index = cfg.DoAgreement(/*cmd=*/5101, /*n=*/NSERVERS, /*retry=*/true);
  ASSERT_GT(index, 0u);
  EXPECT_EQ(cfg.NCommitted(index), NSERVERS);

  cfg.Shutdown();
}

TEST(RaftTestConfigClusterTest, DisconnectReconnectControlsQuorumInClusterBackend) {
  auto cluster = janus::raft::TestCluster::with_in_memory_transport(NSERVERS);
  janus::RaftTestConfig cfg(*cluster);
  cfg.SetLearnerAction();

  const int leader = cfg.OneLeader();
  ASSERT_GE(leader, 0);

  const siteid_t follower =
      cfg.getNextServerId(static_cast<siteid_t>(leader), 1);
  cfg.Disconnect(follower);
  EXPECT_EQ(cfg.NDisconnected(), 1);

  const uint64_t partial_index =
      cfg.DoAgreement(/*cmd=*/5102, /*n=*/NSERVERS - 1, /*retry=*/true);
  ASSERT_GT(partial_index, 0u);
  EXPECT_EQ(cfg.NCommitted(partial_index), NSERVERS - 1);

  cfg.Reconnect(follower);
  EXPECT_EQ(cfg.NDisconnected(), 0);

  const uint64_t full_index =
      cfg.DoAgreement(/*cmd=*/5103, /*n=*/NSERVERS, /*retry=*/true);
  ASSERT_GT(full_index, partial_index);
  EXPECT_EQ(cfg.NCommitted(full_index), NSERVERS);

  cfg.Shutdown();
}

TEST(RaftTestConfigClusterTest, ReconnectPreservesOtherDisconnectsInClusterBackend) {
  auto cluster = janus::raft::TestCluster::with_in_memory_transport(NSERVERS);
  janus::RaftTestConfig cfg(*cluster);
  cfg.SetLearnerAction();

  const int leader = cfg.OneLeader();
  ASSERT_GE(leader, 0);

  const siteid_t follower1 =
      cfg.getNextServerId(static_cast<siteid_t>(leader), 1);
  const siteid_t follower2 =
      cfg.getNextServerId(static_cast<siteid_t>(leader), 2);

  cfg.Disconnect(follower1);
  cfg.Disconnect(follower2);
  EXPECT_EQ(cfg.NDisconnected(), 2);

  cfg.Reconnect(follower1);
  EXPECT_EQ(cfg.NDisconnected(), 1);

  const uint64_t partial_index =
      cfg.DoAgreement(/*cmd=*/5201, /*n=*/NSERVERS - 1, /*retry=*/true);
  ASSERT_GT(partial_index, 0u);
  EXPECT_EQ(cfg.NCommitted(partial_index), NSERVERS - 1);

  cfg.Reconnect(follower2);
  EXPECT_EQ(cfg.NDisconnected(), 0);
  const uint64_t full_index =
      cfg.DoAgreement(/*cmd=*/5202, /*n=*/NSERVERS, /*retry=*/true);
  ASSERT_GT(full_index, partial_index);
  EXPECT_EQ(cfg.NCommitted(full_index), NSERVERS);

  cfg.Shutdown();
}

TEST(RaftTestConfigClusterTest, KillRestartPreservesOtherDisconnectsInClusterBackend) {
  auto cluster = janus::raft::TestCluster::with_in_memory_transport(NSERVERS);
  janus::RaftTestConfig cfg(*cluster);
  cfg.SetLearnerAction();

  const int leader = cfg.OneLeader();
  ASSERT_GE(leader, 0);

  const siteid_t restart_target =
      cfg.getNextServerId(static_cast<siteid_t>(leader), 1);
  const siteid_t still_disconnected =
      cfg.getNextServerId(static_cast<siteid_t>(leader), 2);

  cfg.Disconnect(still_disconnected);
  EXPECT_EQ(cfg.NDisconnected(), 1);

  cfg.Kill(restart_target);
  EXPECT_EQ(cfg.NDisconnected(), 2);

  cfg.Restart(restart_target);
  EXPECT_EQ(cfg.NDisconnected(), 1);

  const uint64_t partial_index =
      cfg.DoAgreement(/*cmd=*/5301, /*n=*/NSERVERS - 1, /*retry=*/true);
  ASSERT_GT(partial_index, 0u);
  EXPECT_EQ(cfg.NCommitted(partial_index), NSERVERS - 1);

  cfg.Reconnect(still_disconnected);
  EXPECT_EQ(cfg.NDisconnected(), 0);
  const uint64_t full_index =
      cfg.DoAgreement(/*cmd=*/5302, /*n=*/NSERVERS, /*retry=*/true);
  ASSERT_GT(full_index, partial_index);
  EXPECT_EQ(cfg.NCommitted(full_index), NSERVERS);

  cfg.Shutdown();
}

TEST(RaftTestConfigClusterTest, ReconnectPreservesPartitionFaultsInClusterBackend) {
  auto cluster = janus::raft::TestCluster::with_in_memory_transport(NSERVERS);
  janus::RaftTestConfig cfg(*cluster);

  cluster->partition({1, 2}, {3, 4, 5});

  auto dropped_before =
      cluster->node(1).transport()->send_vote(
          3, janus::raft::VoteReq{1000, 1000, 1, 1});
  EXPECT_FALSE(dropped_before.vote_granted);

  cfg.Disconnect(/*svr=*/2);
  EXPECT_EQ(cfg.NDisconnected(), 1);
  cfg.Reconnect(/*svr=*/2);
  EXPECT_EQ(cfg.NDisconnected(), 0);

  auto dropped_after =
      cluster->node(1).transport()->send_vote(
          3, janus::raft::VoteReq{1001, 1000, 2, 1});
  EXPECT_FALSE(dropped_after.vote_granted);

  cfg.Shutdown();
}

TEST(RaftTestConfigClusterTest, ReconnectKeepsOtherDisconnectedNodesIsolatedInClusterBackend) {
  auto cluster = janus::raft::TestCluster::with_in_memory_transport(NSERVERS);
  janus::RaftTestConfig cfg(*cluster);

  cfg.Disconnect(/*svr=*/1);
  cfg.Disconnect(/*svr=*/2);
  cfg.Disconnect(/*svr=*/3);
  EXPECT_EQ(cfg.NDisconnected(), 3);

  cfg.Reconnect(/*svr=*/3);
  EXPECT_EQ(cfg.NDisconnected(), 2);

  EXPECT_TRUE(cluster->switchboard().is_dropped_for_test(/*from=*/1, /*to=*/3));
  EXPECT_TRUE(cluster->switchboard().is_dropped_for_test(/*from=*/3, /*to=*/1));
  EXPECT_TRUE(cluster->switchboard().is_dropped_for_test(/*from=*/2, /*to=*/3));
  EXPECT_TRUE(cluster->switchboard().is_dropped_for_test(/*from=*/3, /*to=*/2));

  cfg.Shutdown();
}

TEST(RaftTestConfigClusterTest, AcceptsZeroBasedServerIndexesInClusterBackend) {
  auto cluster = janus::raft::TestCluster::with_in_memory_transport(NSERVERS);
  janus::RaftTestConfig cfg(*cluster);
  cfg.SetLearnerAction();

  for (int i = 0; i < NSERVERS; ++i) {
    siteid_t site_id = cfg.getServerIdByIndex(i);
    ASSERT_NE(cfg.GetServer(site_id), nullptr);
    EXPECT_EQ(cfg.GetServer(static_cast<siteid_t>(i)),
              cfg.GetServer(site_id));
  }

  const siteid_t site1_wire = cfg.GetServer(0)->site_id_;
  siteid_t peer_wire = cfg.GetServer(1)->site_id_;
  if (peer_wire == site1_wire) {
    peer_wire = cfg.GetServer(2)->site_id_;
  }

  cfg.Disconnect(/*svr=*/0);
  EXPECT_EQ(cfg.NDisconnected(), 1);
  EXPECT_TRUE(cluster->switchboard().is_dropped_for_test(site1_wire, peer_wire));
  EXPECT_TRUE(cluster->switchboard().is_dropped_for_test(peer_wire, site1_wire));

  cfg.Reconnect(/*svr=*/0);
  EXPECT_EQ(cfg.NDisconnected(), 0);
  EXPECT_FALSE(cluster->switchboard().is_dropped_for_test(site1_wire, peer_wire));
  EXPECT_FALSE(cluster->switchboard().is_dropped_for_test(peer_wire, site1_wire));

  cfg.Shutdown();
}

}  // namespace

#endif
