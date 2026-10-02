// Compile-time sanity check that janus::raft::messages.hpp is
// self-contained and srpc-boundary types round-trip cleanly.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <memory>
#include <type_traits>

#include <gtest/gtest.h>

#include "deptran/raft/messages.hpp"

using namespace janus::raft;

namespace {

// Keep the RPC payload ABI independent of typedef spelling. These mirrors pin
// the exact field order and primitive representations.
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

struct LegacyAppendEntriesReplyLayout {
  uint64_t follower_append_ok;
  uint64_t follower_current_term;
  uint64_t follower_last_log_index;
};

struct LegacyEmptyAppendEntriesReqLayout {
  uint64_t slot;
  int64_t ballot;
  uint64_t leader_current_term;
  uint16_t leader_site_id;
  uint64_t leader_prev_log_index;
  uint64_t leader_prev_log_term;
  uint64_t leader_commit_index;
};

struct LegacyEmptyAppendEntriesReplyLayout {
  uint64_t follower_append_ok;
  uint64_t follower_current_term;
  uint64_t follower_last_log_index;
};

struct LegacyInstallSnapshotReplyLayout {
  uint64_t term_out;
};

#define ASSERT_POD_LAYOUT(type, legacy)                  \
  static_assert(std::is_aggregate_v<type>);              \
  static_assert(std::is_standard_layout_v<type>);        \
  static_assert(std::is_trivially_copyable_v<type>);     \
  static_assert(sizeof(type) == sizeof(legacy));          \
  static_assert(alignof(type) == alignof(legacy))

// These structs carry no default member initializers. The transpiler branch
// srpc needs has no value-init mechanism, so the guarantee that their members
// start at zero rests entirely on every C++ site spelling `T{}` rather than
// `T x;`. scripts/raft_field_census.py enforces that spelling; the assertions
// here pin what `T{}` is then obliged to do, which holds only for a trivial
// aggregate.
#define ASSERT_ZERO_DEFAULT_CONTRACT(type)                        \
  static_assert(std::is_default_constructible_v<type>);           \
  static_assert(std::is_aggregate_v<type>);                       \
  static_assert(std::is_trivially_default_constructible_v<type>)

static_assert(std::is_aggregate_v<VoteReq>);
static_assert(std::is_aggregate_v<VoteReply>);

static_assert(std::is_standard_layout_v<VoteReq>);
static_assert(std::is_standard_layout_v<VoteReply>);

static_assert(std::is_trivially_copyable_v<VoteReq>);
static_assert(std::is_trivially_copyable_v<VoteReply>);

ASSERT_POD_LAYOUT(AppendEntriesReply, LegacyAppendEntriesReplyLayout);
ASSERT_POD_LAYOUT(EmptyAppendEntriesReq, LegacyEmptyAppendEntriesReqLayout);
ASSERT_POD_LAYOUT(EmptyAppendEntriesReply,
                  LegacyEmptyAppendEntriesReplyLayout);
ASSERT_POD_LAYOUT(InstallSnapshotReply, LegacyInstallSnapshotReplyLayout);

ASSERT_ZERO_DEFAULT_CONTRACT(VoteReq);
ASSERT_ZERO_DEFAULT_CONTRACT(VoteReply);
ASSERT_ZERO_DEFAULT_CONTRACT(AppendEntriesReply);
ASSERT_ZERO_DEFAULT_CONTRACT(EmptyAppendEntriesReq);
ASSERT_ZERO_DEFAULT_CONTRACT(EmptyAppendEntriesReply);
ASSERT_ZERO_DEFAULT_CONTRACT(InstallSnapshotReply);

static_assert(
    std::is_same_v<decltype(VoteReq::last_log_idx), uint64_t>);
static_assert(
    std::is_same_v<decltype(VoteReq::last_log_term), int64_t>);
static_assert(
    std::is_same_v<decltype(VoteReq::candidate_site_id), uint16_t>);
static_assert(std::is_same_v<decltype(VoteReq::current_term), int64_t>);
static_assert(std::is_same_v<decltype(VoteReply::max_ballot), int64_t>);
static_assert(std::is_same_v<decltype(VoteReply::vote_granted), bool>);
static_assert(std::is_same_v<decltype(AppendEntriesReply::follower_append_ok),
                             uint64_t>);
static_assert(
    std::is_same_v<decltype(AppendEntriesReply::follower_current_term),
                   uint64_t>);
static_assert(
    std::is_same_v<decltype(AppendEntriesReply::follower_last_log_index),
                   uint64_t>);
static_assert(
    std::is_same_v<decltype(EmptyAppendEntriesReq::slot), uint64_t>);
static_assert(
    std::is_same_v<decltype(EmptyAppendEntriesReq::ballot), int64_t>);
static_assert(std::is_same_v<decltype(EmptyAppendEntriesReq::leader_site_id),
                             uint16_t>);
static_assert(
    std::is_same_v<decltype(InstallSnapshotReply::term_out), uint64_t>);

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

#define ASSERT_FIELD_OFFSET(type, legacy, field) \
  static_assert(offsetof(type, field) == offsetof(legacy, field))

ASSERT_FIELD_OFFSET(AppendEntriesReply, LegacyAppendEntriesReplyLayout,
                    follower_append_ok);
ASSERT_FIELD_OFFSET(AppendEntriesReply, LegacyAppendEntriesReplyLayout,
                    follower_current_term);
ASSERT_FIELD_OFFSET(AppendEntriesReply, LegacyAppendEntriesReplyLayout,
                    follower_last_log_index);
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
ASSERT_FIELD_OFFSET(EmptyAppendEntriesReply,
                    LegacyEmptyAppendEntriesReplyLayout, follower_append_ok);
ASSERT_FIELD_OFFSET(EmptyAppendEntriesReply,
                    LegacyEmptyAppendEntriesReplyLayout,
                    follower_current_term);
ASSERT_FIELD_OFFSET(EmptyAppendEntriesReply,
                    LegacyEmptyAppendEntriesReplyLayout,
                    follower_last_log_index);
ASSERT_FIELD_OFFSET(InstallSnapshotReply, LegacyInstallSnapshotReplyLayout,
                    term_out);

#undef ASSERT_FIELD_OFFSET
#undef ASSERT_POD_LAYOUT
#undef ASSERT_ZERO_DEFAULT_CONTRACT

// Value-initialization, `T()`, zero-initializes the whole object, padding
// included ([dcl.init]: a class with no user-provided constructor is
// zero-initialized first). Brace initialization of an aggregate, `T{}`, is
// aggregate initialization instead: it zeroes every member but leaves padding
// unspecified, and clang 22 at -O2 does leave it untouched. The member
// guarantee for `T{}` is BraceInitializationPreservesZeroContract below; no
// wire path depends on padding (the Rust lane encodes field by field,
// rt/src/rpc.rs). Constructing into 0xFF-filled storage proves the byte
// guarantee rather than trusting a stack frame that happened to arrive zeroed.
template <typename T>
void ExpectValueInitZeroesEveryByte(const char* name) {
  alignas(T) unsigned char storage[sizeof(T)];
  std::memset(storage, 0xFF, sizeof(storage));
  T* value = ::new (static_cast<void*>(storage)) T();
  for (size_t i = 0; i < sizeof(T); ++i) {
    EXPECT_EQ(storage[i], 0u) << name << " byte " << i << " survived 0xFF fill";
  }
  value->~T();
}

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
  {
    auto r = std::make_shared<VoteReply>();
    EXPECT_EQ(r->max_ballot, 0);
    EXPECT_FALSE(r->vote_granted);
  }
  { AppendEntriesReq r{};   EXPECT_EQ(r.leader_commit_index, 0u); }
  { AppendEntriesReply r{}; EXPECT_EQ(r.follower_append_ok, 0u); }
  { EmptyAppendEntriesReq r{};   EXPECT_EQ(r.leader_commit_index, 0u); }
  { EmptyAppendEntriesReply r{}; EXPECT_EQ(r.follower_last_log_index, 0u); }
  { InstallSnapshotReq r{};  EXPECT_TRUE(r.data.empty()); }
  { InstallSnapshotReply r{};EXPECT_EQ(r.term_out, 0u); }
}

TEST(RaftMessagesTest, ValueInitializationZeroesEveryByte) {
  ExpectValueInitZeroesEveryByte<VoteReq>("VoteReq");
  ExpectValueInitZeroesEveryByte<VoteReply>("VoteReply");
  ExpectValueInitZeroesEveryByte<AppendEntriesReply>("AppendEntriesReply");
  ExpectValueInitZeroesEveryByte<EmptyAppendEntriesReq>("EmptyAppendEntriesReq");
  ExpectValueInitZeroesEveryByte<EmptyAppendEntriesReply>(
      "EmptyAppendEntriesReply");
  ExpectValueInitZeroesEveryByte<InstallSnapshotReply>("InstallSnapshotReply");
}

TEST(RaftMessagesTest, BraceInitializationPreservesZeroContract) {
  VoteReq vote{};
  EXPECT_EQ(vote.last_log_idx, 0u);
  EXPECT_EQ(vote.last_log_term, 0);
  EXPECT_EQ(vote.candidate_site_id, 0u);
  EXPECT_EQ(vote.current_term, 0);

  VoteReply vote_reply{};
  EXPECT_EQ(vote_reply.max_ballot, 0);
  EXPECT_FALSE(vote_reply.vote_granted);

  AppendEntriesReply append{};
  EXPECT_EQ(append.follower_append_ok, 0u);
  EXPECT_EQ(append.follower_current_term, 0u);
  EXPECT_EQ(append.follower_last_log_index, 0u);

  EmptyAppendEntriesReq heartbeat{};
  EXPECT_EQ(heartbeat.slot, 0u);
  EXPECT_EQ(heartbeat.ballot, 0);
  EXPECT_EQ(heartbeat.leader_current_term, 0u);
  EXPECT_EQ(heartbeat.leader_site_id, 0u);
  EXPECT_EQ(heartbeat.leader_prev_log_index, 0u);
  EXPECT_EQ(heartbeat.leader_prev_log_term, 0u);
  EXPECT_EQ(heartbeat.leader_commit_index, 0u);

  EmptyAppendEntriesReply heartbeat_reply{};
  EXPECT_EQ(heartbeat_reply.follower_append_ok, 0u);
  EXPECT_EQ(heartbeat_reply.follower_current_term, 0u);
  EXPECT_EQ(heartbeat_reply.follower_last_log_index, 0u);

  InstallSnapshotReply snapshot_reply{};
  EXPECT_EQ(snapshot_reply.term_out, 0u);
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
}

TEST(RaftMessagesTest, PrimitiveFamiliesPreserveValueInitialization) {
  auto append = std::make_shared<AppendEntriesReply>();
  EXPECT_EQ(append->follower_append_ok, 0u);
  EXPECT_EQ(append->follower_current_term, 0u);
  EXPECT_EQ(append->follower_last_log_index, 0u);

  auto snapshot = std::make_shared<InstallSnapshotReply>();
  EXPECT_EQ(snapshot->term_out, 0u);
}

TEST(RaftMessagesTest, PrimitiveFamiliesPreservePositionalConstruction) {
  AppendEntriesReply append{1, 2, 3};
  EXPECT_EQ(append.follower_append_ok, 1u);
  EXPECT_EQ(append.follower_current_term, 2u);
  EXPECT_EQ(append.follower_last_log_index, 3u);

  EmptyAppendEntriesReq heartbeat{5, -6, 7, 8, 9, 10, 11};
  EXPECT_EQ(heartbeat.slot, 5u);
  EXPECT_EQ(heartbeat.ballot, -6);
  EXPECT_EQ(heartbeat.leader_site_id, 8u);
  EXPECT_EQ(heartbeat.leader_commit_index, 11u);

  EmptyAppendEntriesReply heartbeat_reply{12, 13, 14};
  EXPECT_EQ(heartbeat_reply.follower_last_log_index, 14u);

  InstallSnapshotReply snapshot{23};
  EXPECT_EQ(snapshot.term_out, 23u);
}
