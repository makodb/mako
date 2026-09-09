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

  // Test runtime-configurable heartbeat interval
  int testHeartbeatIntervalConfigurable(void);

  // Test runtime-configurable log retention window
  int testLogRetentionWindowConfigurable(void);

  // Test long partition recovery via InstallSnapshot
  int testLongPartitionRecovery(void);

  // Stress test: rapid entry submission verifies apply_pending_ processes all
  int testHighFrequencyApply(void);

  void wait(uint64_t microseconds);

};

#endif

} // namespace janus
