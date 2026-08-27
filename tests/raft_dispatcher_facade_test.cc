// Compile + behavior test for DispatcherBase. Fiber-synchronous
// since Phase 8.0: each handle_* returns a Reply by value.

#include <gtest/gtest.h>

#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/sync/atomic.hpp>

#include "deptran/raft/dispatcher.hpp"

using namespace janus::raft;

namespace {

using AtomicInt = rusty::sync::atomic::detail::Atomic<int>;

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

class RecordingDispatcher : public DispatcherBase {
 public:
  rusty::Arc<Counts> counts{rusty::Arc<Counts>::make()};

  VoteReply handle_vote(VoteReq) override {
    counts->n_vote.fetch_add(1);
    VoteReply r{}; r.vote_granted = true; r.max_ballot = 5; return r;
  }
  VoteDurableReply handle_vote_durable(VoteDurableReq) override {
    counts->n_vote_durable.fetch_add(1);
    VoteDurableReply r{}; r.acknowledged = true; return r;
  }
  AppendEntriesReply handle_append_entries(AppendEntriesReq) override {
    counts->n_append.fetch_add(1);
    AppendEntriesReply r{}; r.follower_append_ok = 1; return r;
  }
  EmptyAppendEntriesReply handle_empty_append_entries(EmptyAppendEntriesReq) override {
    counts->n_empty.fetch_add(1);
    EmptyAppendEntriesReply r{}; r.follower_append_ok = 1; return r;
  }
  AppendEntriesDurableReply handle_append_entries_durable(AppendEntriesDurableReq) override {
    counts->n_append_durable.fetch_add(1);
    AppendEntriesDurableReply r{}; r.acknowledged = true; return r;
  }
  TimeoutNowReply handle_timeout_now(TimeoutNowReq) override {
    counts->n_timeout.fetch_add(1);
    TimeoutNowReply r{}; r.success = true; return r;
  }
  NotifyRestartReply handle_notify_restart(NotifyRestartReq) override {
    counts->n_notify_restart.fetch_add(1);
    NotifyRestartReply r{}; r.acknowledged = true; return r;
  }
  InstallSnapshotReply handle_install_snapshot(InstallSnapshotReq) override {
    counts->n_install_snap.fetch_add(1);
    InstallSnapshotReply r{}; r.term_out = 42; return r;
  }
};

}  // namespace

TEST(RaftDispatcherFacadeTest, AdapterConformsToFacade) {
  auto* raw = new RecordingDispatcher();
  rusty::Arc<Counts> counts_handle = raw->counts;
  DispatcherProxy proxy(raw);

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

  EXPECT_EQ(counts_handle->n_vote.load(),            1);
  EXPECT_EQ(counts_handle->n_vote_durable.load(),    1);
  EXPECT_EQ(counts_handle->n_append.load(),          1);
  EXPECT_EQ(counts_handle->n_empty.load(),           1);
  EXPECT_EQ(counts_handle->n_append_durable.load(),  1);
  EXPECT_EQ(counts_handle->n_timeout.load(),         1);
  EXPECT_EQ(counts_handle->n_notify_restart.load(),  1);
  EXPECT_EQ(counts_handle->n_install_snap.load(),    1);
}
