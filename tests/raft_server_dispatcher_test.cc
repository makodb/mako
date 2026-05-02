#include <gtest/gtest.h>

#include <type_traits>

#include "deptran/raft/dispatcher.hpp"
#include "deptran/raft/raft_server_dispatcher.hpp"

using namespace janus::raft;

TEST(RaftServerDispatcherTest, NullServerReturnsServiceCompatibleDefaults) {
  janus::raft::RaftServerDispatcher disp(nullptr);

  VoteReq vote_req{};
  vote_req.current_term = 17;
  auto vote = disp.handle_vote(vote_req);
  EXPECT_EQ(vote.max_ballot, 17u);
  EXPECT_FALSE(vote.vote_granted);

  auto vote_durable = disp.handle_vote_durable(VoteDurableReq{});
  EXPECT_FALSE(vote_durable.acknowledged);

  auto append = disp.handle_append_entries(AppendEntriesReq{});
  EXPECT_EQ(append.follower_append_ok, 0u);
  EXPECT_EQ(append.follower_current_term, 0u);
  EXPECT_EQ(append.follower_last_log_index, 0u);
  EXPECT_EQ(append.follower_ack_type, 0u);

  auto empty = disp.handle_empty_append_entries(EmptyAppendEntriesReq{});
  EXPECT_EQ(empty.follower_append_ok, 0u);
  EXPECT_EQ(empty.follower_current_term, 0u);
  EXPECT_EQ(empty.follower_last_log_index, 0u);
  EXPECT_EQ(empty.follower_ack_type, 0u);

  auto append_durable =
      disp.handle_append_entries_durable(AppendEntriesDurableReq{});
  EXPECT_FALSE(append_durable.acknowledged);

  auto timeout = disp.handle_timeout_now(TimeoutNowReq{});
  EXPECT_EQ(timeout.follower_term, 0u);
  EXPECT_FALSE(timeout.success);

  auto restart = disp.handle_notify_restart(NotifyRestartReq{});
  EXPECT_FALSE(restart.acknowledged);

  auto snapshot = disp.handle_install_snapshot(InstallSnapshotReq{});
  EXPECT_EQ(snapshot.term_out, 0u);
}

TEST(RaftServerDispatcherTest, FactoryBuildsDispatcherProxy) {
  auto proxy = make_raft_server_dispatcher(nullptr);
  static_assert(
      std::is_same<decltype(proxy), DispatcherProxy>::value,
      "make_raft_server_dispatcher must return DispatcherProxy");

  VoteReq req{};
  req.current_term = 23;
  auto vote = proxy->handle_vote(req);
  EXPECT_EQ(vote.max_ballot, 23u);
  EXPECT_FALSE(vote.vote_granted);

  auto install = proxy->handle_install_snapshot(InstallSnapshotReq{});
  EXPECT_EQ(install.term_out, 0u);
}
