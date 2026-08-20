// Unit tests for janus::raft::RaftQuorum<Reply> (phase 8.1a).
//
// Bootstraps an `rrr::Reactor` + spawns a fiber for each test that
// exercises wait_until_quorum, then drives the reactor's event loop
// from the test thread. Same pattern as `src/rrr/tests/test_and_event.cc`.

#include <stdint.h>

#include <gtest/gtest.h>


#include "deptran/raft/quorum.hpp"

#include "rrr/rrr.hpp"

import std;

using janus::raft::RaftQuorum;
using siteid_t_test = uint16_t;

namespace {

// Helper: drain the reactor's event loop a few times so any fiber waiting
// on an event has a chance to wake up. `loop(false)` is non-blocking and
// const, so a `const Reactor*` (what `Rc<Reactor>::get()` yields) is fine.
void pump_reactor(const ::rrr::Reactor* reactor, int iterations = 8) {
  for (int i = 0; i < iterations; ++i) {
    reactor->run_loop(false, true);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction + accessors
// ---------------------------------------------------------------------------

TEST(RaftQuorumTest, ConstructionAndAccessors) {
  // Need a reactor on the test thread because RaftQuorum's ctor calls
  // Reactor::create_sp_event<IntEvent>.
  auto reactor = ::rrr::Reactor::get_reactor();
  ASSERT_NE(reactor.get(), nullptr);

  RaftQuorum<int> q(/*n_total=*/4, /*n_needed=*/3);
  EXPECT_EQ(q.n_total(), 4);
  EXPECT_EQ(q.n_needed(), 3);
  EXPECT_EQ(q.received(), 0);
}

TEST(RaftQuorumTest, EmptyCollectIsEmpty) {
  auto reactor = ::rrr::Reactor::get_reactor();
  RaftQuorum<int> q(3, 2);
  auto drained = q.collect();
  EXPECT_TRUE(drained.empty());
  EXPECT_EQ(q.received(), 0);
}

// ---------------------------------------------------------------------------
// Single-fiber happy paths (all on the test thread; we drive the loop).
// ---------------------------------------------------------------------------

TEST(RaftQuorumTest, AllRepliesArrive_WaitReturnsTrue) {
  auto reactor = ::rrr::Reactor::get_reactor();

  // Use a heap-allocated quorum so we can capture by raw pointer in
  // the fiber lambda (RaftQuorum is non-movable).
  auto q = std::make_unique<RaftQuorum<int>>(/*n_total=*/3, /*n_needed=*/3);
  std::atomic<bool> waiter_done{false};
  std::atomic<bool> waiter_result{false};

  reactor->create_run_fiber([qp = q.get(), &waiter_done, &waiter_result]() {
    waiter_result = qp->wait_until_quorum(500'000);  // 500 ms
    waiter_done = true;
  });

  // Feed three replies, pumping after each so the IntEvent gets a chance
  // to fire its callbacks.
  q->on_reply(1, 100);
  pump_reactor(reactor.get());
  q->on_reply(2, 200);
  pump_reactor(reactor.get());
  q->on_reply(3, 300);
  pump_reactor(reactor.get());

  // Give the waiter a few more ticks to wake up.
  for (int i = 0; i < 20 && !waiter_done; ++i) pump_reactor(reactor.get());

  EXPECT_TRUE(waiter_done);
  EXPECT_TRUE(waiter_result);
  EXPECT_EQ(q->received(), 3);

  auto drained = q->collect();
  EXPECT_EQ(drained.size(), 3u);
  // Replies are appended in arrival order.
  EXPECT_EQ(drained[0].first,  1);
  EXPECT_EQ(drained[0].second, 100);
  EXPECT_EQ(drained[1].first,  2);
  EXPECT_EQ(drained[1].second, 200);
  EXPECT_EQ(drained[2].first,  3);
  EXPECT_EQ(drained[2].second, 300);
}

TEST(RaftQuorumTest, EarlyQuorum_WaitReturnsBeforeAllReplies) {
  auto reactor = ::rrr::Reactor::get_reactor();
  auto q = std::make_unique<RaftQuorum<int>>(/*n_total=*/5, /*n_needed=*/3);
  std::atomic<bool> waiter_done{false};
  std::atomic<bool> waiter_result{false};

  reactor->create_run_fiber([qp = q.get(), &waiter_done, &waiter_result]() {
    waiter_result = qp->wait_until_quorum(500'000);
    waiter_done = true;
  });

  // First three replies should be enough to trip the quorum.
  q->on_reply(11, 1);
  pump_reactor(reactor.get());
  q->on_reply(12, 2);
  pump_reactor(reactor.get());
  q->on_reply(13, 3);
  for (int i = 0; i < 20 && !waiter_done; ++i) pump_reactor(reactor.get());

  EXPECT_TRUE(waiter_done);
  EXPECT_TRUE(waiter_result);
  EXPECT_EQ(q->received(), 3);

  // Two more replies arrive after the wait already returned. They should
  // still be collected — RaftQuorum keeps recording past the threshold.
  q->on_reply(14, 4);
  q->on_reply(15, 5);
  pump_reactor(reactor.get());
  EXPECT_EQ(q->received(), 5);

  auto drained = q->collect();
  EXPECT_EQ(drained.size(), 5u);
  // Just verify the set of (from, value) pairs (order is arrival order).
  std::vector<std::pair<int, int>> got;
  for (auto& p : drained) got.emplace_back(p.first, p.second);
  EXPECT_EQ(got, (std::vector<std::pair<int, int>>{
      {11, 1}, {12, 2}, {13, 3}, {14, 4}, {15, 5}}));
}

TEST(RaftQuorumTest, Timeout_WaitReturnsFalseWithPartialReplies) {
  auto reactor = ::rrr::Reactor::get_reactor();
  auto q = std::make_unique<RaftQuorum<int>>(/*n_total=*/5, /*n_needed=*/3);
  std::atomic<bool> waiter_done{false};
  std::atomic<bool> waiter_result{true};  // poisoned; expect to flip false

  reactor->create_run_fiber([qp = q.get(), &waiter_done, &waiter_result]() {
    // 50 ms is plenty short relative to gtest's per-test budget.
    waiter_result = qp->wait_until_quorum(50'000);
    waiter_done = true;
  });

  // Send a single reply. Quorum needs 3 → wait must time out.
  q->on_reply(1, 7);
  pump_reactor(reactor.get());

  // Pump the loop until the waiter wakes up via timeout. Sleep briefly
  // between pumps so wall-clock time advances past 50ms.
  for (int i = 0; i < 50 && !waiter_done; ++i) {
    pump_reactor(reactor.get());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  EXPECT_TRUE(waiter_done);
  EXPECT_FALSE(waiter_result);
  EXPECT_EQ(q->received(), 1);

  auto drained = q->collect();
  ASSERT_EQ(drained.size(), 1u);
  EXPECT_EQ(drained[0].first,  1);
  EXPECT_EQ(drained[0].second, 7);
}

// ---------------------------------------------------------------------------
// Behavioural / correctness checks that don't need the reactor loop.
// ---------------------------------------------------------------------------

TEST(RaftQuorumTest, ReceivedCounterAdvancesPerReply) {
  auto reactor = ::rrr::Reactor::get_reactor();
  RaftQuorum<int> q(4, 3);

  EXPECT_EQ(q.received(), 0);
  q.on_reply(1, 1); EXPECT_EQ(q.received(), 1);
  q.on_reply(2, 2); EXPECT_EQ(q.received(), 2);
  q.on_reply(3, 3); EXPECT_EQ(q.received(), 3);
  q.on_reply(4, 4); EXPECT_EQ(q.received(), 4);
}

TEST(RaftQuorumTest, CollectIsOneShot_SecondCallReturnsEmpty) {
  auto reactor = ::rrr::Reactor::get_reactor();
  RaftQuorum<int> q(2, 2);
  q.on_reply(7, 70);
  q.on_reply(8, 80);

  auto first = q.collect();
  EXPECT_EQ(first.size(), 2u);

  auto second = q.collect();  // already drained
  EXPECT_TRUE(second.empty());

  // received() reflects history, not collect state.
  EXPECT_EQ(q.received(), 2);
}

// ---------------------------------------------------------------------------
// Generic Reply type — make sure the template instantiates with a
// non-trivial reply struct (mirroring how phase 8.1c will use VoteReply).
// ---------------------------------------------------------------------------

namespace {
struct FakeReply {
  bool granted{false};
  uint64_t term{0};
};
inline bool operator==(const FakeReply& a, const FakeReply& b) {
  return a.granted == b.granted && a.term == b.term;
}
}  // namespace

TEST(RaftQuorumTest, NonTrivialReplyType) {
  auto reactor = ::rrr::Reactor::get_reactor();
  RaftQuorum<FakeReply> q(/*n_total=*/3, /*n_needed=*/2);

  q.on_reply(1, FakeReply{true,  5});
  q.on_reply(2, FakeReply{false, 6});
  q.on_reply(3, FakeReply{true,  7});

  EXPECT_EQ(q.received(), 3);
  auto drained = q.collect();
  ASSERT_EQ(drained.size(), 3u);
  EXPECT_EQ(drained[0].second, (FakeReply{true,  5}));
  EXPECT_EQ(drained[1].second, (FakeReply{false, 6}));
  EXPECT_EQ(drained[2].second, (FakeReply{true,  7}));
}
