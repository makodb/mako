// Phase 8.0 smoke test: stand up a 3-node in-memory raft cluster, send
// some RPCs through it fiber-synchronously, and verify fault injection
// silences the right traffic.
//
// Each node runs a background drainer thread inside TestCluster, so
// senders just call `transport()->send_x(dst, req)` and receive the
// reply as a return value.

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "deptran/classic/tpc_command.h"
#include "deptran/raft/test_cluster.hpp"

using namespace janus::raft;

namespace {

siteid_t WaitForSingleLeader(TestCluster& c, std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    siteid_t leader = 0;
    int leaders = 0;
    for (auto site_id : c.site_ids()) {
      if (c.node(site_id).is_leader()) {
        leaders++;
        leader = site_id;
      }
    }
    if (leaders == 1) {
      return leader;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return 0;
}

bool WaitForCommitIndexAtLeast(TestCluster& c,
                               const std::vector<siteid_t>& sites,
                               uint64_t target_index,
                               std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    bool all_reached = true;
    for (auto site_id : sites) {
      if (c.node(site_id).commit_index() < target_index) {
        all_reached = false;
        break;
      }
    }
    if (all_reached) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

shared_ptr<Marshallable> MakeTestCommitCommand(int tx_id) {
  auto cmd = std::make_shared<janus::TpcCommitCommand>();
  auto pieces = std::make_shared<janus::VecPieceData>();
  pieces->sp_vec_piece_data_ =
      std::make_shared<std::vector<std::shared_ptr<janus::SimpleCommand>>>();
  cmd->tx_id_ = tx_id;
  cmd->cmd_ = wrap_typed_marshallable(pieces);
  return wrap_typed_marshallable(cmd);
}

}  // namespace

TEST(RaftTestClusterTest, BuildAndSendAVote) {
  auto c = TestCluster::with_in_memory_transport(3);
  EXPECT_EQ(c->size(), 3u);
  EXPECT_EQ(c->site_ids().size(), 3u);

  // Node 1 votes peer 2; RaftServer-backed dispatcher grants vote.
  auto r = c->node(1).transport()->send_vote(2, VoteReq{1, 0, 1, 1});
  EXPECT_TRUE(r.vote_granted);
}

TEST(RaftTestClusterTest, VoteRejectsDifferentCandidateInSameTerm) {
  auto c = TestCluster::with_in_memory_transport(3);

  auto first = c->node(1).transport()->send_vote(2, VoteReq{1, 0, 1, 1});
  EXPECT_TRUE(first.vote_granted);

  // Real Raft behavior: node 2 should reject a different candidate (3)
  // in the same term after voting for candidate 1.
  auto second = c->node(1).transport()->send_vote(2, VoteReq{1, 0, 3, 1});
  EXPECT_FALSE(second.vote_granted);
}

TEST(RaftTestClusterTest, DisconnectStopsTraffic) {
  auto c = TestCluster::with_in_memory_transport(3);
  c->disconnect(2);

  // 1→2 is dropped at the switchboard; reply channel closes, adapter
  // falls back to default VoteReply (vote_granted=false).
  auto dropped = c->node(1).transport()->send_vote(2, VoteReq{1, 0, 1, 1});
  EXPECT_FALSE(dropped.vote_granted);
  // 1→3 still works (not on the drop list).
  auto ok3 = c->node(1).transport()->send_vote(3, VoteReq{1, 0, 1, 1});
  EXPECT_TRUE(ok3.vote_granted);

  c->reset_faults();
  auto ok2 = c->node(1).transport()->send_vote(2, VoteReq{1, 0, 1, 1});
  EXPECT_TRUE(ok2.vote_granted);
}

TEST(RaftTestClusterTest, PartitionIsolatesGroups) {
  auto c = TestCluster::with_in_memory_transport(5);
  c->partition({1, 2}, {3, 4, 5});

  // 1→3: across partition, dropped.
  auto cross = c->node(1).transport()->send_vote(3, VoteReq{1, 0, 1, 1});
  EXPECT_FALSE(cross.vote_granted);
  // 1→2: same partition, delivered.
  auto intra = c->node(1).transport()->send_vote(2, VoteReq{1, 0, 1, 1});
  EXPECT_TRUE(intra.vote_granted);
}

TEST(RaftTestClusterTest, AppendEntriesTransportRoundTrip) {
  auto c = TestCluster::with_in_memory_transport(3);

  AppendEntriesReq append_req{};
  append_req.leader_site_id = 1;
  append_req.leader_current_term = 7;
  auto append_reply = c->node(1).transport()->send_append_entries(2, append_req);
  EXPECT_EQ(append_reply.follower_append_ok, 1u);

  EmptyAppendEntriesReq hb_req{};
  hb_req.leader_site_id = 1;
  hb_req.leader_current_term = 7;
  hb_req.trigger_election_now = true;
  auto hb_reply = c->node(1).transport()->send_empty_append_entries(2, hb_req);
  EXPECT_EQ(hb_reply.follower_append_ok, 1u);
}

TEST(RaftTestClusterTest, AppendEntriesTransportDropFallback) {
  auto c = TestCluster::with_in_memory_transport(3);

  c->disconnect(2);

  AppendEntriesReq append_req{};
  append_req.leader_site_id = 1;
  append_req.leader_current_term = 8;
  auto append_reply = c->node(1).transport()->send_append_entries(2, append_req);
  EXPECT_EQ(append_reply.follower_append_ok, 0u);
  EXPECT_EQ(append_reply.follower_current_term, 0u);
  EXPECT_EQ(append_reply.follower_last_log_index, 0u);
  EXPECT_EQ(append_reply.follower_ack_type, 0u);

  EmptyAppendEntriesReq hb_req{};
  hb_req.leader_site_id = 1;
  hb_req.leader_current_term = 8;
  auto hb_reply = c->node(1).transport()->send_empty_append_entries(2, hb_req);
  EXPECT_EQ(hb_reply.follower_append_ok, 0u);
  EXPECT_EQ(hb_reply.follower_current_term, 0u);
  EXPECT_EQ(hb_reply.follower_last_log_index, 0u);
  EXPECT_EQ(hb_reply.follower_ack_type, 0u);

  // 1->3 remains connected and should still receive successful replies.
  auto append_ok3 = c->node(1).transport()->send_append_entries(3, append_req);
  EXPECT_EQ(append_ok3.follower_append_ok, 1u);
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

  ASSERT_NE(c->node(1).server(), nullptr);
  EXPECT_EQ(c->node(1).server()->currentTerm, 42u);
  EXPECT_EQ(c->node(1).server()->commitIndex, 10u);
}

TEST(RaftTestClusterTest, ServerDispatcherRoutesMembershipHandlers) {
  auto c = TestCluster::with_in_memory_transport(3);
  auto disp = c->node(1).take_dispatcher();

  AddServerReq add_req{};
  add_req.term = 3;
  add_req.new_server_id = 7;
  add_req.new_server_addr = "n7:9007";
  auto add_resp = disp->handle_add_server(add_req);
  EXPECT_FALSE(add_resp.success);
  EXPECT_FALSE(add_resp.error_msg.empty());
  EXPECT_EQ(add_resp.leader_hint, 0u);

  RemoveServerReq rem_req{};
  rem_req.term = 3;
  rem_req.server_id = 7;
  auto rem_resp = disp->handle_remove_server(rem_req);
  EXPECT_FALSE(rem_resp.success);
  EXPECT_FALSE(rem_resp.error_msg.empty());
  EXPECT_EQ(rem_resp.leader_hint, 0u);
}

TEST(RaftTestClusterTest, ServerOwnershipIsBackedByRealRaftServer) {
  auto c = TestCluster::with_in_memory_transport(3);

  ASSERT_TRUE(c->node(1).has_server());
  ASSERT_NE(c->node(1).server(), nullptr);

  ASSERT_TRUE(c->node(2).has_server());
  ASSERT_NE(c->node(2).server(), nullptr);

  ASSERT_TRUE(c->node(3).has_server());
  ASSERT_NE(c->node(3).server(), nullptr);
}

TEST(RaftTestClusterTest, ServersStartHeartbeatReady) {
  auto c = TestCluster::with_in_memory_transport(5);
  for (auto site_id : c->site_ids()) {
    ASSERT_NE(c->node(site_id).server(), nullptr);
    EXPECT_TRUE(c->node(site_id)
                    .server()
                    ->ReplicationStateReadyForHeartbeatTickForTest());
  }
}

TEST(RaftTestClusterTest, RuntimeStartupElectsSingleLeader) {
  auto c = TestCluster::with_in_memory_transport(3);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
  int stable_samples = 0;

  while (std::chrono::steady_clock::now() < deadline && stable_samples < 5) {
    int leaders = 0;
    for (auto site_id : c->site_ids()) {
      if (c->node(site_id).is_leader()) {
        leaders++;
      }
    }
    if (leaders == 1) {
      stable_samples++;
    } else {
      stable_samples = 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  EXPECT_GE(stable_samples, 5);
}

TEST(RaftTestClusterTest, AgreementAdvancesCommitIndexOnAllNodes) {
  auto c = TestCluster::with_in_memory_transport(3);
  auto leader_id = WaitForSingleLeader(*c, std::chrono::seconds(6));
  ASSERT_NE(leader_id, 0u);
  ASSERT_NE(c->node(leader_id).server(), nullptr);

  auto cmd = MakeTestCommitCommand(/*tx_id=*/9001);
  uint64_t log_index = 0;
  uint64_t term = 0;
  ASSERT_TRUE(c->node(leader_id).server()->Start(cmd, &log_index, &term));
  ASSERT_GT(log_index, 0u);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
  bool all_committed = false;
  while (std::chrono::steady_clock::now() < deadline) {
    all_committed = true;
    for (auto site_id : c->site_ids()) {
      if (c->node(site_id).commit_index() < log_index) {
        all_committed = false;
        break;
      }
    }
    if (all_committed) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  EXPECT_TRUE(all_committed);
  for (auto site_id : c->site_ids()) {
    EXPECT_GE(c->node(site_id).commit_index(), log_index);
  }
}

TEST(RaftTestClusterTest, DisconnectFollowerBlocksCatchupUntilResetFaults) {
  auto c = TestCluster::with_in_memory_transport(3);
  auto leader_id = WaitForSingleLeader(*c, std::chrono::seconds(6));
  ASSERT_NE(leader_id, 0u);
  ASSERT_NE(c->node(leader_id).server(), nullptr);

  siteid_t disconnected_follower = 0;
  siteid_t connected_follower = 0;
  for (auto site_id : c->site_ids()) {
    if (site_id == leader_id) {
      continue;
    }
    if (disconnected_follower == 0) {
      disconnected_follower = site_id;
    } else {
      connected_follower = site_id;
    }
  }
  ASSERT_NE(disconnected_follower, 0u);
  ASSERT_NE(connected_follower, 0u);

  c->disconnect(disconnected_follower);

  uint64_t last_log_index = 0;
  for (int i = 0; i < 3; ++i) {
    auto cmd = MakeTestCommitCommand(/*tx_id=*/9100 + i);
    uint64_t log_index = 0;
    uint64_t term = 0;
    ASSERT_TRUE(c->node(leader_id).server()->Start(cmd, &log_index, &term));
    ASSERT_GT(log_index, 0u);
    last_log_index = log_index;
  }

  ASSERT_GT(last_log_index, 0u);
  ASSERT_TRUE(WaitForCommitIndexAtLeast(
      *c, {leader_id, connected_follower}, last_log_index,
      std::chrono::seconds(6)));

  auto behind_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
  while (std::chrono::steady_clock::now() < behind_deadline) {
    EXPECT_LT(c->node(disconnected_follower).commit_index(), last_log_index);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  c->reset_faults();
  ASSERT_TRUE(WaitForCommitIndexAtLeast(
      *c, {disconnected_follower}, last_log_index, std::chrono::seconds(6)));
  EXPECT_GE(c->node(disconnected_follower).commit_index(), last_log_index);
}

TEST(RaftTestClusterTest, RealServerConfigIsBootstrappedFromClusterSites) {
  auto c = TestCluster::with_in_memory_transport(5);

  EXPECT_EQ(c->node(1).server_config_size(), 5u);
  EXPECT_EQ(c->node(3).server_config_size(), 5u);
  EXPECT_TRUE(c->node(1).server_config_contains(1));
  EXPECT_TRUE(c->node(1).server_config_contains(5));
  EXPECT_TRUE(c->node(5).server_config_contains(1));
  EXPECT_TRUE(c->node(5).server_config_contains(5));
}
