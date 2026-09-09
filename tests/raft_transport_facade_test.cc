// Compile + behavior test for TransportBase. Installs a trivial
// in-process adapter that records every send_* invocation, wraps it in
// a TransportProxy, and verifies that each base method routes
// correctly and returns the expected reply.

#include <gtest/gtest.h>

#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/sync/atomic.hpp>

#include "deptran/raft/transport.hpp"

using namespace janus::raft;

namespace {

using AtomicInt = rusty::sync::atomic::detail::Atomic<int>;

struct Counts {
  AtomicInt n_append{0};
  AtomicInt n_empty{0};
  AtomicInt n_vote{0};
  AtomicInt n_install_snap{0};
};

class RecordingAdapter : public TransportBase {
 public:
  siteid_t  self{99};
  rusty::Arc<Counts> counts{rusty::Arc<Counts>::make()};

  siteid_t self_site_id() const override { return self; }

  AppendEntriesReply send_append_entries(siteid_t, AppendEntriesReq) override {
    counts->n_append.fetch_add(1);
    AppendEntriesReply r{};
    r.follower_append_ok = 1;
    r.follower_current_term = 7;
    return r;
  }

  EmptyAppendEntriesReply send_empty_append_entries(siteid_t, EmptyAppendEntriesReq) override {
    counts->n_empty.fetch_add(1);
    EmptyAppendEntriesReply r{};
    r.follower_append_ok = 1;
    return r;
  }

  VoteReply send_vote(siteid_t, VoteReq) override {
    counts->n_vote.fetch_add(1);
    VoteReply r{};
    r.vote_granted = true;
    return r;
  }

  InstallSnapshotReply send_install_snapshot(siteid_t, InstallSnapshotReq) override {
    counts->n_install_snap.fetch_add(1);
    InstallSnapshotReply r{};
    r.term_out = 42;
    return r;
  }
};

}  // namespace

TEST(RaftTransportFacadeTest, AdapterConformsToFacade) {
  auto* raw = new RecordingAdapter();
  rusty::Arc<Counts> counts_handle = raw->counts;
  TransportProxy proxy(raw);

  EXPECT_EQ(proxy->self_site_id(), 99u);

  auto a = proxy->send_append_entries(1, AppendEntriesReq{});
  EXPECT_EQ(a.follower_current_term, 7u);

  auto e = proxy->send_empty_append_entries(2, EmptyAppendEntriesReq{});
  EXPECT_EQ(e.follower_append_ok, 1u);

  auto v = proxy->send_vote(3, VoteReq{});
  EXPECT_TRUE(v.vote_granted);

  auto s = proxy->send_install_snapshot(7, InstallSnapshotReq{});
  EXPECT_EQ(s.term_out, 42u);

  EXPECT_EQ(counts_handle->n_append.load(),          1);
  EXPECT_EQ(counts_handle->n_empty.load(),           1);
  EXPECT_EQ(counts_handle->n_vote.load(),            1);
  EXPECT_EQ(counts_handle->n_install_snap.load(),    1);
}
