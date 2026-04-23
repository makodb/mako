// Compile + behavior test for the TransportFacade. Installs a trivial
// in-process adapter that records every send_* invocation, wraps it in
// a TransportProxy, and verifies that each facade method routes
// correctly.
//
// This is NOT the real ChannelTransport (phase 4); it exists only to
// prove the facade's shape is usable.

#include <gtest/gtest.h>

#include <rusty/arc.hpp>
#include <rusty/sync/atomic.hpp>

#include "deptran/raft/transport.hpp"

using namespace janus::raft;

namespace {

// rusty::sync::atomic::Atomic exposes const-callable fetch_add so the
// adapter can share Counts through rusty::Arc (which only hands out
// const access).
using AtomicInt = rusty::sync::atomic::Atomic<int>;

struct Counts {
  AtomicInt n_append{0};
  AtomicInt n_empty{0};
  AtomicInt n_vote{0};
  AtomicInt n_timeout{0};
  AtomicInt n_vote_durable{0};
  AtomicInt n_append_durable{0};
  AtomicInt n_notify_restart{0};
  AtomicInt n_install_snap{0};
};

struct RecordingAdapter {
  siteid_t  self{99};
  // Counters live behind Arc so the adapter stays movable / cheap to
  // copy into a pro::proxy. The test retains an Arc to observe counts.
  rusty::Arc<Counts> counts{rusty::Arc<Counts>::make()};

  siteid_t self_site_id() const { return self; }

  void send_append_entries(siteid_t dst,
                           AppendEntriesReq /*req*/,
                           OnAppendEntriesReply cb) {
    counts->n_append.fetch_add(1);
    // Synthesise an empty reply so callers see the contract exercised.
    AppendEntriesReply r{};
    r.follower_append_ok = 1;
    r.follower_current_term = 7;
    cb(dst, r);
  }

  void send_empty_append_entries(siteid_t dst,
                                 EmptyAppendEntriesReq /*req*/,
                                 OnAppendEntriesReply cb) {
    counts->n_empty.fetch_add(1);
    AppendEntriesReply r{};
    r.follower_append_ok = 1;
    cb(dst, r);
  }

  void broadcast_vote(parid_t /*par*/,
                      VoteReq /*req*/,
                      OnVoteReply cb) {
    counts->n_vote.fetch_add(1);
    VoteReply r{};
    r.vote_granted = true;
    cb(self + 1, r);
    cb(self + 2, r);
  }

  void send_timeout_now(siteid_t dst,
                        TimeoutNowReq /*req*/,
                        OnTimeoutNowReply cb) {
    counts->n_timeout.fetch_add(1);
    TimeoutNowReply r{};
    r.success = true;
    cb(dst, r);
  }

  void send_vote_durable(siteid_t /*candidate*/, VoteDurableReq /*req*/) {
    counts->n_vote_durable.fetch_add(1);
  }

  void send_append_entries_durable(siteid_t /*leader*/,
                                   AppendEntriesDurableReq /*req*/) {
    counts->n_append_durable.fetch_add(1);
  }

  void send_notify_restart(siteid_t /*self*/, parid_t /*par*/) {
    counts->n_notify_restart.fetch_add(1);
  }

  void send_install_snapshot(siteid_t dst,
                             InstallSnapshotReq /*req*/,
                             OnInstallSnapshotReply cb) {
    counts->n_install_snap.fetch_add(1);
    InstallSnapshotReply r{};
    r.term_out = 42;
    cb(dst, r);
  }
};

}  // namespace

TEST(RaftTransportFacadeTest, AdapterConformsToFacade) {
  auto adapter = rusty::Arc<RecordingAdapter>::make();
  TransportProxy proxy =
      pro::make_proxy<TransportFacade, RecordingAdapter>(*adapter);

  EXPECT_EQ(proxy->self_site_id(), 99u);

  int replies_seen = 0;
  proxy->send_append_entries(1, AppendEntriesReq{},
      [&](siteid_t from, AppendEntriesReply r) {
        EXPECT_EQ(from, 1u);
        EXPECT_EQ(r.follower_current_term, 7u);
        ++replies_seen;
      });
  proxy->send_empty_append_entries(2, EmptyAppendEntriesReq{},
      [&](siteid_t, AppendEntriesReply) { ++replies_seen; });
  proxy->broadcast_vote(0, VoteReq{},
      [&](siteid_t, VoteReply r) {
        EXPECT_TRUE(r.vote_granted);
        ++replies_seen;
      });
  proxy->send_timeout_now(3, TimeoutNowReq{},
      [&](siteid_t, TimeoutNowReply) { ++replies_seen; });
  proxy->send_vote_durable(4, VoteDurableReq{});
  proxy->send_append_entries_durable(5, AppendEntriesDurableReq{});
  proxy->send_notify_restart(99, 0);
  proxy->send_install_snapshot(6, InstallSnapshotReq{},
      [&](siteid_t, InstallSnapshotReply r) {
        EXPECT_EQ(r.term_out, 42u);
        ++replies_seen;
      });

  EXPECT_EQ(adapter->counts->n_append.load(),          1);
  EXPECT_EQ(adapter->counts->n_empty.load(),           1);
  EXPECT_EQ(adapter->counts->n_vote.load(),            1);
  EXPECT_EQ(adapter->counts->n_timeout.load(),         1);
  EXPECT_EQ(adapter->counts->n_vote_durable.load(),    1);
  EXPECT_EQ(adapter->counts->n_append_durable.load(),  1);
  EXPECT_EQ(adapter->counts->n_notify_restart.load(),  1);
  EXPECT_EQ(adapter->counts->n_install_snap.load(),    1);

  // 1 append + 1 empty + 2 vote + 1 timeout + 1 install = 6 reply invocations
  EXPECT_EQ(replies_seen, 6);
}
