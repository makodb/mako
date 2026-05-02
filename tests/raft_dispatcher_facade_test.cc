// Compile + behavior test for the DispatcherFacade. Fiber-synchronous
// since Phase 8.0: each handle_* returns a Reply by value.

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
  AtomicInt n_add_server{0};
  AtomicInt n_remove_server{0};
};

struct RecordingDispatcher {
  rusty::Arc<Counts> counts{rusty::Arc<Counts>::make()};

  VoteReply handle_vote(VoteReq) {
    counts->n_vote.fetch_add(1);
    VoteReply r{}; r.vote_granted = true; r.max_ballot = 5; return r;
  }
  VoteDurableReply handle_vote_durable(VoteDurableReq) {
    counts->n_vote_durable.fetch_add(1);
    VoteDurableReply r{}; r.acknowledged = true; return r;
  }
  AppendEntriesReply handle_append_entries(AppendEntriesReq) {
    counts->n_append.fetch_add(1);
    AppendEntriesReply r{}; r.follower_append_ok = 1; return r;
  }
  EmptyAppendEntriesReply handle_empty_append_entries(EmptyAppendEntriesReq) {
    counts->n_empty.fetch_add(1);
    EmptyAppendEntriesReply r{}; r.follower_append_ok = 1; return r;
  }
  AppendEntriesDurableReply handle_append_entries_durable(AppendEntriesDurableReq) {
    counts->n_append_durable.fetch_add(1);
    AppendEntriesDurableReply r{}; r.acknowledged = true; return r;
  }
  TimeoutNowReply handle_timeout_now(TimeoutNowReq) {
    counts->n_timeout.fetch_add(1);
    TimeoutNowReply r{}; r.success = true; return r;
  }
  NotifyRestartReply handle_notify_restart(NotifyRestartReq) {
    counts->n_notify_restart.fetch_add(1);
    NotifyRestartReply r{}; r.acknowledged = true; return r;
  }
  InstallSnapshotReply handle_install_snapshot(InstallSnapshotReq) {
    counts->n_install_snap.fetch_add(1);
    InstallSnapshotReply r{}; r.term_out = 42; return r;
  }
  AddServerReply handle_add_server(AddServerReq) {
    counts->n_add_server.fetch_add(1);
    AddServerReply r{}; r.success = true; r.leader_hint = 7; return r;
  }
  RemoveServerReply handle_remove_server(RemoveServerReq) {
    counts->n_remove_server.fetch_add(1);
    RemoveServerReply r{}; r.success = true; r.leader_hint = 9; return r;
  }
};

}  // namespace

TEST(RaftDispatcherFacadeTest, AdapterConformsToFacade) {
  auto adapter = rusty::Arc<RecordingDispatcher>::make();
  DispatcherProxy proxy =
      pro::make_proxy<DispatcherFacade, RecordingDispatcher>(*adapter);

  auto v  = proxy->handle_vote(VoteReq{});
  EXPECT_TRUE(v.vote_granted);
  auto vd = proxy->handle_vote_durable(VoteDurableReq{});
  EXPECT_TRUE(vd.acknowledged);
  auto a  = proxy->handle_append_entries(AppendEntriesReq{});
  EXPECT_EQ(a.follower_append_ok, 1u);
  auto e  = proxy->handle_empty_append_entries(EmptyAppendEntriesReq{});
  EXPECT_EQ(e.follower_append_ok, 1u);
  auto ad = proxy->handle_append_entries_durable(AppendEntriesDurableReq{});
  EXPECT_TRUE(ad.acknowledged);
  auto tn = proxy->handle_timeout_now(TimeoutNowReq{});
  EXPECT_TRUE(tn.success);
  auto nr = proxy->handle_notify_restart(NotifyRestartReq{});
  EXPECT_TRUE(nr.acknowledged);
  auto is = proxy->handle_install_snapshot(InstallSnapshotReq{});
  EXPECT_EQ(is.term_out, 42u);
  auto add = proxy->handle_add_server(AddServerReq{});
  EXPECT_TRUE(add.success);
  EXPECT_EQ(add.leader_hint, 7u);
  auto rem = proxy->handle_remove_server(RemoveServerReq{});
  EXPECT_TRUE(rem.success);
  EXPECT_EQ(rem.leader_hint, 9u);

  EXPECT_EQ(adapter->counts->n_vote.load(),            1);
  EXPECT_EQ(adapter->counts->n_vote_durable.load(),    1);
  EXPECT_EQ(adapter->counts->n_append.load(),          1);
  EXPECT_EQ(adapter->counts->n_empty.load(),           1);
  EXPECT_EQ(adapter->counts->n_append_durable.load(),  1);
  EXPECT_EQ(adapter->counts->n_timeout.load(),         1);
  EXPECT_EQ(adapter->counts->n_notify_restart.load(),  1);
  EXPECT_EQ(adapter->counts->n_install_snap.load(),    1);
  EXPECT_EQ(adapter->counts->n_add_server.load(),      1);
  EXPECT_EQ(adapter->counts->n_remove_server.load(),   1);
}
