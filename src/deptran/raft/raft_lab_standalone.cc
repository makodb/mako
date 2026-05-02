/**
 * raft_lab_standalone — runs the full RaftLabTest suite against the in-process
 * TestCluster backend (Phase 8.7).
 */

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <thread>

#include "deptran/raft/test.h"
#include "deptran/raft/test_cluster.hpp"
#include "deptran/raft/testconf.h"
#include "rrr/rrr.hpp"

#ifndef RAFT_TEST_CORO

int main(int /*argc*/, char** /*argv*/) {
  std::fprintf(stderr,
               "raft_lab_standalone requires RAFT_TEST_CORO build mode\n");
  return 1;
}

#else

int main(int /*argc*/, char** /*argv*/) {
  // RaftLab standalone uses an in-process test cluster without the full
  // production communicator wiring required by Jetpack recovery RPCs.
  ::setenv("MAKO_DISABLE_JETPACK", "1", 1);
  // Match the lab/coro heartbeat cadence expected by the original test
  // thresholds; the production default is much tighter.
  ::setenv("MAKO_RAFT_HEARTBEAT_INTERVAL_US", "100000", 1);

  auto cluster = janus::raft::TestCluster::with_in_memory_transport(NSERVERS);
  janus::RaftTestConfig cfg(*cluster);
  janus::RaftLabTest test(&cfg);

  auto reactor = rrr::Reactor::get_reactor();
  if (reactor.get() == nullptr) {
    std::fprintf(stderr, "raft_lab_standalone: reactor unavailable\n");
    test.Cleanup();
    return 1;
  }

  std::atomic<bool> done{false};
  std::atomic<int> rc{1};
  reactor->create_run_fiber([&test, &done, &rc]() {
    rc.store(test.Run(), std::memory_order_relaxed);
    done.store(true, std::memory_order_release);
  });

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::minutes(20);
  while (!done.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    reactor->loop(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  test.Cleanup();

  if (!done.load(std::memory_order_acquire)) {
    std::fprintf(stderr, "raft_lab_standalone: timed out waiting for Run()\n");
    return 1;
  }

  return rc.load(std::memory_order_relaxed) == 0 ? 0 : 1;
}

#endif
