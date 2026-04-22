// Compile + behavior test for the DispatcherFacade. Defines a trivial
// recording adapter that satisfies every handle_* method and verifies
// each facade method lands on the adapter and its reply callback fires.

#include <gtest/gtest.h>

#include <rusty/arc.hpp>
#include <rusty/sync/atomic.hpp>

#include "deptran/raft/dispatcher.hpp"

using namespace janus::raft;

namespace {

using AtomicInt = rusty::sync::atomic::Atomic<int>;

struct Counts {
  AtomicInt n_vote{0};
  AtomicInt n_vote_durable{0};
  AtomicInt n_append{0};
  AtomicInt n_empty{0};
  AtomicInt n_append_durable{0};
  AtomicInt n_timeout{0};
  AtomicInt n_notify_restart{0};
  AtomicInt n_install_snap{0};
};

struct RecordingDispatcher {
  rusty::Arc<Counts> counts{rusty::Arc<Counts>::make()};

  void handle_vote(VoteReq, OnVoteReplyDispatch cb) {
    counts->n_vote.fetch_add(1);
    VoteReply r{}; r.vote_granted = true; r.max_ballot = 5;
    cb(r);
  }
  void handle_vote_durable(VoteDurableReq, OnVoteDurableReplyDispatch cb) {
    counts->n_vote_durable.fetch_add(1);
    VoteDurableReply r{}; r.acknowledged = true;
    cb(r);
  }
  void handle_append_entries(AppendEntriesReq, OnAppendEntriesReplyDispatch cb) {
    counts->n_append.fetch_add(1);
    AppendEntriesReply r{}; r.follower_append_ok = 1;
    cb(r);
  }
  void handle_empty_append_entries(EmptyAppendEntriesReq,
                                   OnEmptyAppendEntriesReplyDispatch cb) {
    counts->n_empty.fetch_add(1);
    EmptyAppendEntriesReply r{}; r.follower_append_ok = 1;
    cb(r);
  }
  void handle_append_entries_durable(AppendEntriesDurableReq,
                                     OnAppendEntriesDurableReplyDispatch cb) {
    counts->n_append_durable.fetch_add(1);
    AppendEntriesDurableReply r{}; r.acknowledged = true;
    cb(r);
  }
  void handle_timeout_now(TimeoutNowReq, OnTimeoutNowReplyDispatch cb) {
    counts->n_timeout.fetch_add(1);
    TimeoutNowReply r{}; r.success = true;
    cb(r);
  }
  void handle_notify_restart(NotifyRestartReq, OnNotifyRestartReplyDispatch cb) {
    counts->n_notify_restart.fetch_add(1);
    NotifyRestartReply r{}; r.acknowledged = true;
    cb(r);
  }
  void handle_install_snapshot(InstallSnapshotReq,
                               OnInstallSnapshotReplyDispatch cb) {
    counts->n_install_snap.fetch_add(1);
    InstallSnapshotReply r{}; r.term_out = 42;
    cb(r);
  }
};

}  // namespace

TEST(RaftDispatcherFacadeTest, AdapterConformsToFacade) {
  auto adapter = rusty::Arc<RecordingDispatcher>::make();
  DispatcherProxy proxy =
      pro::make_proxy<DispatcherFacade, RecordingDispatcher>(*adapter);

  int replies = 0;
  proxy->handle_vote(VoteReq{},
      [&](VoteReply r) { EXPECT_TRUE(r.vote_granted); ++replies; });
  proxy->handle_vote_durable(VoteDurableReq{},
      [&](VoteDurableReply r) { EXPECT_TRUE(r.acknowledged); ++replies; });
  proxy->handle_append_entries(AppendEntriesReq{},
      [&](AppendEntriesReply) { ++replies; });
  proxy->handle_empty_append_entries(EmptyAppendEntriesReq{},
      [&](EmptyAppendEntriesReply) { ++replies; });
  proxy->handle_append_entries_durable(AppendEntriesDurableReq{},
      [&](AppendEntriesDurableReply) { ++replies; });
  proxy->handle_timeout_now(TimeoutNowReq{},
      [&](TimeoutNowReply) { ++replies; });
  proxy->handle_notify_restart(NotifyRestartReq{},
      [&](NotifyRestartReply) { ++replies; });
  proxy->handle_install_snapshot(InstallSnapshotReq{},
      [&](InstallSnapshotReply r) { EXPECT_EQ(r.term_out, 42u); ++replies; });

  EXPECT_EQ(adapter->counts->n_vote.load(),            1);
  EXPECT_EQ(adapter->counts->n_vote_durable.load(),    1);
  EXPECT_EQ(adapter->counts->n_append.load(),          1);
  EXPECT_EQ(adapter->counts->n_empty.load(),           1);
  EXPECT_EQ(adapter->counts->n_append_durable.load(),  1);
  EXPECT_EQ(adapter->counts->n_timeout.load(),         1);
  EXPECT_EQ(adapter->counts->n_notify_restart.load(),  1);
  EXPECT_EQ(adapter->counts->n_install_snap.load(),    1);
  EXPECT_EQ(replies, 8);
}
