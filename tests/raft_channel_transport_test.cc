// Phase 8.0 smoke test: two sites send RPCs through a ChannelSwitchboard
// to recording dispatchers. Fiber-synchronous — senders block on an
// mpsc reply channel until the remote worker thread produces the reply.
//
// Each node runs a dedicated std::thread calling step_blocking() so the
// sender actually has someone to unblock it.

#include <stdlib.h>

#include <gtest/gtest.h>


#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/sync/atomic.hpp>

#include "deptran/raft/channel_transport.hpp"

import std;

using namespace janus::raft;

namespace {

using AtomicInt = rusty::sync::atomic::detail::Atomic<int>;

struct Counts {
  AtomicInt n_append{0};
  AtomicInt n_vote{0};
  AtomicInt n_install{0};
};

class RecordingDispatcher : public DispatcherBase {
 public:
  rusty::Arc<Counts> counts{rusty::Arc<Counts>::make()};

  VoteReply handle_vote(VoteReq) override {
    counts->n_vote.fetch_add(1);
    VoteReply r{}; r.vote_granted = true; return r;
  }
  AppendEntriesReply handle_append_entries(AppendEntriesReq) override {
    counts->n_append.fetch_add(1);
    AppendEntriesReply r{}; r.follower_append_ok = 1; return r;
  }
  EmptyAppendEntriesReply handle_empty_append_entries(EmptyAppendEntriesReq) override {
    counts->n_append.fetch_add(1);
    EmptyAppendEntriesReply r{}; r.follower_append_ok = 1; return r;
  }
  InstallSnapshotReply handle_install_snapshot(InstallSnapshotReq) override {
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

  auto* raw_a = new RecordingDispatcher();
  auto* raw_b = new RecordingDispatcher();
  rusty::Arc<Counts> counts_a = raw_a->counts;
  rusty::Arc<Counts> counts_b = raw_b->counts;
  DispatcherProxy disp_a(raw_a);
  DispatcherProxy disp_b(raw_b);

  TransportProxy tr_a = make_channel_transport(&sw, /*self=*/1, /*par=*/0);
  TransportProxy tr_b = make_channel_transport(&sw, /*self=*/2, /*par=*/0);

  ChannelNodeWorker w_a{std::move(rx_a), std::move(disp_a)};
  ChannelNodeWorker w_b{std::move(rx_b), std::move(disp_b)};

  WorkerHarness ha{&w_a};
  WorkerHarness hb{&w_b};

  auto ae = tr_a->send_append_entries(2, AppendEntriesReq{});
  EXPECT_EQ(ae.follower_append_ok, 1u);

  auto vb = tr_b->send_vote(1, VoteReq{});
  EXPECT_TRUE(vb.vote_granted);

  auto v = tr_a->send_vote(2, VoteReq{});
  EXPECT_TRUE(v.vote_granted);

  EXPECT_EQ(counts_a->n_vote.load(),    1);
  EXPECT_EQ(counts_b->n_append.load(),  1);
  EXPECT_EQ(counts_b->n_vote.load(),    1);
}

TEST(RaftChannelTransportTest, DropDirectionFallsBackToDefault) {
  ChannelSwitchboard sw;
  auto rx_a = sw.register_site(1);
  auto rx_b = sw.register_site(2);

  auto* raw_a = new RecordingDispatcher();
  auto* raw_b = new RecordingDispatcher();
  rusty::Arc<Counts> counts_b = raw_b->counts;
  DispatcherProxy disp_a(raw_a);
  DispatcherProxy disp_b(raw_b);

  TransportProxy tr_a = make_channel_transport(&sw, 1, 0);

  ChannelNodeWorker w_a{std::move(rx_a), std::move(disp_a)};
  ChannelNodeWorker w_b{std::move(rx_b), std::move(disp_b)};

  WorkerHarness ha{&w_a};
  WorkerHarness hb{&w_b};

  // Drop 1→2; send_vote's envelope is dropped at the switchboard,
  // so the reply sender is destroyed and recv() returns Err. The adapter
  // falls back to a default-constructed reply (vote_granted=false).
  sw.drop_direction(/*from=*/1, /*to=*/2);
  auto dropped = tr_a->send_vote(2, VoteReq{});
  EXPECT_FALSE(dropped.vote_granted);

  sw.reset_faults();
  auto ok = tr_a->send_vote(2, VoteReq{});
  EXPECT_TRUE(ok.vote_granted);
  EXPECT_EQ(counts_b->n_vote.load(), 1);
}
