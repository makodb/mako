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

  // Test that voter crash before VoteDurable fsync is handled correctly
  int testVoterCrashBeforeVoteFsync(void);

  // Test double-vote prevention after crash
  int testDoubleVotePrevention(void);

  // ===========================================================================
  // PHASE 7.4: Stress Tests
  // ===========================================================================

  // Test rapid follower restarts
  int testRapidRestarts(void);

  // Test concurrent elections with speculative voting
  int testConcurrentElections(void);

  // ===========================================================================
  // PHASE 5.3: Client Notification Tests
  // ===========================================================================

  // Test that client gets SPECULATIVE notification
  int testSpeculativeCommitNotification(void);

  // Test that client gets DURABLE notification
  int testDurableCommitNotification(void);

  // Test that SPECULATIVE comes before DURABLE for same entry
  int testNotificationOrdering(void);

  // Test that unsecured leader step-down notifies ROLLEDBACK
  int testUnsecuredStepDownNotifiesRollback(void);

  // Test full commit path: SPECULATIVE -> DURABLE -> persist after restart
  int testFullCommitPath(void);

  // Test secured leader step-down: only entries > securedLogIndex get ROLLEDBACK
  int testSecuredStepDownPartialRollback(void);

  // Test speculative entries overwritten when new leader commits at same index
  int testSpeculativeEntriesOverwritten(void);

  // ===========================================================================
  // PHASE 6: Relaxed Invariant Tests
  // ===========================================================================
  // Tests for durableVoters ⊆ specVoters relaxation

  // Test that leader doesn't step down if durableVoters >= quorum (even when specVoters < quorum)
  int testDurableQuorumPreemptsStepDown(void);

  // Test transition to secured via durable quorum after spec quorum lost due to restarts
  int testSecuredViaDurableAfterSpecLoss(void);

  // ===========================================================================
  // PHASE 3.1: Snapshot Data Format and Metadata Tests
  // ===========================================================================
  // Unit tests for snapshot infrastructure (no cluster needed)

  // Test SnapshotMetadata creation and field access
  int testSnapshotMetadataCreation(void);

  // Test Snapshot creation with data (serialize/deserialize round-trip)
  int testSnapshotFormatRoundTrip(void);

  // Test SnapshotManager save/load round-trip via FileSnapshotManager
  int testSnapshotManagerSaveLoad(void);

  // Test snapshot metadata persistence and listing
  int testSnapshotManagerListing(void);

  // Test snapshot_manager_ wiring in RaftServer
  int testSnapshotManagerWiring(void);

  // ===========================================================================
  // PHASE 3.2: CreateSnapshot Tests
  // ===========================================================================

  // Test CreateSnapshot basic functionality
  int testCreateSnapshotBasic(void);

  // Test CreateSnapshot triggers compaction
  int testCreateSnapshotAndCompaction(void);

  // Test snapshot threshold is configurable
  int testSnapshotThresholdConfigurable(void);

  // ===========================================================================
  // PHASE 3.3: InstallSnapshot Tests
  // ===========================================================================

  // Test InstallSnapshot basic functionality
  int testInstallSnapshotBasic(void);

  // Test InstallSnapshot rejects stale term
  int testInstallSnapshotRejectsStaleTerm(void);

  // Test HeartbeatLoop triggers InstallSnapshot for lagging followers
  int testHeartbeatTriggersInstallSnapshot(void);

  // ===========================================================================
  // SPECULATIVE INDEX PERSISTENCE TESTS
  // ===========================================================================

  // Test that specCommitIndex and securedLogIndex are persisted to storage
  int testSpecCommitIndexPersistence(void);

  // Test that speculative indices are recovered correctly on restart
  int testSpecIndicesRecoveredOnRestart(void);

  // ===========================================================================
  // REASON-AWARE ROLLBACK NOTIFICATION TESTS
  // ===========================================================================

  // Test that UnsecuredFailure step-down rolls back all entries above commitIndex
  int testRollbackOnUnsecuredFailure(void);

  // Test that HigherTerm step-down does not send rollback notifications
  int testNoRollbackOnHigherTerm(void);

  // ===========================================================================
  // PHASE 3.4: Snapshot Recovery on Startup Tests
  // ===========================================================================

  // Test snapshot recovery updates executeIndex/commitIndex/min_active_slot_ on restart
  int testSnapshotRecoveryOnStartup(void);

  // Test InitializeSnapshotManager only advances indices, never goes backwards
  int testSnapshotRecoveryFieldAdvancement(void);

  // Test runtime-configurable heartbeat interval
  int testHeartbeatIntervalConfigurable(void);

  void wait(uint64_t microseconds);

};

#endif

} // namespace janus
