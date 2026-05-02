// Phase 8.0 smoke test: two sites send RPCs through a ChannelSwitchboard
// to recording dispatchers. Fiber-synchronous — senders block on an
// mpsc reply channel until the remote worker thread produces the reply.
//
// Each node runs a dedicated std::thread calling step_blocking() so the
// sender actually has someone to unblock it.

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

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
  AtomicInt n_append_durable{0};
  AtomicInt n_install{0};
};

struct RecordingDispatcher {
  rusty::Arc<Counts> counts{rusty::Arc<Counts>::make()};

  VoteReply handle_vote(VoteReq) {
    counts->n_vote.fetch_add(1);
    VoteReply r{}; r.vote_granted = true; return r;
  }
  VoteDurableReply handle_vote_durable(VoteDurableReq) {
    counts->n_vote_durable.fetch_add(1);
    return VoteDurableReply{};
  }
  AppendEntriesReply handle_append_entries(AppendEntriesReq) {
    counts->n_append.fetch_add(1);
    AppendEntriesReply r{}; r.follower_append_ok = 1; return r;
  }
  EmptyAppendEntriesReply handle_empty_append_entries(EmptyAppendEntriesReq) {
    counts->n_append.fetch_add(1);
    EmptyAppendEntriesReply r{}; r.follower_append_ok = 1; return r;
  }
  AppendEntriesDurableReply handle_append_entries_durable(AppendEntriesDurableReq) {
    counts->n_append_durable.fetch_add(1);
    return AppendEntriesDurableReply{};
  }
  TimeoutNowReply handle_timeout_now(TimeoutNowReq) {
    counts->n_timeout.fetch_add(1);
    TimeoutNowReply r{}; r.success = true; return r;
  }
  NotifyRestartReply handle_notify_restart(NotifyRestartReq) {
    return NotifyRestartReply{};
  }
  InstallSnapshotReply handle_install_snapshot(InstallSnapshotReq) {
    counts->n_install.fetch_add(1);
    InstallSnapshotReply r{}; r.term_out = 7; return r;
  }
};

// Spins a std::thread running step_blocking() until a stop flag is set.
struct WorkerHarness {
  std::atomic<bool> stop{false};
  std::thread th;

  // @unsafe { std::thread is on its way out; background worker for tests }
  WorkerHarness(ChannelNodeWorker* w) {
    th = std::thread([w, this] {
      while (!stop.load()) {
        if (!w->step_blocking()) break;  // channel closed
      }
    });
  }

  ~WorkerHarness() {
    stop.store(true);
    if (th.joinable()) th.detach();  // will exit when recv errors on drop
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

  TransportProxy tr_a = make_channel_transport(&sw, /*self=*/1, /*par=*/0);
  TransportProxy tr_b = make_channel_transport(&sw, /*self=*/2, /*par=*/0);

  ChannelNodeWorker w_a{std::move(rx_a), std::move(disp_a)};
  ChannelNodeWorker w_b{std::move(rx_b), std::move(disp_b)};

  WorkerHarness ha{&w_a};
  WorkerHarness hb{&w_b};

  auto ae = tr_a->send_append_entries(2, AppendEntriesReq{});
  EXPECT_EQ(ae.follower_append_ok, 1u);

  auto tn = tr_b->send_timeout_now(1, TimeoutNowReq{});
  EXPECT_TRUE(tn.success);

  auto v = tr_a->send_vote(2, VoteReq{});
  EXPECT_TRUE(v.vote_granted);

  auto snap = tr_a->send_install_snapshot(2, InstallSnapshotReq{});
  EXPECT_EQ(snap.term_out, 7u);

  tr_a->send_vote_durable(2, VoteDurableReq{});
  tr_a->send_append_entries_durable(2, AppendEntriesDurableReq{});
  // Give the durables a moment to be consumed before we tear down.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  EXPECT_EQ(disp_a_impl->counts->n_timeout.load(), 1);
  EXPECT_EQ(disp_b_impl->counts->n_append.load(),  1);
  EXPECT_EQ(disp_b_impl->counts->n_vote.load(),    1);
  EXPECT_EQ(disp_b_impl->counts->n_vote_durable.load(), 1);
  EXPECT_EQ(disp_b_impl->counts->n_append_durable.load(), 1);
  EXPECT_EQ(disp_b_impl->counts->n_install.load(), 1);
}

TEST(RaftChannelTransportTest, DropDirectionFallsBackToDefault) {
  ChannelSwitchboard sw;
  auto rx_a = sw.register_site(1);
  auto rx_b = sw.register_site(2);

  auto disp_a_impl = rusty::Arc<RecordingDispatcher>::make();
  auto disp_b_impl = rusty::Arc<RecordingDispatcher>::make();
  DispatcherProxy disp_a =
      pro::make_proxy<DispatcherFacade, RecordingDispatcher>(*disp_a_impl);
  DispatcherProxy disp_b =
      pro::make_proxy<DispatcherFacade, RecordingDispatcher>(*disp_b_impl);

  TransportProxy tr_a = make_channel_transport(&sw, 1, 0);

  ChannelNodeWorker w_a{std::move(rx_a), std::move(disp_a)};
  ChannelNodeWorker w_b{std::move(rx_b), std::move(disp_b)};

  WorkerHarness ha{&w_a};
  WorkerHarness hb{&w_b};

  // Drop 1→2; send_timeout_now's envelope is dropped at the switchboard,
  // so the reply sender is destroyed and recv() returns Err. The adapter
  // falls back to a default-constructed reply (success=false).
  sw.drop_direction(/*from=*/1, /*to=*/2);
  auto dropped = tr_a->send_timeout_now(2, TimeoutNowReq{});
  EXPECT_FALSE(dropped.success);
  tr_a->send_vote_durable(2, VoteDurableReq{});
  tr_a->send_append_entries_durable(2, AppendEntriesDurableReq{});
  auto dropped_snapshot = tr_a->send_install_snapshot(2, InstallSnapshotReq{});
  EXPECT_EQ(dropped_snapshot.term_out, 0u);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(disp_b_impl->counts->n_vote_durable.load(), 0);
  EXPECT_EQ(disp_b_impl->counts->n_append_durable.load(), 0);

  sw.reset_faults();
  auto ok = tr_a->send_timeout_now(2, TimeoutNowReq{});
  EXPECT_TRUE(ok.success);
  tr_a->send_vote_durable(2, VoteDurableReq{});
  tr_a->send_append_entries_durable(2, AppendEntriesDurableReq{});
  auto snapshot_ok = tr_a->send_install_snapshot(2, InstallSnapshotReq{});
  EXPECT_EQ(snapshot_ok.term_out, 7u);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(disp_b_impl->counts->n_timeout.load(), 1);
  EXPECT_EQ(disp_b_impl->counts->n_vote_durable.load(), 1);
  EXPECT_EQ(disp_b_impl->counts->n_append_durable.load(), 1);
  EXPECT_EQ(disp_b_impl->counts->n_install.load(), 1);
}
