#pragma once

#include "testconf.h"

namespace janus {

#ifdef RAFT_TEST_CORO

class RaftLabTest {

 private:
  RaftTestConfig *config_;
  uint64_t index_;
  uint64_t init_rpcs_;

 public:
  RaftLabTest(RaftTestConfig *config) : config_(config), index_(1) {}
  int Run(void);
  void Cleanup(void);

 private:

  int testPersistence(void);
  int testTwoFollowerPersistence(void);
  int testLeaderFollowerPersistence(void);
  int testComprehensiveCrashRecovery(void);
  int testPartitionPlusRestart(void);
  int testSequentialPartitionsPlusRestart(void);
  int testMultipleRestartsPlusPartition(void);
  int testInitialElection(void);
  int testReElection(void);

  int testBasicAgree(void);
  int testFailAgree(void);
  int testFailNoAgree(void);
  int testRejoin(void);
  int testConcurrentStarts(void);
  int testBackup(void);
  int testCount(void);

  int testUnreliableAgree(void);
  int testFigure8(void);
  int testFigure8CrashRecovery(void);

  // ===========================================================================
  // SPECULATIVE RAFT TESTS (Phase 7)
  // ===========================================================================
  // Tests for speculative replication functionality

  // Test that leader becomes speculative first, then secured after VoteDurable
  int testSpeculativeLeaderElection(void);

  // Test that specCommitIndex advances on memory ack quorum
  int testSpecCommitIndexAdvances(void);

  // Test that invariants hold throughout operations
  int testSpeculativeInvariantsHold(void);

  void wait(uint64_t microseconds);

};

#endif

} // namespace janus
