// Compile + behavior test for the TransportFacade. Installs a trivial
// in-process adapter that records every send_* invocation, wraps it in
// a TransportProxy, and verifies that each facade method routes
// correctly and returns the expected reply.

#include <gtest/gtest.h>

#include <rusty/arc.hpp>
#include <rusty/sync/atomic.hpp>

#include "deptran/raft/transport.hpp"

using namespace janus::raft;

namespace {

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
  rusty::Arc<Counts> counts{rusty::Arc<Counts>::make()};

  siteid_t self_site_id() const { return self; }

  AppendEntriesReply send_append_entries(siteid_t, AppendEntriesReq) {
    counts->n_append.fetch_add(1);
    AppendEntriesReply r{};
    r.follower_append_ok = 1;
    r.follower_current_term = 7;
    return r;
  }

  EmptyAppendEntriesReply send_empty_append_entries(siteid_t, EmptyAppendEntriesReq) {
    counts->n_empty.fetch_add(1);
    EmptyAppendEntriesReply r{};
    r.follower_append_ok = 1;
    return r;
  }

  VoteReply send_vote(siteid_t, VoteReq) {
    counts->n_vote.fetch_add(1);
    VoteReply r{};
    r.vote_granted = true;
    return r;
  }

  TimeoutNowReply send_timeout_now(siteid_t, TimeoutNowReq) {
    counts->n_timeout.fetch_add(1);
    TimeoutNowReply r{};
    r.success = true;
    return r;
  }

  void send_vote_durable(siteid_t, VoteDurableReq) {
    counts->n_vote_durable.fetch_add(1);
  }

  void send_append_entries_durable(siteid_t, AppendEntriesDurableReq) {
    counts->n_append_durable.fetch_add(1);
  }

  void send_notify_restart(siteid_t, parid_t) {
    counts->n_notify_restart.fetch_add(1);
  }

  InstallSnapshotReply send_install_snapshot(siteid_t, InstallSnapshotReq) {
    counts->n_install_snap.fetch_add(1);
    InstallSnapshotReply r{};
    r.term_out = 42;
    return r;
  }
};

}  // namespace

TEST(RaftTransportFacadeTest, AdapterConformsToFacade) {
  auto adapter = rusty::Arc<RecordingAdapter>::make();
  TransportProxy proxy =
      pro::make_proxy<TransportFacade, RecordingAdapter>(*adapter);

  EXPECT_EQ(proxy->self_site_id(), 99u);

  auto a = proxy->send_append_entries(1, AppendEntriesReq{});
  EXPECT_EQ(a.follower_current_term, 7u);

  auto e = proxy->send_empty_append_entries(2, EmptyAppendEntriesReq{});
  EXPECT_EQ(e.follower_append_ok, 1u);

  auto v = proxy->send_vote(3, VoteReq{});
  EXPECT_TRUE(v.vote_granted);

  auto t = proxy->send_timeout_now(4, TimeoutNowReq{});
  EXPECT_TRUE(t.success);

  proxy->send_vote_durable(5, VoteDurableReq{});
  proxy->send_append_entries_durable(6, AppendEntriesDurableReq{});
  proxy->send_notify_restart(99, 0);

  auto s = proxy->send_install_snapshot(7, InstallSnapshotReq{});
  EXPECT_EQ(s.term_out, 42u);

  EXPECT_EQ(adapter->counts->n_append.load(),          1);
  EXPECT_EQ(adapter->counts->n_empty.load(),           1);
  EXPECT_EQ(adapter->counts->n_vote.load(),            1);
  EXPECT_EQ(adapter->counts->n_timeout.load(),         1);
  EXPECT_EQ(adapter->counts->n_vote_durable.load(),    1);
  EXPECT_EQ(adapter->counts->n_append_durable.load(),  1);
  EXPECT_EQ(adapter->counts->n_notify_restart.load(),  1);
  EXPECT_EQ(adapter->counts->n_install_snap.load(),    1);
}
