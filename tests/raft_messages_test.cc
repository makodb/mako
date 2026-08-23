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

struct LegacyAppendEntriesReplyLayout {
  uint64_t follower_append_ok;
  uint64_t follower_current_term;
  uint64_t follower_last_log_index;
  uint64_t follower_ack_type;
};

struct LegacyEmptyAppendEntriesReqLayout {
  uint64_t slot;
  int64_t ballot;
  uint64_t leader_current_term;
  uint16_t leader_site_id;
  uint64_t leader_prev_log_index;
  uint64_t leader_prev_log_term;
  uint64_t leader_commit_index;
  bool trigger_election_now;
};

struct LegacyEmptyAppendEntriesReplyLayout {
  uint64_t follower_append_ok;
  uint64_t follower_current_term;
  uint64_t follower_last_log_index;
  uint64_t follower_ack_type;
};

struct LegacyAppendEntriesDurableReqLayout {
  int64_t term;
  uint16_t follower_id;
  uint64_t last_log_index;
};

struct LegacyAppendEntriesDurableReplyLayout {
  bool acknowledged;
};

struct LegacyTimeoutNowReqLayout {
  uint64_t leader_term;
  uint16_t leader_site_id;
};

struct LegacyTimeoutNowReplyLayout {
  uint64_t follower_term;
  bool success;
};

struct LegacyNotifyRestartReqLayout {
  uint16_t restarted_site_id;
};

struct LegacyNotifyRestartReplyLayout {
  bool acknowledged;
};

struct LegacyInstallSnapshotReplyLayout {
  uint64_t term_out;
};

struct LegacyRemoveServerReqLayout {
  uint64_t term;
  uint64_t server_id;
};

#define ASSERT_POD_LAYOUT(type, legacy)                  \
  static_assert(std::is_aggregate_v<type>);              \
  static_assert(std::is_standard_layout_v<type>);        \
  static_assert(std::is_trivially_copyable_v<type>);     \
  static_assert(sizeof(type) == sizeof(legacy));          \
  static_assert(alignof(type) == alignof(legacy))

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

ASSERT_POD_LAYOUT(AppendEntriesReply, LegacyAppendEntriesReplyLayout);
ASSERT_POD_LAYOUT(EmptyAppendEntriesReq, LegacyEmptyAppendEntriesReqLayout);
ASSERT_POD_LAYOUT(EmptyAppendEntriesReply,
                  LegacyEmptyAppendEntriesReplyLayout);
ASSERT_POD_LAYOUT(AppendEntriesDurableReq,
                  LegacyAppendEntriesDurableReqLayout);
ASSERT_POD_LAYOUT(AppendEntriesDurableReply,
                  LegacyAppendEntriesDurableReplyLayout);
ASSERT_POD_LAYOUT(TimeoutNowReq, LegacyTimeoutNowReqLayout);
ASSERT_POD_LAYOUT(TimeoutNowReply, LegacyTimeoutNowReplyLayout);
ASSERT_POD_LAYOUT(NotifyRestartReq, LegacyNotifyRestartReqLayout);
ASSERT_POD_LAYOUT(NotifyRestartReply, LegacyNotifyRestartReplyLayout);
ASSERT_POD_LAYOUT(InstallSnapshotReply, LegacyInstallSnapshotReplyLayout);
ASSERT_POD_LAYOUT(RemoveServerReq, LegacyRemoveServerReqLayout);

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
static_assert(std::is_same_v<decltype(AppendEntriesReply::follower_append_ok),
                             uint64_t>);
static_assert(
    std::is_same_v<decltype(AppendEntriesReply::follower_current_term),
                   uint64_t>);
static_assert(
    std::is_same_v<decltype(AppendEntriesReply::follower_last_log_index),
                   uint64_t>);
static_assert(std::is_same_v<decltype(AppendEntriesReply::follower_ack_type),
                             uint64_t>);
static_assert(
    std::is_same_v<decltype(EmptyAppendEntriesReq::slot), uint64_t>);
static_assert(
    std::is_same_v<decltype(EmptyAppendEntriesReq::ballot), int64_t>);
static_assert(std::is_same_v<decltype(EmptyAppendEntriesReq::leader_site_id),
                             uint16_t>);
static_assert(
    std::is_same_v<decltype(EmptyAppendEntriesReq::trigger_election_now),
                   bool>);
static_assert(std::is_same_v<
              decltype(EmptyAppendEntriesReply::follower_ack_type), uint64_t>);
static_assert(
    std::is_same_v<decltype(AppendEntriesDurableReq::term), int64_t>);
static_assert(std::is_same_v<decltype(AppendEntriesDurableReq::follower_id),
                             uint16_t>);
static_assert(std::is_same_v<decltype(AppendEntriesDurableReq::last_log_index),
                             uint64_t>);
static_assert(std::is_same_v<
              decltype(AppendEntriesDurableReply::acknowledged), bool>);
static_assert(
    std::is_same_v<decltype(TimeoutNowReq::leader_term), uint64_t>);
static_assert(
    std::is_same_v<decltype(TimeoutNowReq::leader_site_id), uint16_t>);
static_assert(
    std::is_same_v<decltype(TimeoutNowReply::follower_term), uint64_t>);
static_assert(std::is_same_v<decltype(TimeoutNowReply::success), bool>);
static_assert(std::is_same_v<decltype(NotifyRestartReq::restarted_site_id),
                             uint16_t>);
static_assert(
    std::is_same_v<decltype(NotifyRestartReply::acknowledged), bool>);
static_assert(
    std::is_same_v<decltype(InstallSnapshotReply::term_out), uint64_t>);
static_assert(std::is_same_v<decltype(RemoveServerReq::term), uint64_t>);
static_assert(
    std::is_same_v<decltype(RemoveServerReq::server_id), uint64_t>);

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

#define ASSERT_FIELD_OFFSET(type, legacy, field) \
  static_assert(offsetof(type, field) == offsetof(legacy, field))

ASSERT_FIELD_OFFSET(AppendEntriesReply, LegacyAppendEntriesReplyLayout,
                    follower_append_ok);
ASSERT_FIELD_OFFSET(AppendEntriesReply, LegacyAppendEntriesReplyLayout,
                    follower_current_term);
ASSERT_FIELD_OFFSET(AppendEntriesReply, LegacyAppendEntriesReplyLayout,
                    follower_last_log_index);
ASSERT_FIELD_OFFSET(AppendEntriesReply, LegacyAppendEntriesReplyLayout,
                    follower_ack_type);
ASSERT_FIELD_OFFSET(EmptyAppendEntriesReq, LegacyEmptyAppendEntriesReqLayout,
                    slot);
ASSERT_FIELD_OFFSET(EmptyAppendEntriesReq, LegacyEmptyAppendEntriesReqLayout,
                    ballot);
ASSERT_FIELD_OFFSET(EmptyAppendEntriesReq, LegacyEmptyAppendEntriesReqLayout,
                    leader_current_term);
ASSERT_FIELD_OFFSET(EmptyAppendEntriesReq, LegacyEmptyAppendEntriesReqLayout,
                    leader_site_id);
ASSERT_FIELD_OFFSET(EmptyAppendEntriesReq, LegacyEmptyAppendEntriesReqLayout,
                    leader_prev_log_index);
ASSERT_FIELD_OFFSET(EmptyAppendEntriesReq, LegacyEmptyAppendEntriesReqLayout,
                    leader_prev_log_term);
ASSERT_FIELD_OFFSET(EmptyAppendEntriesReq, LegacyEmptyAppendEntriesReqLayout,
                    leader_commit_index);
ASSERT_FIELD_OFFSET(EmptyAppendEntriesReq, LegacyEmptyAppendEntriesReqLayout,
                    trigger_election_now);
ASSERT_FIELD_OFFSET(EmptyAppendEntriesReply,
                    LegacyEmptyAppendEntriesReplyLayout, follower_append_ok);
ASSERT_FIELD_OFFSET(EmptyAppendEntriesReply,
                    LegacyEmptyAppendEntriesReplyLayout,
                    follower_current_term);
ASSERT_FIELD_OFFSET(EmptyAppendEntriesReply,
                    LegacyEmptyAppendEntriesReplyLayout,
                    follower_last_log_index);
ASSERT_FIELD_OFFSET(EmptyAppendEntriesReply,
                    LegacyEmptyAppendEntriesReplyLayout, follower_ack_type);
ASSERT_FIELD_OFFSET(AppendEntriesDurableReq,
                    LegacyAppendEntriesDurableReqLayout, term);
ASSERT_FIELD_OFFSET(AppendEntriesDurableReq,
                    LegacyAppendEntriesDurableReqLayout, follower_id);
ASSERT_FIELD_OFFSET(AppendEntriesDurableReq,
                    LegacyAppendEntriesDurableReqLayout, last_log_index);
ASSERT_FIELD_OFFSET(AppendEntriesDurableReply,
                    LegacyAppendEntriesDurableReplyLayout, acknowledged);
ASSERT_FIELD_OFFSET(TimeoutNowReq, LegacyTimeoutNowReqLayout, leader_term);
ASSERT_FIELD_OFFSET(TimeoutNowReq, LegacyTimeoutNowReqLayout, leader_site_id);
ASSERT_FIELD_OFFSET(TimeoutNowReply, LegacyTimeoutNowReplyLayout,
                    follower_term);
ASSERT_FIELD_OFFSET(TimeoutNowReply, LegacyTimeoutNowReplyLayout, success);
ASSERT_FIELD_OFFSET(NotifyRestartReq, LegacyNotifyRestartReqLayout,
                    restarted_site_id);
ASSERT_FIELD_OFFSET(NotifyRestartReply, LegacyNotifyRestartReplyLayout,
                    acknowledged);
ASSERT_FIELD_OFFSET(InstallSnapshotReply, LegacyInstallSnapshotReplyLayout,
                    term_out);
ASSERT_FIELD_OFFSET(RemoveServerReq, LegacyRemoveServerReqLayout, term);
ASSERT_FIELD_OFFSET(RemoveServerReq, LegacyRemoveServerReqLayout, server_id);

#undef ASSERT_FIELD_OFFSET
#undef ASSERT_POD_LAYOUT

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

TEST(RaftMessagesTest, PrimitiveFamiliesPreserveValueInitialization) {
  auto append = std::make_shared<AppendEntriesReply>();
  EXPECT_EQ(append->follower_append_ok, 0u);
  EXPECT_EQ(append->follower_current_term, 0u);
  EXPECT_EQ(append->follower_last_log_index, 0u);
  EXPECT_EQ(append->follower_ack_type, 0u);

  auto timeout = std::make_shared<TimeoutNowReply>();
  EXPECT_EQ(timeout->follower_term, 0u);
  EXPECT_FALSE(timeout->success);

  auto snapshot = std::make_shared<InstallSnapshotReply>();
  EXPECT_EQ(snapshot->term_out, 0u);
}

TEST(RaftMessagesTest, PrimitiveFamiliesPreservePositionalConstruction) {
  AppendEntriesReply append{1, 2, 3, 4};
  EXPECT_EQ(append.follower_append_ok, 1u);
  EXPECT_EQ(append.follower_current_term, 2u);
  EXPECT_EQ(append.follower_last_log_index, 3u);
  EXPECT_EQ(append.follower_ack_type, 4u);

  EmptyAppendEntriesReq heartbeat{5, -6, 7, 8, 9, 10, 11, true};
  EXPECT_EQ(heartbeat.slot, 5u);
  EXPECT_EQ(heartbeat.ballot, -6);
  EXPECT_EQ(heartbeat.leader_site_id, 8u);
  EXPECT_TRUE(heartbeat.trigger_election_now);

  EmptyAppendEntriesReply heartbeat_reply{12, 13, 14, 15};
  EXPECT_EQ(heartbeat_reply.follower_last_log_index, 14u);

  AppendEntriesDurableReq durable{-16, 17, 18};
  EXPECT_EQ(durable.term, -16);
  EXPECT_EQ(durable.follower_id, 17u);
  EXPECT_EQ(durable.last_log_index, 18u);

  AppendEntriesDurableReply durable_reply{true};
  EXPECT_TRUE(durable_reply.acknowledged);

  TimeoutNowReq timeout{19, 20};
  TimeoutNowReply timeout_reply{21, true};
  EXPECT_EQ(timeout.leader_site_id, 20u);
  EXPECT_EQ(timeout_reply.follower_term, 21u);
  EXPECT_TRUE(timeout_reply.success);

  NotifyRestartReq restart{22};
  NotifyRestartReply restart_reply{true};
  EXPECT_EQ(restart.restarted_site_id, 22u);
  EXPECT_TRUE(restart_reply.acknowledged);

  InstallSnapshotReply snapshot{23};
  EXPECT_EQ(snapshot.term_out, 23u);

  RemoveServerReq remove{24, 25};
  EXPECT_EQ(remove.term, 24u);
  EXPECT_EQ(remove.server_id, 25u);
}
