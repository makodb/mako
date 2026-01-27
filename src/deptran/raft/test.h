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

  // Test that secured leader continues even after losing speculative quorum
  int testSecuredLeaderContinuesAfterSpecQuorumLoss(void);

  // Test that durable commit requires secured leader
  int testDurableCommitRequiresSecuredLeader(void);

  // ===========================================================================
  // PHASE 7.2: NotifyRestart Tests
  // ===========================================================================
  // Tests for notifyRestart and step-down behavior

  // Test that follower restart removes from specVoters
  int testRestartRemovesFromSpecVoters(void);

  // Test that unsecured leader steps down when losing spec quorum
  int testUnsecuredLostQuorumStepsDown(void);

  // Test that restart removes from memoryAcks for unsecured entries
  int testRestartRemovesFromMemoryAcks(void);

  // Test that restart does not affect durableVoters
  int testRestartDoesNotAffectDurableVoters(void);

  // ===========================================================================
  // PHASE 7.3: Integration Tests
  // ===========================================================================
  // Crash and recovery integration scenarios

  // Test that speculative entries survive leader crash if new leader has them
  int testSpeculativeEntriesSurviveCrash(void);

  void wait(uint64_t microseconds);

};

#endif

} // namespace janus
