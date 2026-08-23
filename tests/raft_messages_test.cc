// Compile-time sanity check that janus::raft::messages.hpp is
// self-contained and rrr-boundary types round-trip cleanly.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#include <gtest/gtest.h>

#include "deptran/raft/messages.hpp"

using namespace janus::raft;

namespace {

// The vote-message preparation and following DSL ownership change must be
// ABI-neutral. These mirrors retain the exact field order and primitive types
// of the former declarations.
struct LegacyVoteReqLayout {
  uint64_t last_log_idx;
  int64_t last_log_term;
  uint16_t candidate_site_id;
  int64_t current_term;
};

struct LegacyVoteReplyLayout {
  int64_t max_ballot;
  bool vote_granted;
};

struct LegacyVoteDurableReqLayout {
  int64_t term;
  uint16_t voter_id;
};

struct LegacyVoteDurableReplyLayout {
  bool acknowledged;
};

static_assert(std::is_aggregate_v<VoteReq>);
static_assert(std::is_aggregate_v<VoteReply>);
static_assert(std::is_aggregate_v<VoteDurableReq>);
static_assert(std::is_aggregate_v<VoteDurableReply>);

static_assert(std::is_standard_layout_v<VoteReq>);
static_assert(std::is_standard_layout_v<VoteReply>);
static_assert(std::is_standard_layout_v<VoteDurableReq>);
static_assert(std::is_standard_layout_v<VoteDurableReply>);

static_assert(std::is_trivially_copyable_v<VoteReq>);
static_assert(std::is_trivially_copyable_v<VoteReply>);
static_assert(std::is_trivially_copyable_v<VoteDurableReq>);
static_assert(std::is_trivially_copyable_v<VoteDurableReply>);

static_assert(
    std::is_same_v<decltype(VoteReq::last_log_idx), uint64_t>);
static_assert(
    std::is_same_v<decltype(VoteReq::last_log_term), int64_t>);
static_assert(
    std::is_same_v<decltype(VoteReq::candidate_site_id), uint16_t>);
static_assert(std::is_same_v<decltype(VoteReq::current_term), int64_t>);
static_assert(std::is_same_v<decltype(VoteReply::max_ballot), int64_t>);
static_assert(std::is_same_v<decltype(VoteReply::vote_granted), bool>);
static_assert(std::is_same_v<decltype(VoteDurableReq::term), int64_t>);
static_assert(
    std::is_same_v<decltype(VoteDurableReq::voter_id), uint16_t>);
static_assert(
    std::is_same_v<decltype(VoteDurableReply::acknowledged), bool>);

static_assert(sizeof(VoteReq) == sizeof(LegacyVoteReqLayout));
static_assert(alignof(VoteReq) == alignof(LegacyVoteReqLayout));
static_assert(offsetof(VoteReq, last_log_idx) ==
              offsetof(LegacyVoteReqLayout, last_log_idx));
static_assert(offsetof(VoteReq, last_log_term) ==
              offsetof(LegacyVoteReqLayout, last_log_term));
static_assert(offsetof(VoteReq, candidate_site_id) ==
              offsetof(LegacyVoteReqLayout, candidate_site_id));
static_assert(offsetof(VoteReq, current_term) ==
              offsetof(LegacyVoteReqLayout, current_term));

static_assert(sizeof(VoteReply) == sizeof(LegacyVoteReplyLayout));
static_assert(alignof(VoteReply) == alignof(LegacyVoteReplyLayout));
static_assert(offsetof(VoteReply, max_ballot) ==
              offsetof(LegacyVoteReplyLayout, max_ballot));
static_assert(offsetof(VoteReply, vote_granted) ==
              offsetof(LegacyVoteReplyLayout, vote_granted));

static_assert(sizeof(VoteDurableReq) == sizeof(LegacyVoteDurableReqLayout));
static_assert(alignof(VoteDurableReq) == alignof(LegacyVoteDurableReqLayout));
static_assert(offsetof(VoteDurableReq, term) ==
              offsetof(LegacyVoteDurableReqLayout, term));
static_assert(offsetof(VoteDurableReq, voter_id) ==
              offsetof(LegacyVoteDurableReqLayout, voter_id));

static_assert(sizeof(VoteDurableReply) ==
              sizeof(LegacyVoteDurableReplyLayout));
static_assert(alignof(VoteDurableReply) ==
              alignof(LegacyVoteDurableReplyLayout));
static_assert(offsetof(VoteDurableReply, acknowledged) ==
              offsetof(LegacyVoteDurableReplyLayout, acknowledged));

}  // namespace

TEST(RaftMessagesTest, DefaultConstructAllRequestReplyTypes) {
  // Smoke test: each struct must be default-constructible and have
  // sensible zero-initialised fields.
  {
    VoteReq r{};
    EXPECT_EQ(r.last_log_idx, 0u);
    EXPECT_EQ(r.candidate_site_id, 0u);
  }
  { VoteReply r{};          EXPECT_FALSE(r.vote_granted); }
  { VoteDurableReq r{};     EXPECT_EQ(r.term, 0); }
  { VoteDurableReply r{};   EXPECT_FALSE(r.acknowledged); }
  {
    auto r = std::make_shared<VoteReply>();
    EXPECT_EQ(r->max_ballot, 0);
    EXPECT_FALSE(r->vote_granted);
  }
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

TEST(RaftMessagesTest, VoteFamilyPreservesPositionalAggregateConstruction) {
  VoteReq vote{42, -7, 3, -9};
  EXPECT_EQ(vote.last_log_idx, 42u);
  EXPECT_EQ(vote.last_log_term, -7);
  EXPECT_EQ(vote.candidate_site_id, 3u);
  EXPECT_EQ(vote.current_term, -9);

  VoteReply reply{-11, true};
  EXPECT_EQ(reply.max_ballot, -11);
  EXPECT_TRUE(reply.vote_granted);

  VoteDurableReq durable{-13, 5};
  EXPECT_EQ(durable.term, -13);
  EXPECT_EQ(durable.voter_id, 5u);

  VoteDurableReply durable_reply{true};
  EXPECT_TRUE(durable_reply.acknowledged);
}
