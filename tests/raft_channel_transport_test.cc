// Phase 4 smoke test: two sites send RPCs through a ChannelSwitchboard
// to a recording dispatcher. Verifies that the facade + adapter + worker
// round-trip correctly, and that drop_direction blocks traffic in one
// direction while still allowing the other.

#include <gtest/gtest.h>

#include <rusty/arc.hpp>
#include <rusty/sync/atomic.hpp>

#include "deptran/raft/channel_transport.hpp"

using namespace janus::raft;

namespace {

using AtomicInt = rusty::sync::atomic::Atomic<int>;

struct Counts {
  AtomicInt n_append{0};
  AtomicInt n_vote{0};
  AtomicInt n_timeout{0};
  AtomicInt n_vote_durable{0};
  AtomicInt n_install{0};
};

struct RecordingDispatcher {
  rusty::Arc<Counts> counts{rusty::Arc<Counts>::make()};

  void handle_vote(VoteReq, OnVoteReplyDispatch cb) {
    counts->n_vote.fetch_add(1);
    VoteReply r{}; r.vote_granted = true;
    cb(std::move(r));
  }
  void handle_vote_durable(VoteDurableReq, OnVoteDurableReplyDispatch cb) {
    counts->n_vote_durable.fetch_add(1);
    cb(VoteDurableReply{});
  }
  void handle_append_entries(AppendEntriesReq, OnAppendEntriesReplyDispatch cb) {
    counts->n_append.fetch_add(1);
    AppendEntriesReply r{}; r.follower_append_ok = 1;
    cb(std::move(r));
  }
  void handle_empty_append_entries(EmptyAppendEntriesReq,
                                   OnEmptyAppendEntriesReplyDispatch cb) {
    counts->n_append.fetch_add(1);
    EmptyAppendEntriesReply r{}; r.follower_append_ok = 1;
    cb(std::move(r));
  }
  void handle_append_entries_durable(AppendEntriesDurableReq,
                                     OnAppendEntriesDurableReplyDispatch cb) {
    cb(AppendEntriesDurableReply{});
  }
  void handle_timeout_now(TimeoutNowReq, OnTimeoutNowReplyDispatch cb) {
    counts->n_timeout.fetch_add(1);
    TimeoutNowReply r{}; r.success = true;
    cb(std::move(r));
  }
  void handle_notify_restart(NotifyRestartReq, OnNotifyRestartReplyDispatch cb) {
    cb(NotifyRestartReply{});
  }
  void handle_install_snapshot(InstallSnapshotReq,
                               OnInstallSnapshotReplyDispatch cb) {
    counts->n_install.fetch_add(1);
    InstallSnapshotReply r{}; r.term_out = 7;
    cb(std::move(r));
  }
};

}  // namespace

TEST(RaftChannelTransportTest, RoundTripBetweenTwoSites) {
  ChannelSwitchboard sw;
  auto rx_a = sw.register_site(1);
  auto rx_b = sw.register_site(2);

  auto disp_a_impl = rusty::Arc<RecordingDispatcher>::make();
  auto disp_b_impl = rusty::Arc<RecordingDispatcher>::make();
  DispatcherProxy disp_a =
      pro::make_proxy<DispatcherFacade, RecordingDispatcher>(*disp_a_impl);
  DispatcherProxy disp_b =
      pro::make_proxy<DispatcherFacade, RecordingDispatcher>(*disp_b_impl);

  TransportProxy tr_a =
      make_channel_transport(&sw, /*self=*/1, /*par=*/0, {1, 2});
  TransportProxy tr_b =
      make_channel_transport(&sw, /*self=*/2, /*par=*/0, {1, 2});

  ChannelNodeWorker w_a{std::move(rx_a), std::move(disp_a)};
  ChannelNodeWorker w_b{std::move(rx_b), std::move(disp_b)};

  // A -> B: AppendEntries
  int replies = 0;
  tr_a->send_append_entries(2, AppendEntriesReq{},
      [&](siteid_t from, AppendEntriesReply) {
        EXPECT_EQ(from, 2u);
        ++replies;
      });
  EXPECT_EQ(w_b.run_until_empty(), 1u);

  // B -> A: TimeoutNow
  tr_b->send_timeout_now(1, TimeoutNowReq{},
      [&](siteid_t from, TimeoutNowReply) {
        EXPECT_EQ(from, 1u);
        ++replies;
      });
  EXPECT_EQ(w_a.run_until_empty(), 1u);

  // A broadcasts vote; only B receives (self excluded)
  tr_a->broadcast_vote(0, VoteReq{},
      [&](siteid_t from, VoteReply r) {
        EXPECT_EQ(from, 2u);
        EXPECT_TRUE(r.vote_granted);
        ++replies;
      });
  EXPECT_EQ(w_b.run_until_empty(), 1u);

  // Fire-and-forget durables
  tr_a->send_vote_durable(2, VoteDurableReq{});
  tr_a->send_append_entries_durable(2, AppendEntriesDurableReq{});
  EXPECT_EQ(w_b.run_until_empty(), 2u);

  EXPECT_EQ(disp_a_impl->counts->n_timeout.load(), 1);
  EXPECT_EQ(disp_b_impl->counts->n_append.load(),  1);
  EXPECT_EQ(disp_b_impl->counts->n_vote.load(),    1);
  EXPECT_EQ(disp_b_impl->counts->n_vote_durable.load(), 1);
  EXPECT_EQ(replies, 3);  // append, timeout, vote (durables are fire-and-forget)
}

TEST(RaftChannelTransportTest, DropDirectionBlocksOneWay) {
  ChannelSwitchboard sw;
  auto rx_a = sw.register_site(1);
  auto rx_b = sw.register_site(2);

  auto disp_b_impl = rusty::Arc<RecordingDispatcher>::make();
  DispatcherProxy disp_b =
      pro::make_proxy<DispatcherFacade, RecordingDispatcher>(*disp_b_impl);

  TransportProxy tr_a = make_channel_transport(&sw, 1, 0, {1, 2});
  ChannelNodeWorker w_b{std::move(rx_b), std::move(disp_b)};

  sw.drop_direction(/*from=*/1, /*to=*/2);
  tr_a->send_timeout_now(2, TimeoutNowReq{},
      [](siteid_t, TimeoutNowReply) { FAIL() << "reply should never fire"; });
  EXPECT_EQ(w_b.run_until_empty(), 0u);

  sw.reset_faults();
  int replies = 0;
  tr_a->send_timeout_now(2, TimeoutNowReq{},
      [&](siteid_t, TimeoutNowReply) { ++replies; });
  EXPECT_EQ(w_b.run_until_empty(), 1u);
  EXPECT_EQ(replies, 1);
}
