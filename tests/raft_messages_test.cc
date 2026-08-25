// Compile-time sanity check that janus::raft::messages.hpp is
// self-contained and srpc-boundary types round-trip cleanly.
#include <gtest/gtest.h>

#include "deptran/raft/messages.hpp"

using namespace janus::raft;

TEST(RaftMessagesTest, DefaultConstructAllRequestReplyTypes) {
  // Smoke test: each struct must be default-constructible and have
  // sensible zero-initialised fields.
  {
    VoteReq r{};
    EXPECT_EQ(r.last_log_idx, 0u);
    EXPECT_EQ(r.candidate_site_id, 0u);
  }
  { VoteReply r{};          EXPECT_FALSE(r.vote_granted); }
  { VoteDurableReq r{};     EXPECT_EQ(r.term, 0u); }
  { VoteDurableReply r{};   EXPECT_FALSE(r.acknowledged); }
  { AppendEntriesReq r{};   EXPECT_EQ(r.leader_commit_index, 0u); }
  { AppendEntriesReply r{}; EXPECT_EQ(r.follower_ack_type, 0u); }
  { EmptyAppendEntriesReq r{};   EXPECT_FALSE(r.trigger_election_now); }
  { EmptyAppendEntriesReply r{}; EXPECT_EQ(r.follower_last_log_index, 0u); }
  { AppendEntriesDurableReq r{};   EXPECT_EQ(r.last_log_index, 0u); }
  { AppendEntriesDurableReply r{}; EXPECT_FALSE(r.acknowledged); }
  { TimeoutNowReq r{};       EXPECT_EQ(r.leader_term, 0u); }
  { TimeoutNowReply r{};     EXPECT_FALSE(r.success); }
  { NotifyRestartReq r{};    EXPECT_EQ(r.restarted_site_id, 0u); }
  { NotifyRestartReply r{};  EXPECT_FALSE(r.acknowledged); }
  { InstallSnapshotReq r{};  EXPECT_TRUE(r.data.empty()); }
  { InstallSnapshotReply r{};EXPECT_EQ(r.term_out, 0u); }
  { AddServerReq r{};        EXPECT_EQ(r.new_server_id, 0u); }
  { AddServerReply r{};      EXPECT_FALSE(r.success); }
  { RemoveServerReq r{};     EXPECT_EQ(r.server_id, 0u); }
  { RemoveServerReply r{};   EXPECT_FALSE(r.success); }
}

TEST(RaftMessagesTest, FieldAssignmentRoundTrip) {
  VoteReq req{};
  req.last_log_idx = 42;
  req.last_log_term = 7;
  req.candidate_site_id = 3;
  req.current_term = 9;

  EXPECT_EQ(req.last_log_idx, 42u);
  EXPECT_EQ(req.last_log_term, 7u);
  EXPECT_EQ(req.candidate_site_id, 3u);
  EXPECT_EQ(req.current_term, 9u);

  AppendEntriesReply reply{};
  reply.follower_append_ok = 1;
  reply.follower_current_term = 11;
  reply.follower_last_log_index = 100;
  reply.follower_ack_type = 2;

  EXPECT_EQ(reply.follower_append_ok, 1u);
  EXPECT_EQ(reply.follower_last_log_index, 100u);
}
