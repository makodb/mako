#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "deptran/raft/test.h"
#include "deptran/raft/test_cluster.hpp"
#include "deptran/raft/testconf.h"
#include "rrr/rrr.hpp"

#ifndef RAFT_TEST_CORO

TEST(RaftLabSubsetWithTestCluster, RequiresRaftTestCoro) {
  GTEST_SKIP() << "RAFT_TEST_CORO is required for RaftLab subset tests";
}

#else

namespace {

TEST(RaftLabSubsetWithTestCluster, RunsBasicSubsetAgainstClusterBackend) {
  auto cluster = janus::raft::TestCluster::with_in_memory_transport(NSERVERS);
  janus::RaftTestConfig cfg(*cluster);
  janus::RaftLabTest lab(&cfg);

  auto reactor = rrr::Reactor::get_reactor();
  ASSERT_NE(reactor.get(), nullptr);

  std::atomic<bool> done{false};
  std::atomic<int> rc{1};
  reactor->create_run_fiber([&lab, &done, &rc]() {
    rc.store(lab.RunBasicSubset(), std::memory_order_relaxed);
    done.store(true, std::memory_order_release);
  });

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(180);
  while (!done.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    reactor->loop(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_TRUE(done.load(std::memory_order_acquire));
  lab.Cleanup();
  EXPECT_EQ(rc.load(std::memory_order_relaxed), 0);
}

}  // namespace

#endif
