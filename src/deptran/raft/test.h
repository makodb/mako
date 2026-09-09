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

  // Test-only atomic manager rotation. It reaches RaftServer's private
  // snapshot state through RaftServer::LabAccess, so this can preserve the
  // production lock order and seed replacement storage without exposing a
  // combined manager+snapshot API.
  bool InstallAndSeedSnapshotManager(
      RaftServer* server,
      std::shared_ptr<janus::raft::SnapshotManager> manager,
      uint64_t snapshot_threshold,
      uint64_t* seeded_snapshot_index);

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

  // ===========================================================================
  // SPECULATIVE RAFT TESTS
  // ===========================================================================
  // Tests for speculative replication functionality

  // Test that an elected leader holds a speculative (memory) vote quorum
  int testSpeculativeLeaderElection(void);

  // Test that specCommitIndex advances on memory ack quorum
  int testSpecCommitIndexAdvances(void);

  // Test that invariants hold throughout operations
  int testSpeculativeInvariantsHold(void);

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

  // ===========================================================================
  // PHASE 7.3: Integration Tests
  // ===========================================================================
  // Crash and recovery integration scenarios

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

  // Test that unsecured leader step-down notifies ROLLEDBACK
  int testUnsecuredStepDownNotifiesRollback(void);

  // Test speculative entries overwritten when new leader commits at same index
  int testSpeculativeEntriesOverwritten(void);

  // ===========================================================================
  // PHASE 3.1: Snapshot Data Format and Metadata Tests
  // ===========================================================================
  // Unit tests for snapshot infrastructure (no cluster needed)

  // Test SnapshotMetadata creation and field access
  int testSnapshotMetadataCreation(void);

  // Test Snapshot creation with data (serialize/deserialize round-trip)
  int testSnapshotFormatRoundTrip(void);

  // Test SnapshotManager save/load round-trip via MemorySnapshotManager
  int testSnapshotManagerSaveLoad(void);

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
  // REASON-AWARE ROLLBACK NOTIFICATION TESTS
  // ===========================================================================

  // Test that UnsecuredFailure step-down rolls back all entries above commitIndex
  int testRollbackOnUnsecuredFailure(void);

  // Test that HigherTerm step-down does not send rollback notifications
  int testNoRollbackOnHigherTerm(void);

  // Test runtime-configurable heartbeat interval
  int testHeartbeatIntervalConfigurable(void);

  // Test runtime-configurable log retention window
  int testLogRetentionWindowConfigurable(void);

  // Test long partition recovery via InstallSnapshot
  int testLongPartitionRecovery(void);

  // Test leadership transfer timeout (preferred replica crashes before winning)
  int testLeadershipTransferTimeout(void);

  // Stress test: rapid entry submission verifies apply_pending_ processes all
  int testHighFrequencyApply(void);

  // ===========================================================================
  // MEMBERSHIP CHANGE TESTS
  // ===========================================================================

  // Test AddServer basic functionality: add a new server, verify config grows
  int testAddServerBasic(void);

  // Test RemoveServer basic functionality: remove a server, verify config shrinks
  int testRemoveServerBasic(void);

  // Test that duplicate config changes are rejected when one is pending
  int testRejectDuplicateConfigChange(void);

  // Test new server catch-up: learner tracking and promotion
  int testNewServerCatchUp(void);

  // Test add server with log verification: learner receives logs and promotes with correct quorum
  int testAddServerReceivesLogs(void);

  // Test remove server quorum shrinks: verify quorum decreases after removal
  int testRemoveServerQuorumShrinks(void);

  // Test add server during active workload: entries commit while learner is added
  int testAddServerDuringActiveWorkload(void);

  // Test leader failure during config change: new leader clears pending state
  int testLeaderFailureDuringConfigChange(void);

  // Test cannot add two servers simultaneously via OnAddServer-like logic
  int testCannotAddTwoServersSimultaneously(void);

  void wait(uint64_t microseconds);

};

#endif

} // namespace janus
