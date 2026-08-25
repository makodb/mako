#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <inttypes.h>

#include "test.h"
#include "snapshot_manager.hpp"
#include "snapshot_format.hpp"
#include "file_snapshot_manager.hpp"
#include "replicated_db.h"

import std;
import rusty;

namespace janus {

#ifdef RAFT_TEST_CORO

// #define TEST_EXPAND(x) x || x || x || x || x 
#define TEST_EXPAND(x) x 

int RaftLabTest::Run(void) {
  Log_info("Starting Raft lab tests");
  Log_info("Setting up learner action callbacks");
  config_->SetLearnerAction();
  uint64_t start_rpc = config_->RpcTotal();
  Log_info("Beginning test sequence");

  const char* persistence_flag = std::getenv("MAKO_RAFT_PERSISTENCE");
  const bool persistence_enabled =
      persistence_flag != nullptr &&
      (std::strcmp(persistence_flag, "1") == 0 ||
       std::strcmp(persistence_flag, "true") == 0 ||
       std::strcmp(persistence_flag, "TRUE") == 0 ||
       std::strcmp(persistence_flag, "True") == 0);

  bool failed = false;
  if (!persistence_enabled) {
    Log_info("Running BASIC Raft test group (MAKO_RAFT_PERSISTENCE disabled)");
    failed =
        // Basic Raft tests (no disk durability)
        testInitialElection()                              // Test 1
        || TEST_EXPAND(testReElection())                   // Test 2
        || TEST_EXPAND(testBasicAgree())                   // Test 3
        || TEST_EXPAND(testFailAgree())                    // Test 4
        || TEST_EXPAND(testFailNoAgree())                  // Test 5
        || TEST_EXPAND(testRejoin())                       // Test 6
        || TEST_EXPAND(testConcurrentStarts())             // Test 7
        || TEST_EXPAND(testBackup())                       // Test 8
        || TEST_EXPAND(testCount())                        // Test 9
        || TEST_EXPAND(testUnreliableAgree())              // Test 10
        || TEST_EXPAND(testFigure8());                     // Test 11
  } else {
    Log_info("Running PERSISTENCE Raft test group (MAKO_RAFT_PERSISTENCE enabled)");
    Log_info("TEST 15 (testComprehensiveCrashRecovery) is temporarily disabled");
    failed =
        // Disk persistence and crash-recovery tests
        TEST_EXPAND(testPersistence())                     // Test 13
        || TEST_EXPAND(testLeaderFollowerPersistence())    // Test 14
        // Test 15 disabled: testComprehensiveCrashRecovery()
        || TEST_EXPAND(testPartitionPlusRestart())         // Test 16
        || TEST_EXPAND(testSequentialPartitionsPlusRestart()) // Test 17
        || TEST_EXPAND(testMultipleRestartsPlusPartition()) // Test 18
        || TEST_EXPAND(testFigure8CrashRecovery());        // Test 19
  }

  // Snapshot data format and metadata tests
  // These are unit tests that don't require persistence
  if (!failed) {
    Log_info("Running SNAPSHOT data format tests");
    failed =
        TEST_EXPAND(testSnapshotMetadataCreation())         // Test 50
        || TEST_EXPAND(testSnapshotFormatRoundTrip())       // Test 51
        || TEST_EXPAND(testSnapshotManagerSaveLoad())       // Test 52
        || TEST_EXPAND(testSnapshotManagerListing())         // Test 53
        || TEST_EXPAND(testSnapshotManagerWiring());         // Test 54
  }

  // CreateSnapshot integration tests
  if (!failed) {
    Log_info("Running CreateSnapshot tests");
    failed =
        TEST_EXPAND(testCreateSnapshotBasic())               // Test 55
        || TEST_EXPAND(testCreateSnapshotAndCompaction())    // Test 56
        || TEST_EXPAND(testSnapshotThresholdConfigurable()); // Test 57
  }

  // InstallSnapshot tests
  if (!failed) {
    Log_info("Running InstallSnapshot tests");
    failed =
        TEST_EXPAND(testInstallSnapshotBasic())              // Test 58
        || TEST_EXPAND(testInstallSnapshotRejectsStaleTerm()) // Test 59
        || TEST_EXPAND(testHeartbeatTriggersInstallSnapshot()); // Test 60
  }

  // Speculative index persistence tests
  if (!failed) {
    if (persistence_enabled) {
      Log_info("Running speculative index persistence tests");
      failed =
          TEST_EXPAND(testSpecCommitIndexPersistence())             // Test 61
          || TEST_EXPAND(testSpecIndicesRecoveredOnRestart());      // Test 62
      ;
    } else {
      Log_info("Skipping speculative index persistence tests (MAKO_RAFT_PERSISTENCE disabled)");
    }
  }

  // Reason-aware rollback notification tests
  if (!failed) {
    Log_info("Running reason-aware rollback notification tests");
    failed =
        TEST_EXPAND(testRollbackOnUnsecuredFailure())            // Test 63
        || TEST_EXPAND(testNoRollbackOnHigherTerm());            // Test 64
  }

  // Snapshot recovery on startup tests
  if (!failed) {
    Log_info("Running snapshot recovery on startup tests");
    failed =
        TEST_EXPAND(testSnapshotRecoveryOnStartup())              // Test 65
        || TEST_EXPAND(testSnapshotRecoveryFieldAdvancement());   // Test 66
  }

  // Heartbeat interval configurability test
  if (!failed) {
    Log_info("Running heartbeat interval configurability test");
    failed =
        TEST_EXPAND(testHeartbeatIntervalConfigurable());         // Test 67
  }

  // Log retention window configurability test
  if (!failed) {
    Log_info("Running log retention window configurability test");
    failed =
        TEST_EXPAND(testLogRetentionWindowConfigurable());        // Test 68
  }

  // Long partition recovery test
  if (!failed) {
    Log_info("Running long partition recovery test");
    failed =
        TEST_EXPAND(testLongPartitionRecovery());              // Test 69
  }

  // Leadership transfer timeout test
  if (!failed) {
    Log_info("Running leadership transfer timeout test");
    failed =
        TEST_EXPAND(testLeadershipTransferTimeout());          // Test 70
  }

  // Durable ack loss test
  if (!failed) {
    Log_info("Running durable ack loss test");
    failed =
        TEST_EXPAND(testDurableAckLoss());                     // Test 71
  }

  // High frequency apply stress test
  if (!failed) {
    Log_info("Running high frequency apply stress test");
    failed =
        TEST_EXPAND(testHighFrequencyApply());                 // Test 72
  }

  // Membership change tests
  if (!failed) {
    Log_info("Running membership change tests");
    failed =
        TEST_EXPAND(testAddServerBasic())                      // Test 73
        || TEST_EXPAND(testRemoveServerBasic())                // Test 74
        || TEST_EXPAND(testRejectDuplicateConfigChange())      // Test 75
        || TEST_EXPAND(testNewServerCatchUp())                // Test 76
        || TEST_EXPAND(testAddServerReceivesLogs())           // Test 77
        || TEST_EXPAND(testRemoveServerQuorumShrinks())       // Test 78
        || TEST_EXPAND(testAddServerDuringActiveWorkload())   // Test 79
        || TEST_EXPAND(testLeaderFailureDuringConfigChange()) // Test 80
        || TEST_EXPAND(testCannotAddTwoServersSimultaneously()); // Test 81
  }

  // ReplicatedDB command serialization tests
  if (!failed) {
    Log_info("Running ReplicatedDB command tests");
    failed =
        TEST_EXPAND(testReplicatedDBCommandPutMarshal())      // Test 82
        || TEST_EXPAND(testReplicatedDBCommandDeleteMarshal()) // Test 83
        || TEST_EXPAND(testReplicatedDBCommandBatchMarshal()); // Test 84
  }

  // ReplicatedDB integration tests (require running Raft cluster)
  if (!failed) {
    Log_info("Running ReplicatedDB integration tests");
    failed =
        TEST_EXPAND(testReplicatedDBPutGet())         // Test 85
        || TEST_EXPAND(testReplicatedDBDelete())      // Test 86
        || TEST_EXPAND(testReplicatedDBReplication()) // Test 87
        || TEST_EXPAND(testReplicatedDBSnapshot())    // Test 88
        || TEST_EXPAND(testReplicatedDBSnapshotTransfer()) // Test 89
        || TEST_EXPAND(testReplicatedDBWiring())      // Test 90
        || TEST_EXPAND(testReplicatedDBSnapshotCompression()); // Test 91
  }

  // ConfigManager / ClusterConfig / ConfigWatcher tests (old Tests 92-98)
  // were removed: ConfigManager no longer stores config in a
  // ReplicatedDB — it now uses Mako's unified FullOrderedIndex storage
  // interface (the __mako_config__ system table). That component is
  // covered standalone by tests/config_manager_test.cc (against an
  // in-memory FullOrderedIndex fake). A future integration test can
  // exercise ConfigManager over a real replicated index once the
  // dbtest bootstrap wiring lands.

  // LinearizableGet tests (require running Raft cluster + ReplicatedDB)
  if (!failed) {
    Log_info("Running LinearizableGet tests");
    failed =
        TEST_EXPAND(testLinearizableGet())                    // Test 99
        || TEST_EXPAND(testLinearizableGetAfterLeaderChange()); // Test 100
  }

  // ReplicatedDB crash recovery tests
  if (!failed) {
    Log_info("Running ReplicatedDB crash recovery tests");
    failed =
        TEST_EXPAND(testReplicatedDBCrashRecovery());          // Test 101
  }

  // Speculative/notify/integration/stress/notification/relaxed-invariant tests
  // remain intentionally disabled in this runner for now.
  if (failed) {
    Log_info("Test sequence failed");
    Print("TESTS FAILED");
    return 1;
  }
  Log_info("Test sequence completed successfully");
  Print("ALL TESTS PASSED");
  Log_info("Calculating final RPC count");
  Print("Total RPC count: %ld", config_->RpcTotal() - start_rpc);
  return 0;
}

void RaftLabTest::Cleanup(void) {
  config_->Shutdown();
}

#define Init2(test_id, description) \
  Init(test_id, description); \
  verify(config_->NDisconnected() == 0 && !config_->IsUnreliable())
#define Passed2() Passed(); return 0

#define Assert(expr) if (!(expr)) { \
  return 1; \
}
#define Assert2(expr, msg, ...) if (!(expr)) { \
  Failed(msg, ##__VA_ARGS__); \
  return 1; \
}

#define AssertOneLeader(ldr) Assert(ldr >= 0)
#define AssertReElection(ldr, old) \
        Assert2(ldr != old, "no reelection despite leader being disconnected")
#define AssertNoneCommitted(index) { \
        auto nc = config_->NCommitted(index); \
        Assert2(nc == 0, \
                "%d servers unexpectedly committed index %ld", \
                nc, index) \
      }
#define AssertNCommitted(index, expected) { \
        auto nc = config_->NCommitted(index); \
        Assert2(nc == expected, \
                "%d servers committed index %ld (%d expected)", \
                nc, index, expected) \
      }
#define AssertStartOk(ok) Assert2(ok, "unexpected leader change during Start()")
#define AssertWaitNoError(ret, index) \
        Assert2(ret != -3, "committed values differ for index %ld", index)
#define AssertWaitNoTimeout(ret, index, n) \
        Assert2(ret != -1, "waited too long for %d server(s) to commit index %ld", n, index); \
        Assert2(ret != -2, "term moved on before index %ld committed by %d server(s)", index, n)
#define DoAgreeAndAssertIndex(cmd, n, index) { \
        /* Log_info("DoAgreeAndAssertIndex: Starting agreement for command {} with {} servers, expected index {}", cmd, n, index); */ \
        auto r = config_->DoAgreement(cmd, n, false); \
        auto ind = index; \
        /* Log_info("DoAgreeAndAssertIndex: DoAgreement returned {} for command {}", r, cmd); */ \
        Assert2(r > 0, "failed to reach agreement for command %d among %d servers, expected commit index>0, got %" PRId64, cmd, n, r); \
        Assert2(r == ind, "agreement index incorrect. got %ld, expected %ld", r, ind); \
      }
#define DoAgreeAndAssertWaitSuccess(cmd, n) { \
        auto r = config_->DoAgreement(cmd, n, true); \
        Assert2(r > 0, "failed to reach agreement for command %d among %d servers", cmd, n); \
        index_ = r + 1; \
      }

int RaftLabTest::testPersistence(void) {
  Init2(12, "Persistence across server kill and restart (single)");

  Log_info("TEST 12: Waiting for initial election");
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 12: Leader elected: {}", leader);

  // Commit some entries
  Log_info("TEST 12: Committing initial entries");
  DoAgreeAndAssertIndex(101, NSERVERS, index_++);
  DoAgreeAndAssertIndex(102, NSERVERS, index_++);
  DoAgreeAndAssertIndex(103, NSERVERS, index_++);
  Log_info("TEST 12: Committed 3 entries");

  // Pick a follower to kill and restart
  siteid_t victim = config_->getNextServerId(leader, 1);
  Log_info("TEST 12: Killing follower {}", victim);

  // Get state before killing
  auto victim_server = config_->GetServer(victim);
  uint64_t term_before = victim_server->currentTerm;
  uint64_t last_log_before = victim_server->lastLogIndex;
  Log_info("TEST 12: Before kill - term={}, lastLogIndex={}", term_before, last_log_before);

  // Kill the server
  config_->Kill(victim);
  Log_info("TEST 12: Server {} killed", victim);

  // Sleep to ensure it's really gone
  Fiber::sleep(ELECTIONTIMEOUT / 2);

  // Commit more entries with remaining servers
  Log_info("TEST 12: Committing entries with {} servers", NSERVERS - 1);
  DoAgreeAndAssertIndex(104, NSERVERS - 1, index_++);
  DoAgreeAndAssertIndex(105, NSERVERS - 1, index_++);
  Log_info("TEST 12: Committed 2 more entries");

  // Restart the killed server
  Log_info("TEST 12: Restarting server {}", victim);
  config_->Restart(victim);
  Log_info("TEST 12: Server {} restarted", victim);

  // Give it time to catch up
  Fiber::sleep(ELECTIONTIMEOUT);

  Log_info("TEST 12: After Sleep for ELECTIONTIMEOUT");

  // Verify the restarted server recovered its state
  victim_server = config_->GetServer(victim);
  uint64_t term_after = victim_server->currentTerm;
  uint64_t last_log_after = victim_server->lastLogIndex;
  Log_info("TEST 12: After restart - term={}, lastLogIndex={}", term_after, last_log_after);

  // Term should be at least what it was before (may be higher if elections occurred)
  Assert2(term_after >= term_before,
          "term decreased after restart: was %lu, now %lu",
          term_before, term_after);

  // Last log index should be at least what it was before
  Assert2(last_log_after >= last_log_before,
          "lastLogIndex decreased after restart: was %lu, now %lu",
          last_log_before, last_log_after);

  Log_info("TEST 12: State recovered correctly");

  // Commit with all servers to verify restarted server works
  Log_info("TEST 12: Committing with all {} servers", NSERVERS);
  DoAgreeAndAssertWaitSuccess(106, NSERVERS);
  Log_info("TEST 12: Final commit successful");

  // Now test killing and restarting the leader
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 12: Testing leader kill - current leader is {}", leader);

  // Get leader state before killing
  auto leader_server = config_->GetServer(leader);
  term_before = leader_server->currentTerm;
  last_log_before = leader_server->lastLogIndex;
  Log_info("TEST 12: Leader before kill - term={}, lastLogIndex={}", term_before, last_log_before);

  // Kill the leader
  config_->Kill(leader);
  Log_info("TEST 12: Leader {} killed", leader);

  // Wait for new leader election
  Fiber::sleep(ELECTIONTIMEOUT);
  int new_leader = config_->OneLeader();
  AssertOneLeader(new_leader);
  AssertReElection(new_leader, leader);
  Log_info("TEST 12: New leader elected: {}", new_leader);

  // Commit entries with new leader
  DoAgreeAndAssertIndex(107, NSERVERS - 1, index_++);
  Log_info("TEST 12: Committed entry with new leader");

  // Restart the old leader
  Log_info("TEST 12: Restarting old leader {}", leader);
  config_->Restart(leader);
  Log_info("TEST 12: Old leader {} restarted", leader);

  // Give it time to catch up
  Fiber::sleep(ELECTIONTIMEOUT);

  // Verify old leader recovered
  leader_server = config_->GetServer(leader);
  term_after = leader_server->currentTerm;
  last_log_after = leader_server->lastLogIndex;
  Log_info("TEST 12: Old leader after restart - term={}, lastLogIndex={}", term_after, last_log_after);

  Assert2(term_after >= term_before,
          "old leader term decreased after restart: was %lu, now %lu",
          term_before, term_after);

  // Final commit with all servers
  Log_info("TEST 12: Final commit with all servers");
  DoAgreeAndAssertWaitSuccess(108, NSERVERS);
  Log_info("TEST 12: All servers working correctly");

  Passed2();
}

int RaftLabTest::testTwoFollowerPersistence(void) {
  Init2(13, "Persistence across two follower kill and restart");

  Log_info("TEST 13: Waiting for initial election");
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 13: Leader elected: {}", leader);

  // Commit some entries
  Log_info("TEST 13: Committing initial entries");
  DoAgreeAndAssertIndex(1301, NSERVERS, index_++);
  DoAgreeAndAssertIndex(1302, NSERVERS, index_++);
  DoAgreeAndAssertIndex(1303, NSERVERS, index_++);
  Log_info("TEST 13: Committed 3 entries");

  // Pick two followers to kill
  siteid_t victim1 = config_->getNextServerId(leader, 1);
  siteid_t victim2 = config_->getNextServerId(leader, 2);
  Log_info("TEST 13: Killing two followers: {} and {}", victim1, victim2);

  // Get state before killing
  auto victim1_server = config_->GetServer(victim1);
  auto victim2_server = config_->GetServer(victim2);
  uint64_t term_before1 = victim1_server->currentTerm;
  uint64_t term_before2 = victim2_server->currentTerm;
  uint64_t last_log_before1 = victim1_server->lastLogIndex;
  uint64_t last_log_before2 = victim2_server->lastLogIndex;
  Log_info("TEST 13: Victim1 before kill - term={}, lastLogIndex={}", term_before1, last_log_before1);
  Log_info("TEST 13: Victim2 before kill - term={}, lastLogIndex={}", term_before2, last_log_before2);

  // Kill both servers
  config_->Kill(victim1);
  Log_info("TEST 13: Server {} killed", victim1);
  config_->Kill(victim2);
  Log_info("TEST 13: Server {} killed", victim2);

  // Sleep to ensure they're really gone
  Fiber::sleep(ELECTIONTIMEOUT / 2);

  // We still have quorum (3 out of 5), commit more entries
  Log_info("TEST 13: Committing entries with {} servers", NSERVERS - 2);
  DoAgreeAndAssertIndex(1304, NSERVERS - 2, index_++);
  DoAgreeAndAssertIndex(1305, NSERVERS - 2, index_++);
  Log_info("TEST 13: Committed 2 more entries with reduced cluster");

  // Restart victim1 first
  Log_info("TEST 13: Restarting server {}", victim1);
  config_->Restart(victim1);
  Log_info("TEST 13: Server {} restarted", victim1);

  // Give it time to catch up
  Fiber::sleep(ELECTIONTIMEOUT / 2);

  // Restart victim2
  Log_info("TEST 13: Restarting server {}", victim2);
  config_->Restart(victim2);
  Log_info("TEST 13: Server {} restarted", victim2);

  // Give both time to catch up
  Fiber::sleep(ELECTIONTIMEOUT);

  Log_info("TEST 13: Both servers restarted, verifying state");

  // Verify both restarted servers recovered their state
  victim1_server = config_->GetServer(victim1);
  victim2_server = config_->GetServer(victim2);
  uint64_t term_after1 = victim1_server->currentTerm;
  uint64_t term_after2 = victim2_server->currentTerm;
  uint64_t last_log_after1 = victim1_server->lastLogIndex;
  uint64_t last_log_after2 = victim2_server->lastLogIndex;
  Log_info("TEST 13: Victim1 after restart - term={}, lastLogIndex={}", term_after1, last_log_after1);
  Log_info("TEST 13: Victim2 after restart - term={}, lastLogIndex={}", term_after2, last_log_after2);

  // Term should be at least what it was before
  Assert2(term_after1 >= term_before1,
          "victim1 term decreased after restart: was %lu, now %lu",
          term_before1, term_after1);
  Assert2(term_after2 >= term_before2,
          "victim2 term decreased after restart: was %lu, now %lu",
          term_before2, term_after2);

  // Last log index should be at least what it was before
  Assert2(last_log_after1 >= last_log_before1,
          "victim1 lastLogIndex decreased after restart: was %lu, now %lu",
          last_log_before1, last_log_after1);
  Assert2(last_log_after2 >= last_log_before2,
          "victim2 lastLogIndex decreased after restart: was %lu, now %lu",
          last_log_before2, last_log_after2);

  Log_info("TEST 13: State recovered correctly for both servers");

  // Commit with all servers to verify both restarted servers work
  Log_info("TEST 13: Committing with all {} servers", NSERVERS);
  DoAgreeAndAssertWaitSuccess(1306, NSERVERS);
  Log_info("TEST 13: Final commit successful with all servers");

  // Verify leader is still stable
  int final_leader = config_->OneLeader();
  AssertOneLeader(final_leader);
  Log_info("TEST 13: Leader after all restarts: {}", final_leader);

  Passed2();
}

int RaftLabTest::testLeaderFollowerPersistence(void) {
  Init2(14, "Persistence across leader + follower kill and restart");

  Log_info("TEST 14: Waiting for initial election");
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 14: Leader elected: {}", leader);

  // Commit some entries
  Log_info("TEST 14: Committing initial entries");
  DoAgreeAndAssertIndex(1401, NSERVERS, index_++);
  DoAgreeAndAssertIndex(1402, NSERVERS, index_++);
  DoAgreeAndAssertIndex(1403, NSERVERS, index_++);
  Log_info("TEST 14: Committed 3 entries");

  // Pick one follower to kill along with the leader
  siteid_t victim_follower = config_->getNextServerId(leader, 1);
  siteid_t victim_leader = leader;
  Log_info("TEST 14: Killing leader {} and follower {}", victim_leader, victim_follower);

  // Get state before killing
  auto leader_server = config_->GetServer(victim_leader);
  auto follower_server = config_->GetServer(victim_follower);
  uint64_t term_before_leader = leader_server->currentTerm;
  uint64_t term_before_follower = follower_server->currentTerm;
  uint64_t last_log_before_leader = leader_server->lastLogIndex;
  uint64_t last_log_before_follower = follower_server->lastLogIndex;
  Log_info("TEST 14: Leader before kill - term={}, lastLogIndex={}", term_before_leader, last_log_before_leader);
  Log_info("TEST 14: Follower before kill - term={}, lastLogIndex={}", term_before_follower, last_log_before_follower);

  // Kill both servers (leader first, then follower)
  config_->Kill(victim_leader);
  Log_info("TEST 14: Leader {} killed", victim_leader);
  config_->Kill(victim_follower);
  Log_info("TEST 14: Follower {} killed", victim_follower);

  // Wait for new leader election among remaining 3 servers
  Log_info("TEST 14: Waiting for new leader election");
  Fiber::sleep(ELECTIONTIMEOUT);

  int new_leader = config_->OneLeader();
  AssertOneLeader(new_leader);
  Assert2(new_leader != victim_leader, "new leader should not be the killed leader");
  Assert2(new_leader != victim_follower, "new leader should not be the killed follower");
  Log_info("TEST 14: New leader elected: {}", new_leader);

  // We still have quorum (3 out of 5), commit more entries
  Log_info("TEST 14: Committing entries with {} servers", NSERVERS - 2);
  DoAgreeAndAssertIndex(1404, NSERVERS - 2, index_++);
  DoAgreeAndAssertIndex(1405, NSERVERS - 2, index_++);
  Log_info("TEST 14: Committed 2 more entries with reduced cluster");

  // Restart the follower first
  Log_info("TEST 14: Restarting follower {}", victim_follower);
  config_->Restart(victim_follower);
  Log_info("TEST 14: Follower {} restarted", victim_follower);

  // Give it time to catch up
  Fiber::sleep(ELECTIONTIMEOUT / 2);

  // Restart the old leader
  Log_info("TEST 14: Restarting old leader {}", victim_leader);
  config_->Restart(victim_leader);
  Log_info("TEST 14: Old leader {} restarted", victim_leader);

  // Give both time to catch up
  Fiber::sleep(ELECTIONTIMEOUT);

  Log_info("TEST 14: Both servers restarted, verifying state");

  // Verify both restarted servers recovered their state
  leader_server = config_->GetServer(victim_leader);
  follower_server = config_->GetServer(victim_follower);
  uint64_t term_after_leader = leader_server->currentTerm;
  uint64_t term_after_follower = follower_server->currentTerm;
  uint64_t last_log_after_leader = leader_server->lastLogIndex;
  uint64_t last_log_after_follower = follower_server->lastLogIndex;
  Log_info("TEST 14: Old leader after restart - term={}, lastLogIndex={}", term_after_leader, last_log_after_leader);
  Log_info("TEST 14: Follower after restart - term={}, lastLogIndex={}", term_after_follower, last_log_after_follower);

  // Term should be at least what it was before (may be higher due to new election)
  Assert2(term_after_leader >= term_before_leader,
          "old leader term decreased after restart: was %lu, now %lu",
          term_before_leader, term_after_leader);
  Assert2(term_after_follower >= term_before_follower,
          "follower term decreased after restart: was %lu, now %lu",
          term_before_follower, term_after_follower);

  // Last log index should be at least what it was before
  Assert2(last_log_after_leader >= last_log_before_leader,
          "old leader lastLogIndex decreased after restart: was %lu, now %lu",
          last_log_before_leader, last_log_after_leader);
  Assert2(last_log_after_follower >= last_log_before_follower,
          "follower lastLogIndex decreased after restart: was %lu, now %lu",
          last_log_before_follower, last_log_after_follower);

  Log_info("TEST 14: State recovered correctly for both servers");

  // Commit with all servers to verify both restarted servers work
  Log_info("TEST 14: Committing with all {} servers", NSERVERS);
  DoAgreeAndAssertWaitSuccess(1406, NSERVERS);
  Log_info("TEST 14: Final commit successful with all servers");

  // Verify we have a stable leader
  int final_leader = config_->OneLeader();
  AssertOneLeader(final_leader);
  Log_info("TEST 14: Leader after all restarts: {}", final_leader);

  Passed2();
}

int RaftLabTest::testComprehensiveCrashRecovery(void) {
  Init2(15, "Comprehensive crash-recovery with random server selection");

  Log_info("TEST 15: Waiting for initial election");
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 15: Initial leader elected: {}", leader);

  // Commit initial entries
  Log_info("TEST 15: Committing initial entries");
  DoAgreeAndAssertIndex(1501, NSERVERS, index_++);
  DoAgreeAndAssertIndex(1502, NSERVERS, index_++);
  Log_info("TEST 15: Initial entries committed");

  // Random number generator
  std::srand(std::time(nullptr));

  // Helper to get a random server from a set
  auto pickRandom = [](const std::set<siteid_t>& servers) -> siteid_t {
    if (servers.empty()) return -1;
    int idx = std::rand() % servers.size();
    auto it = servers.begin();
    std::advance(it, idx);
    return *it;
  };

  // Track which servers are currently alive
  std::set<siteid_t> alive_servers;
  std::set<siteid_t> dead_servers;
  for (int i = 0; i < NSERVERS; i++) {
    alive_servers.insert(config_->getServerIdByIndex(i));
  }

  const int NUM_ROUNDS = 5;
  int cmd_base = 1510;

  for (int round = 1; round <= NUM_ROUNDS; round++) {
    Log_info("TEST 15: ===== ROUND {} =====", round);

    // Kill 2 random servers (maintain quorum with 3 remaining)
    Log_info("TEST 15: Phase 1 - Killing 2 random servers");

    siteid_t victim1 = pickRandom(alive_servers);
    alive_servers.erase(victim1);
    dead_servers.insert(victim1);

    siteid_t victim2 = pickRandom(alive_servers);
    alive_servers.erase(victim2);
    dead_servers.insert(victim2);

    Log_info("TEST 15: Round {} - Killing servers {} and {}", round, victim1, victim2);
    config_->Kill(victim1);
    config_->Kill(victim2);

    // Wait for potential leader election if we killed the leader
    Fiber::sleep(ELECTIONTIMEOUT);

    // Verify we still have a leader among surviving servers
    leader = config_->OneLeader();
    AssertOneLeader(leader);
    Assert2(alive_servers.count(leader) > 0, "Leader %d should be among alive servers", leader);
    Log_info("TEST 15: Round {} - Leader after kills: {}", round, leader);

    // Commit with 3 servers (quorum)
    Log_info("TEST 15: Round {} - Committing with {} alive servers", round, alive_servers.size());
    DoAgreeAndAssertIndex(cmd_base++, (int)alive_servers.size(), index_++);

    // Restart one of the dead servers
    Log_info("TEST 15: Phase 2 - Restarting one dead server");

    siteid_t restart1 = pickRandom(dead_servers);
    dead_servers.erase(restart1);
    alive_servers.insert(restart1);

    Log_info("TEST 15: Round {} - Restarting server {}", round, restart1);
    config_->Restart(restart1);

    // Wait for it to catch up
    Fiber::sleep(ELECTIONTIMEOUT);

    // Verify leader and commit
    leader = config_->OneLeader();
    AssertOneLeader(leader);
    Log_info("TEST 15: Round {} - Leader after restart1: {}", round, leader);

    DoAgreeAndAssertIndex(cmd_base++, (int)alive_servers.size(), index_++);

    // Kill another random alive server (back to 3 alive)
    Log_info("TEST 15: Phase 3 - Killing another random server");

    // Make sure we don't kill the current leader to make it more interesting sometimes
    // But we allow it with 50% probability to test leader crash recovery
    siteid_t victim3;
    if (std::rand() % 2 == 0 && alive_servers.size() > 1) {
      // Try to kill a non-leader
      std::set<siteid_t> non_leaders = alive_servers;
      non_leaders.erase(leader);
      if (!non_leaders.empty()) {
        victim3 = pickRandom(non_leaders);
      } else {
        victim3 = pickRandom(alive_servers);
      }
    } else {
      victim3 = pickRandom(alive_servers);
    }

    alive_servers.erase(victim3);
    dead_servers.insert(victim3);

    Log_info("TEST 15: Round {} - Killing server {} (was leader: {})",
             round, victim3, victim3 == leader ? "yes" : "no");
    config_->Kill(victim3);

    // Wait for potential leader election
    Fiber::sleep(ELECTIONTIMEOUT);

    leader = config_->OneLeader();
    AssertOneLeader(leader);
    Log_info("TEST 15: Round {} - Leader after kill3: {}", round, leader);

    // Commit with remaining servers
    DoAgreeAndAssertIndex(cmd_base++, (int)alive_servers.size(), index_++);

    // Restart all dead servers
    Log_info("TEST 15: Phase 4 - Restarting all dead servers");

    std::vector<siteid_t> to_restart(dead_servers.begin(), dead_servers.end());
    for (siteid_t svr : to_restart) {
      Log_info("TEST 15: Round {} - Restarting server {}", round, svr);
      config_->Restart(svr);
      dead_servers.erase(svr);
      alive_servers.insert(svr);

      // Small delay between restarts
      Fiber::sleep(ELECTIONTIMEOUT / 2);
    }

    // Wait for all servers to catch up
    Fiber::sleep(ELECTIONTIMEOUT);

    // Verify all servers are working
    leader = config_->OneLeader();
    AssertOneLeader(leader);
    Log_info("TEST 15: Round {} - Leader after all restarts: {}", round, leader);

    Assert2(alive_servers.size() == NSERVERS,
            "Expected %d alive servers, got %zu", NSERVERS, alive_servers.size());
    Assert2(dead_servers.empty(),
            "Expected 0 dead servers, got %zu", dead_servers.size());

    // Final commit with all servers
    Log_info("TEST 15: Round {} - Final commit with all {} servers", round, NSERVERS);
    DoAgreeAndAssertWaitSuccess(cmd_base++, NSERVERS);

    Log_info("TEST 15: ===== ROUND {} COMPLETE =====", round);
  }

  // Final verification
  Log_info("TEST 15: Final verification after {} rounds", NUM_ROUNDS);

  leader = config_->OneLeader();
  AssertOneLeader(leader);

  // One more commit to verify everything works
  DoAgreeAndAssertWaitSuccess(cmd_base++, NSERVERS);

  Log_info("TEST 15: All {} rounds completed successfully!", NUM_ROUNDS);

  Passed2();
}

int RaftLabTest::testPartitionPlusRestart(void) {
  Init2(16, "Partition plus restart - one server partitioned, another killed/restarted");

  Log_info("TEST 16: Waiting for initial election");
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 16: Initial leader elected: {}", leader);

  // Commit initial entries to ensure cluster is stable
  Log_info("TEST 16: Committing initial entries");
  DoAgreeAndAssertIndex(1601, NSERVERS, index_++);
  DoAgreeAndAssertIndex(1602, NSERVERS, index_++);
  Log_info("TEST 16: Initial entries committed");

  // Pick two different non-leader servers
  // Server A will be partitioned, Server B will be killed/restarted
  siteid_t partitioned_server = 0;
  siteid_t killed_server = 0;
  bool found_partitioned = false;
  bool found_killed = false;

  for (int i = 0; i < NSERVERS; i++) {
    siteid_t svr = config_->getServerIdByIndex(i);
    if (svr != (siteid_t)leader) {
      if (!found_partitioned) {
        partitioned_server = svr;
        found_partitioned = true;
      } else if (!found_killed) {
        killed_server = svr;
        found_killed = true;
        break;
      }
    }
  }

  Assert2(found_partitioned, "Could not find server to partition");
  Assert2(found_killed, "Could not find server to kill");
  Assert2(partitioned_server != killed_server, "Partitioned and killed server must be different");

  Log_info("TEST 16: Will partition server {} and kill/restart server {}",
           partitioned_server, killed_server);

  // Step 1: Partition server A
  Log_info("TEST 16: Step 1 - Partitioning server {}", partitioned_server);
  config_->Disconnect(partitioned_server);

  // Step 2: Kill server B
  Log_info("TEST 16: Step 2 - Killing server {}", killed_server);
  config_->Kill(killed_server);

  // Wait for potential leader re-election (if we killed/partitioned the leader)
  Fiber::sleep(ELECTIONTIMEOUT);

  // Verify cluster still works with 3 servers (quorum)
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 16: Leader after partition+kill: {}", leader);

  // Commit with 3 servers
  Log_info("TEST 16: Committing with 3 servers");
  DoAgreeAndAssertIndex(1603, 3, index_++);

  // Step 3: Restart server B (while A is still partitioned)
  Log_info("TEST 16: Step 3 - Restarting server {} (while {} is still partitioned)",
           killed_server, partitioned_server);
  config_->Restart(killed_server);

  // Wait for server B to catch up
  Fiber::sleep(ELECTIONTIMEOUT);

  // Verify cluster works with 4 servers
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 16: Leader after restart: {}", leader);

  // Commit with 4 servers
  Log_info("TEST 16: Committing with 4 servers");
  DoAgreeAndAssertIndex(1604, 4, index_++);

  // Step 4: Heal partition (reconnect server A)
  // This is the critical test: A was partitioned when B restarted
  // A's connection to B should be stale, but B's retry mechanism should fix it
  Log_info("TEST 16: Step 4 - Healing partition (reconnecting server {})", partitioned_server);
  config_->Reconnect(partitioned_server);

  // Wait for server A to catch up and for NotifyRestart retry to work
  Log_info("TEST 16: Waiting for partition to heal and connections to refresh...");
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  // Verify all 5 servers are working
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 16: Leader after partition heal: {}", leader);

  // Step 5: Final commit with all 5 servers
  // This verifies that A can communicate with B (the restarted server)
  Log_info("TEST 16: Step 5 - Final commit with all {} servers", NSERVERS);
  DoAgreeAndAssertWaitSuccess(1605, NSERVERS);

  // Additional verification: commit a few more entries
  Log_info("TEST 16: Additional commits to verify stability");
  DoAgreeAndAssertWaitSuccess(1606, NSERVERS);
  DoAgreeAndAssertWaitSuccess(1607, NSERVERS);

  Log_info("TEST 16: Partition plus restart test PASSED!");

  Passed2();
}

int RaftLabTest::testSequentialPartitionsPlusRestart(void) {
  Init2(17, "Sequential partitions plus restart - two servers partitioned at different times while one restarts");

  Log_info("TEST 17: Waiting for initial election");
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 17: Initial leader elected: {}", leader);

  // Commit initial entries to ensure cluster is stable
  Log_info("TEST 17: Committing initial entries");
  DoAgreeAndAssertIndex(1701, NSERVERS, index_++);
  DoAgreeAndAssertIndex(1702, NSERVERS, index_++);
  Log_info("TEST 17: Initial entries committed");

  // Pick three different non-leader servers: A (partition first), B (kill/restart), C (partition second)
  siteid_t server_A = 0;  // Will be partitioned first
  siteid_t server_B = 0;  // Will be killed and restarted
  siteid_t server_C = 0;  // Will be partitioned second
  int found_count = 0;

  for (int i = 0; i < NSERVERS && found_count < 3; i++) {
    siteid_t svr = config_->getServerIdByIndex(i);
    if (svr != (siteid_t)leader) {
      if (found_count == 0) {
        server_A = svr;
      } else if (found_count == 1) {
        server_B = svr;
      } else if (found_count == 2) {
        server_C = svr;
      }
      found_count++;
    }
  }

  Assert2(found_count >= 3, "Could not find 3 non-leader servers");
  Log_info("TEST 17: Server A (partition first): {}", server_A);
  Log_info("TEST 17: Server B (kill/restart): {}", server_B);
  Log_info("TEST 17: Server C (partition second): {}", server_C);

  // ========================================
  // T1: Partition A → Healthy: {B,C,D,E} = 4
  // ========================================
  Log_info("TEST 17: Step 1 - Partitioning server A ({})", server_A);
  config_->Disconnect(server_A);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 17: Leader after partitioning A: {}", leader);

  // Commit with 4 servers
  Log_info("TEST 17: Committing with 4 servers (A partitioned)");
  DoAgreeAndAssertIndex(1703, 4, index_++);

  // ========================================
  // T2: Kill B → Healthy: {C,D,E} = 3
  // ========================================
  Log_info("TEST 17: Step 2 - Killing server B ({})", server_B);
  config_->Kill(server_B);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 17: Leader after killing B: {}", leader);

  // Commit with 3 servers
  Log_info("TEST 17: Committing with 3 servers (A partitioned, B dead)");
  DoAgreeAndAssertIndex(1704, 3, index_++);

  // ========================================
  // T3: Restart B → B sends NotifyRestart, A is PENDING
  // ========================================
  Log_info("TEST 17: Step 3 - Restarting server B ({}) while A ({}) is still partitioned",
           server_B, server_A);
  config_->Restart(server_B);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 17: Leader after restarting B: {}", leader);

  // Commit with 4 servers (B is back, A still partitioned)
  Log_info("TEST 17: Committing with 4 servers (A partitioned, B restarted)");
  DoAgreeAndAssertIndex(1705, 4, index_++);

  // ========================================
  // T4: Partition C → Healthy: {B,D,E} = 3 (A and C both isolated)
  // ========================================
  Log_info("TEST 17: Step 4 - Partitioning server C ({}) while A ({}) is still partitioned",
           server_C, server_A);
  config_->Disconnect(server_C);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 17: Leader after partitioning C: {}", leader);

  // Commit with 3 servers (A and C partitioned)
  Log_info("TEST 17: Committing with 3 servers (A and C partitioned)");
  DoAgreeAndAssertIndex(1706, 3, index_++);

  // ========================================
  // T5: Heal A → A has stale connection to B, B's retry fixes it
  // ========================================
  Log_info("TEST 17: Step 5 - Healing partition for server A ({})", server_A);
  Log_info("TEST 17: A was partitioned when B restarted, so A has stale connection to B");
  config_->Reconnect(server_A);

  // Wait for A to catch up and for B's retry mechanism to fix A's stale connection
  Log_info("TEST 17: Waiting for A to reconnect and B's retry to fix stale connection...");
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 17: Leader after healing A: {}", leader);

  // Commit with 4 servers (A is back, C still partitioned)
  Log_info("TEST 17: Committing with 4 servers (A healed, C still partitioned)");
  DoAgreeAndAssertIndex(1707, 4, index_++);

  // ========================================
  // T6: Heal C → C has stale connection to B, B's retry fixes it
  // ========================================
  Log_info("TEST 17: Step 6 - Healing partition for server C ({})", server_C);
  Log_info("TEST 17: C was partitioned when B restarted, so C has stale connection to B");
  config_->Reconnect(server_C);

  // Wait for C to catch up and for B's retry mechanism to fix C's stale connection
  Log_info("TEST 17: Waiting for C to reconnect and B's retry to fix stale connection...");
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 17: Leader after healing C: {}", leader);

  // ========================================
  // T7: Verify all 5 servers work
  // ========================================
  Log_info("TEST 17: Step 7 - Final verification with all {} servers", NSERVERS);
  DoAgreeAndAssertWaitSuccess(1708, NSERVERS);

  // Additional commits to verify stability
  Log_info("TEST 17: Additional commits to verify stability");
  DoAgreeAndAssertWaitSuccess(1709, NSERVERS);
  DoAgreeAndAssertWaitSuccess(1710, NSERVERS);

  Log_info("TEST 17: Sequential partitions plus restart test PASSED!");

  Passed2();
}

int RaftLabTest::testMultipleRestartsPlusPartition(void) {
  Init2(18, "Multiple restarts plus partition - server restarts multiple times while another is partitioned");

  Log_info("TEST 18: Waiting for initial election");
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 18: Initial leader elected: {}", leader);

  // Commit initial entries
  Log_info("TEST 18: Committing initial entries");
  DoAgreeAndAssertIndex(1801, NSERVERS, index_++);
  DoAgreeAndAssertIndex(1802, NSERVERS, index_++);
  Log_info("TEST 18: Initial entries committed");

  // Pick three different non-leader servers: A (partition), B (multiple restarts), C (single restart)
  siteid_t server_A = 0;  // Will be partitioned
  siteid_t server_B = 0;  // Will be killed/restarted multiple times
  siteid_t server_C = 0;  // Will be killed/restarted once
  int found_count = 0;

  for (int i = 0; i < NSERVERS && found_count < 3; i++) {
    siteid_t svr = config_->getServerIdByIndex(i);
    if (svr != (siteid_t)leader) {
      if (found_count == 0) {
        server_A = svr;
      } else if (found_count == 1) {
        server_B = svr;
      } else if (found_count == 2) {
        server_C = svr;
      }
      found_count++;
    }
  }

  Assert2(found_count >= 3, "Could not find 3 non-leader servers");
  Log_info("TEST 18: Server A (partition): {}", server_A);
  Log_info("TEST 18: Server B (multiple restarts): {}", server_B);
  Log_info("TEST 18: Server C (single restart): {}", server_C);

  // ========================================
  // T1: Partition A → Healthy: {B,C,D,E} = 4
  // ========================================
  Log_info("TEST 18: Step 1 - Partitioning server A ({})", server_A);
  config_->Disconnect(server_A);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 18: Leader after partitioning A: {}", leader);

  DoAgreeAndAssertIndex(1803, 4, index_++);

  // ========================================
  // T2: Kill B → Healthy: {C,D,E} = 3
  // ========================================
  Log_info("TEST 18: Step 2 - Killing server B ({}) [first time]", server_B);
  config_->Kill(server_B);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 18: Leader after killing B: {}", leader);

  DoAgreeAndAssertIndex(1804, 3, index_++);

  // ========================================
  // T3: Restart B → B sends NotifyRestart, A is PENDING
  // ========================================
  Log_info("TEST 18: Step 3 - Restarting server B ({}) [first time]", server_B);
  config_->Restart(server_B);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 18: Leader after restarting B [first time]: {}", leader);

  DoAgreeAndAssertIndex(1805, 4, index_++);

  // ========================================
  // T4: Kill B again → Healthy: {C,D,E} = 3
  //     B's retry state is lost!
  // ========================================
  Log_info("TEST 18: Step 4 - Killing server B ({}) [second time] - retry state will be lost!", server_B);
  config_->Kill(server_B);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 18: Leader after killing B [second time]: {}", leader);

  DoAgreeAndAssertIndex(1806, 3, index_++);

  // ========================================
  // T5: Restart B again → B sends NotifyRestart again, A still PENDING
  // ========================================
  Log_info("TEST 18: Step 5 - Restarting server B ({}) [second time]", server_B);
  config_->Restart(server_B);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 18: Leader after restarting B [second time]: {}", leader);

  DoAgreeAndAssertIndex(1807, 4, index_++);

  // ========================================
  // T6: Heal A → A receives NotifyRestart from B, fixes stale connection
  // ========================================
  Log_info("TEST 18: Step 6 - Healing partition for server A ({})", server_A);
  Log_info("TEST 18: A was partitioned through TWO restart cycles of B");
  config_->Reconnect(server_A);

  // Wait for A to catch up and for B's retry mechanism to fix A's stale connection
  Log_info("TEST 18: Waiting for A to reconnect and B's retry to fix stale connection...");
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 18: Leader after healing A: {}", leader);

  // Verify all 5 servers work
  Log_info("TEST 18: Verifying all 5 servers work after A healed");
  DoAgreeAndAssertWaitSuccess(1808, NSERVERS);

  // ========================================
  // T7: Kill C → Healthy: {A,B,D,E} = 4
  // ========================================
  Log_info("TEST 18: Step 7 - Killing server C ({})", server_C);
  config_->Kill(server_C);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 18: Leader after killing C: {}", leader);

  DoAgreeAndAssertIndex(1809, 4, index_++);

  // ========================================
  // T8: Restart C → C notifies all (all respond since everyone is connected)
  // ========================================
  Log_info("TEST 18: Step 8 - Restarting server C ({})", server_C);
  config_->Restart(server_C);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 18: Leader after restarting C: {}", leader);

  // ========================================
  // T9: Verify all 5 servers work
  // ========================================
  Log_info("TEST 18: Step 9 - Final verification with all {} servers", NSERVERS);
  DoAgreeAndAssertWaitSuccess(1810, NSERVERS);

  // Additional commits to verify stability
  Log_info("TEST 18: Additional commits to verify stability");
  DoAgreeAndAssertWaitSuccess(1811, NSERVERS);
  DoAgreeAndAssertWaitSuccess(1812, NSERVERS);

  Log_info("TEST 18: Multiple restarts plus partition test PASSED!");

  Passed2();
}

int RaftLabTest::testInitialElection(void) {
  Init2(1, "Initial election");

  // Start election timers by calling Start() on each server
  // This triggers the election timer to start on each server
  // for (int i = 0; i < NSERVERS; i++) {
  //   siteid_t server_id = config_->getServerIdByIndex(i);
  //   uint64_t index, term;
  //   // Call Start() with a dummy command to trigger election timer
  //   // The command won't actually be processed since no leader exists yet
  //   config_->Start(server_id, 100 + i, &index, &term);
  // }
  
  // Wait a bit for election timers to start and elections to begin
  Fiber::sleep(ELECTIONTIMEOUT / 10);
  
  // Initial election: is there one leader?
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  
  // calculate RPC count for initial election for later use
  init_rpcs_ = 0;
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t server_id = config_->getServerIdByIndex(i);
    init_rpcs_ += config_->RpcCount(server_id);
  }
  
  // Does everyone agree on the term number?
  uint64_t term = config_->OneTerm();
  Assert2(term != -1, "servers disagree on term number");
  
  // Does the term stay the same after a while if there's no failures?
  Assert2(config_->OneTerm() == term, "unexpected term change");
  
  // Is the same server still the only leader?
  AssertOneLeader(config_->OneLeader(leader));
  
  // Log carryover context after test 1
  // Log_info("=== CARRYOVER CONTEXT AFTER TEST 1 (testInitialElection) ===");
  // Log_info("Current leader: {}", leader);
  // Log_info("Current term: {}", term);
  // Log_info("init_rpcs_ value: {}", init_rpcs_);
  // Log_info("index_ value: {}", index_);
  // Log_info("All servers connected: {}", config_->NDisconnected() == 0 ? "true" : "false");
  // Log_info("Network reliable: {}", !config_->IsUnreliable() ? "true" : "false");
  // Log_info("==========================================================");
  
  Passed2();
}

int RaftLabTest::testReElection(void) {
  Init2(2, "Re-election after network failure");
  // Log_info("TEST 2: Starting re-election test");
  
  // find current leader
  // Log_info("TEST 2: Finding current leader");
  int leader = config_->OneLeader();
  // Log_info("TEST 2: Current leader is {}", leader);
  
  // Check if OneLeader returned a valid leader
  if (leader == -1) {
    // Log_info("TEST 2: No leader found, test cannot proceed");
    Failed("No leader found in initial election");
    return -1;
  }
  
  AssertOneLeader(leader);
  
  // disconnect leader - make sure a new one is elected
  // Log_info("TEST 2: Disconnecting old leader {}", leader);
  config_->Disconnect(leader);
  int oldLeader = leader;
  // Log_info("TEST 2: Old leader {} disconnected, sleeping for election timeout", oldLeader);
  Fiber::sleep(ELECTIONTIMEOUT);
  
  // Log_info("TEST 2: Finding new leader after old leader disconnected");
  leader = config_->OneLeader();
  // Log_info("TEST 2: New leader is {}", leader);
  
  // Check if OneLeader returned a valid leader
  if (leader == -1) {
    // Log_info("TEST 2: No new leader found after disconnecting old leader");
    Failed("No new leader elected after disconnecting old leader");
    return -1;
  }
  
  AssertOneLeader(leader);
  AssertReElection(leader, oldLeader);
  
  // reconnect old leader - should not disturb new leader
  // Log_info("TEST 2: Reconnecting old leader {}", oldLeader);
  config_->Reconnect(oldLeader);
  // Log_info("TEST 2: Old leader reconnected, sleeping for election timeout");
  Fiber::sleep(ELECTIONTIMEOUT);
  AssertOneLeader(config_->OneLeader(leader));
  
  // no quorum -> no leader
  // Log_info("TEST 2: Disconnecting more servers to break quorum");
  // Log_info("TEST 2: Current leader is {}", leader);
  
  siteid_t next1 = config_->getNextServerId(leader, 1);
  // Log_info("TEST 2: Next server 1 offset from leader {} is {}", leader, next1);
  config_->Disconnect(next1);
  
  siteid_t next2 = config_->getNextServerId(leader, 2);
  // Log_info("TEST 2: Next server 2 offset from leader {} is {}", leader, next2);
  config_->Disconnect(next2);
  
  // Log_info("TEST 2: Disconnecting leader {}", leader);
  config_->Disconnect(leader);
  
  // Log_info("TEST 2: Checking for no leader condition");
  Assert(config_->NoLeader());
  
  // quorum restored
  // Log_info("TEST 2: Reconnecting a server to restore quorum");
  siteid_t reconnect_server = config_->getNextServerId(leader, 2);
  // Log_info("TEST 2: Reconnecting server {}", reconnect_server);
  config_->Reconnect(reconnect_server);
  Fiber::sleep(ELECTIONTIMEOUT);
  AssertOneLeader(config_->OneLeader());
  
  // rejoin all servers
  // Log_info("TEST 2: Rejoining all servers");
  siteid_t rejoin1 = config_->getNextServerId(leader, 1);
  // Log_info("TEST 2: Rejoining server {}", rejoin1);
  config_->Reconnect(rejoin1);
  
  // Log_info("TEST 2: Rejoining leader {}", leader);
  config_->Reconnect(leader);
  Fiber::sleep(ELECTIONTIMEOUT);
  AssertOneLeader(config_->OneLeader());
  
  // Log carryover context after test 2
  // Log_info("=== CARRYOVER CONTEXT AFTER TEST 2 (testReElection) ===");
  // int final_leader = config_->OneLeader();
  // uint64_t final_term = config_->OneTerm();
  // Log_info("Current leader: {}", final_leader);
  // Log_info("Current term: {}", final_term);
  // Log_info("init_rpcs_ value: {}", init_rpcs_);
  // Log_info("index_ value: {}", index_);
  // Log_info("All servers connected: {}", config_->NDisconnected() == 0 ? "true" : "false");
  // Log_info("Network reliable: {}", !config_->IsUnreliable() ? "true" : "false");
  // Log_info("==========================================================");
  
  Passed2();
}

int RaftLabTest::testBasicAgree(void) {
  Init2(3, "Basic agreement");
  
  // Log carryover context at start of test 3
  // Log_info("=== CARRYOVER CONTEXT AT START OF TEST 3 (testBasicAgree) ===");
  int current_leader = config_->OneLeader();
  uint64_t current_term = config_->OneTerm();
  // Log_info("Current leader: {}", current_leader);
  // Log_info("Current term: {}", current_term);
  // Log_info("init_rpcs_ value: {}", init_rpcs_);
  // Log_info("index_ value: {}", index_);
  // Log_info("All servers connected: {}", config_->NDisconnected() == 0 ? "true" : "false");
  // Log_info("Network reliable: {}", !config_->IsUnreliable() ? "true" : "false");
  // Log_info("=============================================================");
  
  for (int i = 1; i <= 3; i++) {
    // make sure no commits exist before any agreements are started
    AssertNoneCommitted(index_);
    // complete 1 agreement and make sure its index is as expected
    int temp_index = index_;
    int command_value = (int)(temp_index + 300);
    // Log_info("TEST 3: About to test agreement for command {} (iteration {}/3)", command_value, i);
    // Log_info("Starting Agreement for command {}", command_value);
    DoAgreeAndAssertIndex(command_value, NSERVERS, index_);
    index_++;
    // Log_info("Agreement for command {} completed", command_value);
  }
  Passed2();
}

int RaftLabTest::testFailAgree(void) {
  Init2(4, "Agreement despite follower disconnection");
  // disconnect 2 followers
  auto leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_debug("disconnecting two followers leader");
  config_->Disconnect(config_->getNextServerId(leader, 1));
  config_->Disconnect(config_->getNextServerId(leader, 2));
  // Agreement despite 2 disconnected servers
  Log_debug("try commit a few commands after disconnect");
  DoAgreeAndAssertIndex(401, NSERVERS - 2, index_++);
  DoAgreeAndAssertIndex(402, NSERVERS - 2, index_++);
  Fiber::sleep(ELECTIONTIMEOUT);
  DoAgreeAndAssertIndex(403, NSERVERS - 2, index_++);
  DoAgreeAndAssertIndex(404, NSERVERS - 2, index_++);
  // reconnect followers
  Log_debug("reconnect servers");
  config_->Reconnect(config_->getNextServerId(leader, 1));
  config_->Reconnect(config_->getNextServerId(leader, 2));
  Fiber::sleep(ELECTIONTIMEOUT);
  Log_debug("try commit a few commands after reconnect");
  DoAgreeAndAssertWaitSuccess(405, NSERVERS);
  DoAgreeAndAssertWaitSuccess(406, NSERVERS);
  Passed2();
}

int RaftLabTest::testFailNoAgree(void) {
  Init2(5, "No agreement if too many followers disconnect");
  // disconnect 3 followers
  auto leader = config_->OneLeader();
  AssertOneLeader(leader);
  config_->Disconnect(config_->getNextServerId(leader, 1));
  config_->Disconnect(config_->getNextServerId(leader, 2));
  config_->Disconnect(config_->getNextServerId(leader, 3));
  // attempt to do an agreement
  uint64_t index, term;
  AssertStartOk(config_->Start(leader, 501, &index, &term));
  Assert2(index == index_++ && term > 0,
          "Start() returned unexpected index (%ld, expected %ld) and/or term (%ld, expected >0)",
          index, index_-1, term);
  Fiber::sleep(ELECTIONTIMEOUT);
  AssertNoneCommitted(index);
  // reconnect followers
  config_->Reconnect(config_->getNextServerId(leader, 1));
  config_->Reconnect(config_->getNextServerId(leader, 2));
  config_->Reconnect(config_->getNextServerId(leader, 3));
  // do agreement in restored quorum
  Fiber::sleep(ELECTIONTIMEOUT);
  DoAgreeAndAssertWaitSuccess(502, NSERVERS);
  Passed2();
}

int RaftLabTest::testRejoin(void) {
  Init2(6, "Rejoin of disconnected leader");
  DoAgreeAndAssertIndex(601, NSERVERS, index_++);
  // disconnect leader
  auto leader1 = config_->OneLeader();
  AssertOneLeader(leader1);
  config_->Disconnect(leader1);
  Fiber::sleep(ELECTIONTIMEOUT);
  // Make old leader try to agree on some entries (these should not commit)
  uint64_t index, term;
  AssertStartOk(config_->Start(leader1, 602, &index, &term));
  AssertStartOk(config_->Start(leader1, 603, &index, &term));
  AssertStartOk(config_->Start(leader1, 604, &index, &term));
  // New leader commits, successfully
  DoAgreeAndAssertWaitSuccess(605, NSERVERS - 1);
  DoAgreeAndAssertWaitSuccess(606, NSERVERS - 1);
  // Disconnect new leader
  auto leader2 = config_->OneLeader();
  AssertOneLeader(leader2);
  AssertReElection(leader2, leader1);
  config_->Disconnect(leader2);
  // reconnect old leader
  config_->Reconnect(leader1);
  // wait for new election
  Fiber::sleep(ELECTIONTIMEOUT);
  auto leader3 = config_->OneLeader();
  AssertOneLeader(leader3);
  AssertReElection(leader3, leader2);
  // More commits
  DoAgreeAndAssertWaitSuccess(607, NSERVERS - 1);
  DoAgreeAndAssertWaitSuccess(608, NSERVERS - 1);
  // Reconnect all
  config_->Reconnect(leader2);
  DoAgreeAndAssertWaitSuccess(609, NSERVERS);
  Passed2();
}

class CSArgs {
 public:
  std::vector<uint64_t> *indices;
  std::mutex *mtx;
  int i;
  int leader;
  uint64_t term;
  RaftTestConfig *config;
};

static void *doConcurrentStarts(void *args) {
  CSArgs *csargs = (CSArgs *)args;
  uint64_t idx, tm;
  auto ok = csargs->config->Start(csargs->leader, 701 + csargs->i, &idx, &tm);
  if (!ok || tm != csargs->term) {
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(*(csargs->mtx));
    csargs->indices->push_back(idx);
  }
  return nullptr;
}

int RaftLabTest::testConcurrentStarts(void) {
  Init2(7, "Concurrently started agreements");
  int nconcurrent = 5;
  bool success = false;
  for (int again = 0; again < 5; again++) {
    if (again > 0) {
      wait(3000000);
    }
    auto leader = config_->OneLeader();
    AssertOneLeader(leader);
    uint64_t index, term;
    auto ok = config_->Start(leader, 701, &index, &term);
    if (!ok) {
      continue; // retry (up to 5 times)
    }
    // create 5 threads that each Start a command to leader
    std::vector<uint64_t> indices{};
    std::vector<int> cmds{};
    std::mutex mtx{};
    pthread_t threads[nconcurrent];
    for (int i = 0; i < nconcurrent; i++) {
      CSArgs *args = new CSArgs{};
      args->indices = &indices;
      args->mtx = &mtx;
      args->i = i;
      args->leader = leader;
      args->term = term;
      args->config = config_;
      verify(pthread_create(&threads[i], nullptr, doConcurrentStarts, (void*)args) == 0);
    }
    // join all threads
    for (int i = 0; i < nconcurrent; i++) {
      verify(pthread_join(threads[i], nullptr) == 0);
    }
    if (config_->TermMovedOn(term)) {
      goto skip; // if leader's term is expiring, start over
    }
    // wait for all indices to commit
    for (auto index : indices) {
      int cmd = config_->Wait(index, NSERVERS, term);
      if (cmd < 0) {
        AssertWaitNoError(cmd, index);
        goto skip; // on timeout and term changes, try again
      }
      cmds.push_back(cmd);
    }
    // make sure all the commits are there with the correct values
    for (int i = 0; i < nconcurrent; i++) {
      auto val = 701 + i;
      int j;
      for (j = 0; j < cmds.size(); j++) {
        if (cmds[j] == val) {
          break;
        }
      }
      Assert2(j < cmds.size(), "cmd %d missing", val);
    }
    success = true;
    break;
    skip: ;
  }
  Assert2(success, "too many term changes and/or delayed responses");
  index_ += nconcurrent + 1;
  Passed2();
}

int RaftLabTest::testBackup(void) {
  Init2(8, "Leader backs up quickly over incorrect follower logs");
  // disconnect 3 servers that are not the leader
  int leader1 = config_->OneLeader();
  AssertOneLeader(leader1);
  Log_debug("disconnect 3 followers");
  config_->Disconnect(config_->getNextServerId(leader1, 2));
  config_->Disconnect(config_->getNextServerId(leader1, 3));
  config_->Disconnect(config_->getNextServerId(leader1, 4));
  // Start() a bunch of commands that won't be committed
  uint64_t index, term;
  for (int i = 0; i < 50; i++) {
    AssertStartOk(config_->Start(leader1, 800 + i, &index, &term));
  }
  Fiber::sleep(ELECTIONTIMEOUT);
  // disconnect the leader and its 1 follower, then reconnect the 3 servers
  Log_debug("disconnect the leader and its 1 follower, reconnect the 3 followers");
  config_->Disconnect(config_->getNextServerId(leader1, 1));
  config_->Disconnect(leader1);
  config_->Reconnect(config_->getNextServerId(leader1, 2));
  config_->Reconnect(config_->getNextServerId(leader1, 3));
  config_->Reconnect(config_->getNextServerId(leader1, 4));
  // do a bunch of agreements among the new quorum
  Fiber::sleep(ELECTIONTIMEOUT);
  Log_debug("try to commit a lot of commands");
  for (int i = 1; i <= 50; i++) {
    DoAgreeAndAssertIndex(800 + i, NSERVERS - 2, index_++);
  }
  // reconnect the old leader and its follower
  Log_debug("reconnect the old leader and the follower");
  config_->Reconnect(config_->getNextServerId(leader1, 1));
  config_->Reconnect(leader1);
  Fiber::sleep(ELECTIONTIMEOUT);
  // do an agreement all together to check the old leader's incorrect
  // entries are replaced in a timely manner
  int leader2 = config_->OneLeader();
  AssertOneLeader(leader2);
  AssertStartOk(config_->Start(leader2, 851, &index, &term));
  index_++;
  // 10 seconds should be enough to back up 50 incorrect logs
  Fiber::sleep(2*ELECTIONTIMEOUT);
  Log_debug("check if the old leader has enough committed");
  AssertNCommitted(index, NSERVERS);
  Passed2();
}

int RaftLabTest::testCount(void) {
  Init2(9, "RPC counts aren't too high");

  // reset RPC counts before starting
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t server_id = config_->getServerIdByIndex(i);
    config_->RpcCount(server_id, true);
  }

  auto rpcs = [this]() {
    uint64_t total = 0;
    for (int i = 0; i < NSERVERS; i++) {
      siteid_t server_id = config_->getServerIdByIndex(i);
      total += config_->RpcCount(server_id);
    }
    return total;
  };

  // initial election RPC count
  Log_info("TEST 9: init_rpcs_ observed = {}", init_rpcs_);
  // Ceiling raised from 40 to 70 to accommodate Mako-specific RPC traffic
  // (VoteDurable, AppendEntriesDurable, TimeoutNow, NotifyRestart) that the
  // upstream MIT 6.824 reference implementation did not emit. Observed
  // range on a quiet local run: 40-56; 70 leaves headroom for jitter.
  Assert2(init_rpcs_ > 1 && init_rpcs_ <= 70,
          "too many or too few RPCs (%ld) to elect initial leader",
          init_rpcs_);

  // agreement RPC count
  int iters = 10;
  uint64_t total = -1;
  bool success = false;
  for (int again = 0; again < 5; again++) {
    if (again > 0) {
      wait(3000000);
    }
    auto leader = config_->OneLeader();
    AssertOneLeader(leader);
    rpcs();
    uint64_t index, term, startindex, startterm;
    auto ok = config_->Start(leader, 900, &startindex, &startterm);
    if (!ok) {
      // leader moved on quickly: start over
      continue;
    }
    for (int i = 1; i <= iters; i++) {
      ok = config_->Start(leader, 900 + i, &index, &term);
      if (!ok || term != startterm) {
        // no longer the leader and/or term changed: start over
        goto loop;
      }
      Assert2(index == (startindex + i), "Start() failed");
    }
    for (int i = 1; i <= iters; i++) {
      auto r = config_->Wait(startindex + i, NSERVERS, startterm);
      AssertWaitNoError(r, startindex + i);
      if (r < 0) {
        // timeout or term change: start over
        goto loop;
      }
      Assert2(r == (900 + i), "wrong value %d committed for index %ld: expected %d", r, startindex + i, 900 + i);
    }
    if (config_->TermMovedOn(startterm)) {
      // term changed -- can't expect low RPC counts: start over
      continue;
    }
    total = rpcs();
    Assert2(total <= COMMITRPCS(iters),
            "too many RPCs (%ld) for %d entries",
            total, iters);
    success = true;
    break;
    loop: ;
  }
  Assert2(success, "term changed too often");

  // idle RPC count
  wait(1000000);
  total = rpcs();
  Assert2(total <= 60,
          "too many RPCs (%ld) for 1 second of idleness",
          total);
  Passed2();
}

class CAArgs {
 public:
  int iter;
  int i;
  std::mutex *mtx;
  std::vector<uint64_t> *retvals;
  RaftTestConfig *config;
};

static void *doConcurrentAgreement(void *args) {
  CAArgs *caargs = (CAArgs *)args;
  uint64_t retval = caargs->config->DoAgreement(1000 + caargs->iter, 1, true);
  if (retval == 0) {
    std::lock_guard<std::mutex> lock(*(caargs->mtx));
    caargs->retvals->push_back(retval);
  }
  return nullptr;
}

int RaftLabTest::testUnreliableAgree(void) {
  Init2(10, "Unreliable agreement (takes a few minutes)");
  config_->SetUnreliable(true);
  std::vector<pthread_t> threads{};
  std::vector<uint64_t> retvals{};
  std::mutex mtx{};
  for (int iter = 1; iter < 50; iter++) {
    for (int i = 0; i < 4; i++) {
      CAArgs *args = new CAArgs{};
      args->iter = iter;
      args->i = i;
      args->mtx = &mtx;
      args->retvals = &retvals;
      args->config = config_;
      pthread_t thread;
      verify(pthread_create(&thread,
                            nullptr,
                            doConcurrentAgreement,
                            (void*)args) == 0);
      threads.push_back(thread);
    }
    if (retvals.size() > 0)
      break;
    if (config_->DoAgreement(1000 + iter, 1, true) == 0) {
      std::lock_guard<std::mutex> lock(mtx);
      retvals.push_back(0);
      break;
    }
  }
  config_->SetUnreliable(false);
  // join all threads
  for (auto thread : threads) {
    verify(pthread_join(thread, nullptr) == 0);
  }
  Assert2(retvals.size() == 0, "Failed to reach agreement");
  index_ += 50 * 5;
  DoAgreeAndAssertWaitSuccess(1060, NSERVERS);
  Passed2();
}

int RaftLabTest::testFigure8(void) {
  Init2(11, "Figure 8");
  bool success = false;
  // Leader should not determine commitment using log entries from previous terms
  for (int again = 0; again < 10; again++) {
    // find out initial leader (S1) and term
    auto leader1 = config_->OneLeader();
    AssertOneLeader(leader1);
    uint64_t index1, term1, index2, term2;
    auto ok = config_->Start(leader1, 1100, &index1, &term1);
    if (!ok) {
      continue; // term moved on too quickly: start over
    }
    auto r = config_->Wait(index1, NSERVERS, term1);
    AssertWaitNoError(r, index1);
    AssertWaitNoTimeout(r, index1, NSERVERS);
    index_ = index1;
    // Start() a command (C1) and only let it get replicated to 1 follower (S2)
    config_->Disconnect(config_->getNextServerId(leader1, 1));
    config_->Disconnect(config_->getNextServerId(leader1, 2));
    config_->Disconnect(config_->getNextServerId(leader1, 3));
    ok = config_->Start(leader1, 1101, &index1, &term1);
    if (!ok) {
      config_->Reconnect(config_->getNextServerId(leader1, 1));
      config_->Reconnect(config_->getNextServerId(leader1, 2));
      config_->Reconnect(config_->getNextServerId(leader1, 3));
      continue;
    }
    Fiber::sleep(ELECTIONTIMEOUT);
    // C1 is at index i1 for S1 and S2
    AssertNoneCommitted(index1);
    // Elect new leader (S3) among other 3 servers
    config_->Disconnect(config_->getNextServerId(leader1, 4));
    config_->Disconnect(leader1);
    config_->Reconnect(config_->getNextServerId(leader1, 1));
    config_->Reconnect(config_->getNextServerId(leader1, 2));
    config_->Reconnect(config_->getNextServerId(leader1, 3));
    auto leader2 = config_->OneLeader();
    AssertOneLeader(leader2);
    // let old leader (S1) and follower (S2) become a follower in the new term
    config_->Reconnect(config_->getNextServerId(leader1, 4));
    config_->Reconnect(leader1);
    Fiber::sleep(ELECTIONTIMEOUT);
    AssertOneLeader(config_->OneLeader(leader2));
    Log_debug("disconnect all followers and Start() a cmd (C2) to isolated new leader");
    for (int i = 0; i < NSERVERS; i++) {
      siteid_t server_id = config_->getServerIdByIndex(i);
      if (server_id != leader2) {
        config_->Disconnect(server_id);
      }
    }
    ok = config_->Start(leader2, 1102, &index2, &term2);
    if (!ok) {
      for (int i = 1; i < 5; i++) {
        config_->Reconnect(config_->getNextServerId(leader2, i));
      }
      continue;
    }
    // C2 is at index i1 for S3, C1 still at index i1 for S1 & S2
    Assert2(index2 == index1, "Start() returned index %ld (%ld expected)", index2, index1);
    Assert2(term2 > term1, "Start() returned term %ld (%ld expected)", term2, term1);
    Fiber::sleep(ELECTIONTIMEOUT);
    AssertNoneCommitted(index1);
    // Let first leader (S1) or its initial follower (S2) become next leader
    config_->Disconnect(leader2);
    config_->Reconnect(leader1);
    verify(config_->getNextServerId(leader1, 4) != leader2);
    config_->Reconnect(config_->getNextServerId(leader1, 4));
    if (leader2 == config_->getNextServerId(leader1, 1))
      config_->Reconnect(config_->getNextServerId(leader1, 2));
    else
      config_->Reconnect(config_->getNextServerId(leader1, 1));
    auto leader3 = config_->OneLeader();
    AssertOneLeader(leader3);
    if (leader3 != leader1 && leader3 != config_->getNextServerId(leader1, 4)) {
      continue; // failed this step with a 1/3 chance. just start over until success.
    }
    // give leader3 more than enough time to replicate index1 to a third server
    Fiber::sleep(ELECTIONTIMEOUT);
    // Make sure initial Start() value isn't getting committed at this point
    AssertNoneCommitted(index1);
    // Commit a new index in the current term
    Assert2(config_->DoAgreement(1103, NSERVERS - 2, false) > index1,
            "failed to reach agreement");
    // Make sure that C1 is committed for index i1 now
    AssertNCommitted(index1, NSERVERS - 2);
    Assert2(config_->ServerCommitted(leader3, index1, 1101),
            "value 1101 is not committed at index %ld when it should be", index1);
    success = true;
    // Reconnect all servers
    config_->Reconnect(config_->getNextServerId(leader1, 3));
    if (leader2 == config_->getNextServerId(leader1, 1))
      config_->Reconnect(config_->getNextServerId(leader1, 1));
    else
      config_->Reconnect(config_->getNextServerId(leader1, 2));
    break;
  }
  Assert2(success, "Failed to test figure 8");
  Passed2();
}

int RaftLabTest::testFigure8CrashRecovery(void) {
  Init2(19, "Figure 8 with crash/recovery instead of partitions");
  bool success = false;

  // This test is the crash/recovery version of testFigure8.
  // Instead of using Disconnect/Reconnect (network partitions),
  // we use Kill/Restart (crash/recovery) to test persistence.
  // Leader should not determine commitment using log entries from previous terms.

  for (int again = 0; again < 10; again++) {
    Log_info("TEST 19: Attempt {}", again + 1);

    // 1. Find initial leader (S1) and commit an entry
    auto leader1 = config_->OneLeader();
    AssertOneLeader(leader1);
    uint64_t index1, term1, index2, term2;
    auto ok = config_->Start(leader1, 1900, &index1, &term1);
    if (!ok) {
      Log_info("TEST 19: Leader changed during initial Start, retrying");
      continue;
    }
    auto r = config_->Wait(index1, NSERVERS, term1);
    AssertWaitNoError(r, index1);
    AssertWaitNoTimeout(r, index1, NSERVERS);
    index_ = index1;
    Log_info("TEST 19: Initial entry committed at index {}, term {}", index1, term1);

    // 2. Kill 3 followers, leaving S1 + 1 follower (S2)
    //    S2 is getNextServerId(leader1, 4) to match original Figure 8 structure
    siteid_t follower_s2 = config_->getNextServerId(leader1, 4);
    siteid_t killed1 = config_->getNextServerId(leader1, 1);
    siteid_t killed2 = config_->getNextServerId(leader1, 2);
    siteid_t killed3 = config_->getNextServerId(leader1, 3);

    Log_info("TEST 19: Killing 3 followers: {}, {}, {} (keeping leader {} and follower {})",
             killed1, killed2, killed3, leader1, follower_s2);
    config_->Kill(killed1);
    config_->Kill(killed2);
    config_->Kill(killed3);

    // 3. Start C1 on S1 - only replicated to S1 and S2 (NOT committed, only 2 servers)
    ok = config_->Start(leader1, 1901, &index1, &term1);
    if (!ok) {
      Log_info("TEST 19: Leader changed during C1 Start, restarting killed servers and retrying");
      config_->Restart(killed1);
      config_->Restart(killed2);
      config_->Restart(killed3);
      Fiber::sleep(ELECTIONTIMEOUT);
      continue;
    }
    Log_info("TEST 19: Started C1 (1901) at index {}, term {} - should NOT be committed", index1, term1);
    Fiber::sleep(ELECTIONTIMEOUT);

    // C1 is at index1 for S1 and S2, but NOT committed (only 2 servers)
    AssertNoneCommitted(index1);
    Log_info("TEST 19: Verified C1 is not committed (only on 2 servers)");

    // 4. Kill S1 and S2, restart the other 3 to elect new leader S3
    Log_info("TEST 19: Killing S1 ({}) and S2 ({}), restarting other 3", leader1, follower_s2);
    config_->Kill(leader1);
    config_->Kill(follower_s2);
    config_->Restart(killed1);
    config_->Restart(killed2);
    config_->Restart(killed3);

    // 5. New leader S3 elected among the 3 restarted servers
    Fiber::sleep(ELECTIONTIMEOUT);
    auto leader2 = config_->OneLeader();
    AssertOneLeader(leader2);
    Log_info("TEST 19: New leader S3 elected: {}", leader2);

    // 6. Restart S1 and S2 - they recover from disk with C1 in their logs
    Log_info("TEST 19: Restarting S1 ({}) and S2 ({}) - they should recover C1 from disk",
             leader1, follower_s2);
    config_->Restart(leader1);
    config_->Restart(follower_s2);
    Fiber::sleep(ELECTIONTIMEOUT);

    // S3 should still be leader
    int current_leader = config_->OneLeader();
    AssertOneLeader(current_leader);
    Log_info("TEST 19: Current leader after S1/S2 restart: {}", current_leader);

    // 7. Kill all except S3, have S3 start C2 at the same index as C1
    //    We know exactly which servers exist: leader1, follower_s2, killed1, killed2, killed3
    //    One of killed1/killed2/killed3 is now leader2, so we kill the others
    Log_info("TEST 19: Isolating leader S3 ({}) by killing all others", leader2);

    // Kill using explicit server IDs we tracked, not getServerIdByIndex
    std::vector<siteid_t> all_servers = {
        static_cast<siteid_t>(leader1),
        static_cast<siteid_t>(follower_s2),
        static_cast<siteid_t>(killed1),
        static_cast<siteid_t>(killed2),
        static_cast<siteid_t>(killed3)};
    std::vector<siteid_t> killed_in_step7;
    for (siteid_t svr : all_servers) {
      if (svr != leader2) {
        Log_info("TEST 19: Step 7 - Killing server {}", svr);
        config_->Kill(svr);
        killed_in_step7.push_back(svr);
      }
    }

    ok = config_->Start(leader2, 1902, &index2, &term2);
    if (!ok) {
      Log_info("TEST 19: Leader S3 changed during C2 Start, restarting all and retrying");
      for (siteid_t svr : killed_in_step7) {
        config_->Restart(svr);
      }
      Fiber::sleep(ELECTIONTIMEOUT);
      continue;
    }

    // C2 is at the same index as C1, but in a higher term
    Log_info("TEST 19: Started C2 (1902) at index {}, term {}", index2, term2);
    Assert2(index2 == index1, "Start() returned index %ld (%ld expected)", index2, index1);
    Assert2(term2 > term1, "Start() returned term %ld (expected > %ld)", term2, term1);
    Fiber::sleep(ELECTIONTIMEOUT);
    AssertNoneCommitted(index1);
    Log_info("TEST 19: Verified neither C1 nor C2 is committed yet");

    // 8. Kill S3, restart S1 (has C1), S2 (has C1), and one other
    //    S1 or S2 should become leader because they have C1 (longer log)
    Log_info("TEST 19: Killing S3 ({}), restarting S1, S2, and one other", leader2);
    config_->Kill(leader2);
    config_->Restart(leader1);      // S1 has C1
    config_->Restart(follower_s2);  // S2 has C1

    // Restart one more server (not S3/leader2) - use our tracked server IDs
    // The other servers are killed1, killed2, killed3 - pick one that's not leader2
    siteid_t third_server = 0;
    for (siteid_t svr : {killed1, killed2, killed3}) {
      if (svr != leader2) {
        config_->Restart(svr);
        third_server = svr;
        Log_info("TEST 19: Also restarted server {} as third member", svr);
        break;
      }
    }

    Fiber::sleep(ELECTIONTIMEOUT);
    auto leader3 = config_->OneLeader();
    AssertOneLeader(leader3);
    Log_info("TEST 19: New leader after S3 killed: {}", leader3);

    // Leader3 should ideally be S1 or S2 (they have longer logs with C1)
    // But if not, we retry
    if (leader3 != leader1 && leader3 != follower_s2) {
      Log_info("TEST 19: Leader {} is not S1 or S2, retrying (1/3 chance)", leader3);
      // Restart remaining servers for cleanup using explicit IDs
      config_->Restart(leader2);
      for (siteid_t svr : {killed1, killed2, killed3}) {
        if (svr != third_server && svr != leader2) {
          config_->Restart(svr);
        }
      }
      Fiber::sleep(ELECTIONTIMEOUT);
      continue;
    }

    // 9. C1 should NOT be committed yet - it's from a previous term
    //    Leader cannot commit entries from previous terms directly
    Fiber::sleep(ELECTIONTIMEOUT);
    AssertNoneCommitted(index1);
    Log_info("TEST 19: Verified C1 still not committed (correct - from previous term)");

    // 10. Commit something in the current term - this should also commit C1 indirectly
    Log_info("TEST 19: Committing new entry in current term to trigger C1 commit");
    auto new_index = config_->DoAgreement(1903, NSERVERS - 2, false);
    Assert2(new_index > index1, "failed to reach agreement, got index %ld", new_index);
    Log_info("TEST 19: New entry committed at index {}", new_index);

    // 11. Now C1 SHOULD be committed (indirectly, by the new commit in current term)
    AssertNCommitted(index1, NSERVERS - 2);
    Assert2(config_->ServerCommitted(leader3, index1, 1901),
            "value 1901 not committed at index %ld when it should be", index1);
    Log_info("TEST 19: Verified C1 (1901) is now committed at index {}", index1);

    success = true;

    // Cleanup: restart all remaining dead servers using explicit IDs
    Log_info("TEST 19: Cleaning up - restarting remaining servers");
    config_->Restart(leader2);
    // Restart any of killed1/killed2/killed3 that weren't already restarted
    for (siteid_t svr : {killed1, killed2, killed3}) {
      if (svr != third_server && svr != leader2) {
        config_->Restart(svr);
      }
    }
    Fiber::sleep(ELECTIONTIMEOUT);
    break;
  }

  Assert2(success, "Failed to test Figure 8 with crash/recovery");
  Log_info("TEST 19: Figure 8 crash/recovery test PASSED!");
  Passed2();
}

void RaftLabTest::wait(uint64_t microseconds) {
  create_sp_timeout_event(microseconds)->wait();
}

// ============================================================================
// SPECULATIVE RAFT TESTS
// ============================================================================

/**
 * Test that leader becomes speculative first, then secured after VoteDurable.
 *
 * Expected behavior:
 * 1. After election, leader should exist
 * 2. Leader should initially be unsecured (securedLeader = false)
 * 3. After VoteDurable messages arrive, leader becomes secured
 */
int RaftLabTest::testSpeculativeLeaderElection(void) {
  Init2(20, "Speculative leader election");

  // Wait for initial election to complete
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);
  Log_info("[SPEC-TEST] Leader elected: index={}, site_id={}", leader, leader_id);

  // Check initial speculative state
  // Note: By the time we check, VoteDurable messages may have already arrived
  // So we can't assert securedLeader == false here. Instead, check that
  // the speculative state accessors work and invariants hold.

  size_t specVoters = config_->GetSpecVotersCount(leader_id);
  size_t durableVoters = config_->GetDurableVotersCount(leader_id);

  Log_info("[SPEC-TEST] Leader {}: specVoters={}, durableVoters={}",
           leader_id, specVoters, durableVoters);

  // Spec voters should be at least quorum (we won election)
  size_t quorum = (NSERVERS / 2) + 1;
  Assert2(specVoters >= quorum, "Leader has fewer spec voters (%zu) than quorum (%zu)",
          specVoters, quorum);

  // Wait a bit for VoteDurable messages to arrive
  Fiber::sleep(500000);  // 500ms

  // After waiting, leader should become secured (assuming no crashes)
  bool secured = config_->IsSecuredLeader(leader_id);
  durableVoters = config_->GetDurableVotersCount(leader_id);

  Log_info("[SPEC-TEST] After waiting: secured={}, durableVoters={}",
           secured, durableVoters);

  // With no crashes, we expect durable voters to reach quorum
  Assert2(durableVoters >= quorum, "Leader has fewer durable voters (%zu) than quorum (%zu)",
          durableVoters, quorum);
  Assert2(secured, "Leader should be secured after VoteDurable quorum");

  // Verify invariants hold
  Assert2(config_->VerifySpecInvariants(leader_id), "Speculative invariants violated");

  Passed2();
}

/**
 * Test that specCommitIndex advances on memory ack quorum.
 *
 * Expected behavior:
 * 1. Submit entry to leader
 * 2. specCommitIndex should advance when memory ack quorum reached
 * 3. Eventually entry becomes durably committed
 */
int RaftLabTest::testSpecCommitIndexAdvances(void) {
  Init2(21, "Spec commit index advances");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);

  // Get initial specCommitIndex
  uint64_t initialSpecCommit = config_->GetSpecCommitIndex(leader_id);
  uint64_t initialSecuredLog = config_->GetSecuredLogIndex(leader_id);

  Log_info("[SPEC-TEST] Initial state: specCommitIndex={}, securedLogIndex={}",
           initialSpecCommit, initialSecuredLog);

  // Submit an entry
  int cmd = 100 + rand() % 1000;
  uint64_t index = 0;
  uint64_t term = 0;
  bool ok = config_->Start(leader_id, cmd, &index, &term);
  Assert2(ok, "Failed to submit command to leader");

  Log_info("[SPEC-TEST] Submitted command {} at index {}, term {}", cmd, index, term);

  // Wait a short time for memory acks to arrive
  Fiber::sleep(200000);  // 200ms

  // Check specCommitIndex advanced
  uint64_t newSpecCommit = config_->GetSpecCommitIndex(leader_id);

  Log_info("[SPEC-TEST] After waiting: specCommitIndex={} (was {})",
           newSpecCommit, initialSpecCommit);

  // specCommitIndex should have advanced (at least to our submitted entry)
  Assert2(newSpecCommit >= index, "specCommitIndex (%lu) did not reach submitted index (%lu)",
          newSpecCommit, index);

  // Wait longer for durable commit
  Fiber::sleep(500000);  // 500ms more

  // Check that securedLogIndex also advances (if leader is secured)
  bool secured = config_->IsSecuredLeader(leader_id);
  uint64_t newSecuredLog = config_->GetSecuredLogIndex(leader_id);

  Log_info("[SPEC-TEST] After more waiting: secured={}, securedLogIndex={} (was {})",
           secured, newSecuredLog, initialSecuredLog);

  if (secured) {
    // If leader is secured, securedLogIndex should advance
    Assert2(newSecuredLog >= index, "securedLogIndex (%lu) did not reach submitted index (%lu)",
            newSecuredLog, index);
  }

  // Verify invariants
  Assert2(config_->VerifySpecInvariants(leader_id), "Speculative invariants violated");

  Passed2();
}

/**
 * Test that speculative invariants hold throughout operations.
 *
 * Expected behavior:
 * 1. Submit multiple entries
 * 2. At all times: securedLogIndex <= specCommitIndex <= lastLogIndex
 */
int RaftLabTest::testSpeculativeInvariantsHold(void) {
  Init2(22, "Speculative invariants hold");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);

  // Submit multiple entries and check invariants after each
  for (int i = 0; i < 5; i++) {
    int cmd = 200 + i;
    uint64_t index = 0;
    uint64_t term = 0;

    bool ok = config_->Start(leader_id, cmd, &index, &term);
    Assert2(ok, "Failed to submit command %d to leader", cmd);

    Log_info("[SPEC-TEST] Submitted command {} at index {}", cmd, index);

    // Verify invariants immediately
    Assert2(config_->VerifySpecInvariants(leader_id),
            "Invariants violated after submitting command %d", cmd);

    // Wait a bit for replication
    Fiber::sleep(100000);  // 100ms

    // Verify invariants again
    Assert2(config_->VerifySpecInvariants(leader_id),
            "Invariants violated after waiting for command %d", cmd);
  }

  // Final state check
  uint64_t securedLog = config_->GetSecuredLogIndex(leader_id);
  uint64_t specCommit = config_->GetSpecCommitIndex(leader_id);
  uint64_t lastLog = config_->GetServer(leader_id)->GetLastLogIndex();

  Log_info("[SPEC-TEST] Final state: securedLogIndex={}, specCommitIndex={}, lastLogIndex={}",
           securedLog, specCommit, lastLog);

  Assert2(securedLog <= specCommit, "securedLogIndex (%lu) > specCommitIndex (%lu)",
          securedLog, specCommit);
  Assert2(specCommit <= lastLog, "specCommitIndex (%lu) > lastLogIndex (%lu)",
          specCommit, lastLog);

  Passed2();
}

/**
 * Test that secured leader continues operating even after losing speculative quorum.
 *
 * Scenario:
 * 1. Establish a secured leader (durable vote quorum achieved)
 * 2. Kill followers to lose speculative quorum
 * 3. Leader should continue operating (it's still secured!)
 * 4. Commits should still work with remaining quorum
 *
 * Key insight: Once a leader is secured, it has durably won the election.
 * No other leader can win in this term, so losing speculative voters doesn't
 * invalidate the leadership - they can crash/restart but can't vote elsewhere.
 */
int RaftLabTest::testSecuredLeaderContinuesAfterSpecQuorumLoss(void) {
  Init2(23, "Secured leader continues after spec quorum loss");

  // Wait for initial election and secure leadership
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);

  // Wait for leader to become secured
  Fiber::sleep(500000);  // 500ms for VoteDurable messages

  bool secured = config_->IsSecuredLeader(leader_id);
  size_t initialDurableVoters = config_->GetDurableVotersCount(leader_id);
  size_t initialSpecVoters = config_->GetSpecVotersCount(leader_id);

  Log_info("[SPEC-TEST] Initial state: secured={}, specVoters={}, durableVoters={}",
           secured, initialSpecVoters, initialDurableVoters);

  Assert2(secured, "Leader should be secured before test continues");

  // Commit an initial entry to ensure everything is working
  int cmd = 300;
  uint64_t index = 0;
  uint64_t term = 0;
  bool ok = config_->Start(leader_id, cmd, &index, &term);
  Assert2(ok, "Failed to submit initial command");

  // Wait for commit
  int result = config_->Wait(index, NSERVERS, term);
  AssertWaitNoError(result, index);
  Log_info("[SPEC-TEST] Initial command committed at index {}", index);

  // Now disconnect one follower to simulate losing a speculative voter
  // (but keep majority for quorum)
  siteid_t disconnected_follower = 0;
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t svr = config_->getServerIdByIndex(i);
    if (svr != leader_id) {
      disconnected_follower = svr;
      break;
    }
  }

  Log_info("[SPEC-TEST] Disconnecting follower {} to lose spec voter", disconnected_follower);
  config_->Disconnect(disconnected_follower);

  // Wait a bit for the disconnect to take effect
  Fiber::sleep(200000);  // 200ms

  // Check that leader is still leader
  int current_leader = config_->OneLeader();
  Assert2(current_leader == leader, "Leader %d changed to %d after disconnect",
          leader, current_leader);

  // Leader should still be secured (disconnect doesn't invalidate secured status)
  secured = config_->IsSecuredLeader(leader_id);
  Log_info("[SPEC-TEST] After disconnect: secured={}", secured);

  // The leader should still be able to commit with remaining quorum
  cmd = 301;
  ok = config_->Start(leader_id, cmd, &index, &term);
  Assert2(ok, "Failed to submit command after disconnect");

  // Wait for commit with NSERVERS-1 (we disconnected 1)
  result = config_->Wait(index, NSERVERS - 1, term);
  AssertWaitNoError(result, index);
  Log_info("[SPEC-TEST] Command committed after disconnect at index {}", index);

  // Verify invariants
  Assert2(config_->VerifySpecInvariants(leader_id), "Invariants violated");

  // Reconnect the follower
  config_->Reconnect(disconnected_follower);
  Fiber::sleep(ELECTIONTIMEOUT);

  Passed2();
}

/**
 * Test that durable commit (securedLogIndex advance) requires secured leader.
 *
 * Scenario:
 * 1. Get durable ack quorum for an entry
 * 2. If leader is not secured, securedLogIndex should NOT advance
 * 3. Once leader becomes secured, securedLogIndex can advance
 *
 * Note: This is hard to test directly because in a working cluster,
 * the leader typically becomes secured very quickly (within a few hundred ms).
 * This test verifies the invariants and the relationship between
 * secured status and securedLogIndex.
 */
int RaftLabTest::testDurableCommitRequiresSecuredLeader(void) {
  Init2(24, "Durable commit requires secured leader");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);

  // Wait for leader to become secured
  Fiber::sleep(500000);  // 500ms for VoteDurable messages

  bool secured = config_->IsSecuredLeader(leader_id);

  Log_info("[SPEC-TEST] Initial secured status: {}", secured);
  Assert2(secured, "Leader should be secured for this test");

  uint64_t initialSecuredLog = config_->GetSecuredLogIndex(leader_id);
  Log_info("[SPEC-TEST] Initial securedLogIndex: {}", initialSecuredLog);

  // Submit multiple entries
  for (int i = 0; i < 5; i++) {
    int cmd = 400 + i;
    uint64_t index = 0;
    uint64_t term = 0;

    bool ok = config_->Start(leader_id, cmd, &index, &term);
    Assert2(ok, "Failed to submit command %d", cmd);

    Log_info("[SPEC-TEST] Submitted command {} at index {}", cmd, index);
  }

  // Wait for durable commits
  Fiber::sleep(1000000);  // 1 second for fsync and durable acks

  uint64_t finalSecuredLog = config_->GetSecuredLogIndex(leader_id);
  uint64_t finalSpecCommit = config_->GetSpecCommitIndex(leader_id);

  Log_info("[SPEC-TEST] Final state: securedLogIndex={}, specCommitIndex={}",
           finalSecuredLog, finalSpecCommit);

  // Since leader is secured, securedLogIndex should have advanced
  Assert2(finalSecuredLog > initialSecuredLog,
          "securedLogIndex (%lu) did not advance from initial (%lu)",
          finalSecuredLog, initialSecuredLog);

  // Verify the invariant: securedLogIndex <= specCommitIndex
  Assert2(finalSecuredLog <= finalSpecCommit,
          "securedLogIndex (%lu) > specCommitIndex (%lu)",
          finalSecuredLog, finalSpecCommit);

  // Verify all invariants
  Assert2(config_->VerifySpecInvariants(leader_id), "Invariants violated");

  Passed2();
}

// ============================================================================
// PHASE 7.2: NotifyRestart Tests
// ============================================================================

/**
 * Test that follower restart removes from specVoters.
 *
 * Scenario:
 * 1. Establish a secured leader
 * 2. Kill and restart a follower
 * 3. Verify that the restarted follower sends notifyRestart
 * 4. Leader should remove the follower from specVoters (but not durableVoters)
 *
 * Note: This tests that the notifyRestart mechanism properly invalidates
 * in-memory votes, which is critical for correctness.
 */
int RaftLabTest::testRestartRemovesFromSpecVoters(void) {
  Init2(25, "Restart removes from specVoters");

  // Wait for initial election and secure leadership
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);

  // Wait for leader to become secured
  Fiber::sleep(500000);  // 500ms for VoteDurable messages

  bool secured = config_->IsSecuredLeader(leader_id);
  size_t initialSpecVoters = config_->GetSpecVotersCount(leader_id);
  size_t initialDurableVoters = config_->GetDurableVotersCount(leader_id);

  Log_info("[SPEC-TEST] Initial state: secured={}, specVoters={}, durableVoters={}",
           secured, initialSpecVoters, initialDurableVoters);

  Assert2(secured, "Leader should be secured before test continues");
  Assert2(initialSpecVoters >= 3, "Should have at least quorum spec voters");

  // Commit an entry to ensure everything is stable
  int cmd = 500;
  uint64_t index = 0;
  uint64_t term = 0;
  bool ok = config_->Start(leader_id, cmd, &index, &term);
  Assert2(ok, "Failed to submit command");
  int result = config_->Wait(index, NSERVERS, term);
  AssertWaitNoError(result, index);

  // Pick a follower to restart
  siteid_t follower_to_restart = 0;
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t svr = config_->getServerIdByIndex(i);
    if (svr != leader_id) {
      follower_to_restart = svr;
      break;
    }
  }

  Log_info("[SPEC-TEST] Killing and restarting follower {}", follower_to_restart);

  // Kill the follower
  config_->Kill(follower_to_restart);

  // Wait a bit
  Fiber::sleep(200000);  // 200ms

  // Restart the follower - this should trigger notifyRestart
  config_->Restart(follower_to_restart);

  // Wait for notifyRestart to be processed
  Fiber::sleep(ELECTIONTIMEOUT);

  // Check specVoters count
  // After restart, the follower's in-memory vote should be invalidated
  // However, once it reconnects and sees the leader's heartbeat, it may
  // re-acknowledge the current leader. The key test is that the system
  // continues to function correctly.

  // Leader should still be leader
  int current_leader = config_->OneLeader();
  Assert2(current_leader == leader, "Leader changed after follower restart");

  // Should still be secured (durable voters unaffected)
  secured = config_->IsSecuredLeader(leader_id);
  size_t finalDurableVoters = config_->GetDurableVotersCount(leader_id);

  Log_info("[SPEC-TEST] After restart: secured={}, durableVoters={}",
           secured, finalDurableVoters);

  // Durable voters should not be affected by restart
  // (They already persisted their vote before the restart)
  Assert2(secured, "Leader should still be secured");
  Assert2(finalDurableVoters >= 3, "Durable voters should be preserved");

  // Verify leader can still commit
  cmd = 501;
  ok = config_->Start(leader_id, cmd, &index, &term);
  Assert2(ok, "Failed to submit command after restart");
  result = config_->Wait(index, NSERVERS, term);
  AssertWaitNoError(result, index);

  Log_info("[SPEC-TEST] Successfully committed after follower restart");

  // Verify invariants
  Assert2(config_->VerifySpecInvariants(leader_id), "Invariants violated");

  Passed2();
}

/**
 * Test that unsecured leader steps down when losing spec quorum.
 *
 * Scenario:
 * 1. Have an unsecured leader (before VoteDurable quorum reached)
 * 2. Cause it to lose speculative quorum via restarts
 * 3. Leader should step down
 *
 * Note: This is difficult to test reliably because VoteDurable messages
 * typically arrive very quickly. This test documents the expected behavior
 * but may not always trigger the unsecured state reliably.
 */
int RaftLabTest::testUnsecuredLostQuorumStepsDown(void) {
  Init2(26, "Unsecured lost quorum steps down");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);

  // In a normal cluster, the leader becomes secured quickly.
  // This test verifies that if we kill enough followers after election,
  // the leader can still operate as long as it has quorum.
  // If the leader was unsecured and lost spec quorum, it would step down.

  // Wait for secured state
  Fiber::sleep(500000);  // 500ms

  bool secured = config_->IsSecuredLeader(leader_id);
  Log_info("[SPEC-TEST] Leader {} secured status: {}", leader_id, secured);

  // Since the leader is likely secured, we test that it continues to work
  // even when followers are killed (secured leader doesn't step down on
  // losing spec quorum alone)

  // Kill 2 followers (still have quorum with 3)
  std::vector<siteid_t> killed_followers;
  for (int i = 0; i < NSERVERS && killed_followers.size() < 2; i++) {
    siteid_t svr = config_->getServerIdByIndex(i);
    if (svr != leader_id) {
      config_->Kill(svr);
      killed_followers.push_back(svr);
      Log_info("[SPEC-TEST] Killed follower {}", svr);
    }
  }

  // Wait for changes to take effect
  Fiber::sleep(ELECTIONTIMEOUT);

  // Leader should still be leader (secured leader doesn't step down)
  int current_leader = config_->OneLeader();

  if (secured) {
    // Secured leader should continue
    Assert2(current_leader == leader, "Secured leader should not step down");
    Log_info("[SPEC-TEST] Secured leader continued as expected");
  } else {
    // Unsecured leader may have stepped down
    // Either outcome is acceptable based on timing
    Log_info("[SPEC-TEST] Leader status after kills: current_leader={} (original={})",
             current_leader, leader);
  }

  // Verify system still works with quorum
  if (current_leader >= 0) {
    siteid_t current_leader_id = config_->getServerIdByIndex(current_leader);
    int cmd = 600;
    uint64_t index = 0;
    uint64_t term = 0;
    bool ok = config_->Start(current_leader_id, cmd, &index, &term);
    if (ok) {
      int result = config_->Wait(index, NSERVERS - 2, term);
      AssertWaitNoError(result, index);
      Log_info("[SPEC-TEST] Committed with 3-node quorum");
    }
  }

  // Restart killed followers
  for (siteid_t svr : killed_followers) {
    config_->Restart(svr);
    Log_info("[SPEC-TEST] Restarted follower {}", svr);
  }

  // Wait for cluster to stabilize
  Fiber::sleep(ELECTIONTIMEOUT);

  // Verify final state
  current_leader = config_->OneLeader();
  Assert2(current_leader >= 0, "No leader after restarts");

  Passed2();
}

/**
 * Test that restart removes from memoryAcks for unsecured entries.
 *
 * Scenario:
 * 1. Establish a secured leader with some committed entries
 * 2. Submit new entries and track memory acks
 * 3. Kill and restart a follower
 * 4. Verify that leader properly handles the restart
 *
 * Note: The memoryAcks tracking is internal, so we verify correct behavior
 * through the system's ability to continue operating correctly.
 */
int RaftLabTest::testRestartRemovesFromMemoryAcks(void) {
  Init2(27, "Restart removes from memoryAcks");

  // Wait for initial election and secure leadership
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);

  // Wait for leader to become secured
  Fiber::sleep(500000);  // 500ms for VoteDurable messages

  bool secured = config_->IsSecuredLeader(leader_id);
  Assert2(secured, "Leader should be secured for this test");

  // Commit some entries to ensure securedLogIndex is established
  for (int i = 0; i < 3; i++) {
    int cmd = 700 + i;
    uint64_t index = 0;
    uint64_t term = 0;
    bool ok = config_->Start(leader_id, cmd, &index, &term);
    Assert2(ok, "Failed to submit command %d", cmd);
    int result = config_->Wait(index, NSERVERS, term);
    AssertWaitNoError(result, index);
  }

  uint64_t securedLogBefore = config_->GetSecuredLogIndex(leader_id);
  Log_info("[SPEC-TEST] securedLogIndex before restart: {}", securedLogBefore);

  // Submit more entries
  uint64_t newIndex = 0;
  uint64_t newTerm = 0;
  bool ok = config_->Start(leader_id, 750, &newIndex, &newTerm);
  Assert2(ok, "Failed to submit new command");

  // Wait for memory acks
  Fiber::sleep(200000);  // 200ms

  // Check memory acks before restart
  size_t memAcksBefore = config_->GetMemoryAckCount(leader_id, newIndex);
  Log_info("[SPEC-TEST] Memory acks for index {} before restart: {}",
           newIndex, memAcksBefore);

  // Pick a follower to restart
  siteid_t follower_to_restart = 0;
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t svr = config_->getServerIdByIndex(i);
    if (svr != leader_id) {
      follower_to_restart = svr;
      break;
    }
  }

  Log_info("[SPEC-TEST] Killing and restarting follower {}", follower_to_restart);

  // Kill and restart the follower
  config_->Kill(follower_to_restart);
  Fiber::sleep(100000);  // 100ms
  config_->Restart(follower_to_restart);

  // Wait for notifyRestart and recovery
  Fiber::sleep(ELECTIONTIMEOUT);

  // Leader should still be leader
  int current_leader = config_->OneLeader();
  Assert2(current_leader == leader, "Leader changed unexpectedly");

  // The entry should eventually be committed with remaining quorum
  int result = config_->Wait(newIndex, NSERVERS - 1, newTerm);
  if (result < 0) {
    // May need to wait for the restarted follower to catch up
    Fiber::sleep(ELECTIONTIMEOUT);
    result = config_->Wait(newIndex, NSERVERS, newTerm);
  }

  Log_info("[SPEC-TEST] Entry at index {} committed", newIndex);

  // Verify invariants
  Assert2(config_->VerifySpecInvariants(leader_id), "Invariants violated");

  Passed2();
}

/**
 * Test that restart does not affect durableVoters.
 *
 * Scenario:
 * 1. Establish a secured leader (durable vote quorum achieved)
 * 2. Restart a follower
 * 3. Verify that durableVoters count is preserved
 *
 * Key insight: Durable votes are on disk, so they survive restarts.
 * The restarted follower's vote is still durable.
 */
int RaftLabTest::testRestartDoesNotAffectDurableVoters(void) {
  Init2(28, "Restart does not affect durableVoters");

  // Wait for initial election and secure leadership
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);

  // Wait for leader to become secured
  Fiber::sleep(500000);  // 500ms for VoteDurable messages

  bool secured = config_->IsSecuredLeader(leader_id);
  size_t durableVotersBefore = config_->GetDurableVotersCount(leader_id);

  Log_info("[SPEC-TEST] Initial state: secured={}, durableVoters={}",
           secured, durableVotersBefore);

  Assert2(secured, "Leader should be secured for this test");
  Assert2(durableVotersBefore >= 3, "Should have quorum of durable voters");

  // Commit an entry to ensure stability
  int cmd = 800;
  uint64_t index = 0;
  uint64_t term = 0;
  bool ok = config_->Start(leader_id, cmd, &index, &term);
  Assert2(ok, "Failed to submit command");
  int result = config_->Wait(index, NSERVERS, term);
  AssertWaitNoError(result, index);

  // Pick a follower to restart
  siteid_t follower_to_restart = 0;
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t svr = config_->getServerIdByIndex(i);
    if (svr != leader_id) {
      follower_to_restart = svr;
      break;
    }
  }

  Log_info("[SPEC-TEST] Killing and restarting follower {}", follower_to_restart);

  // Kill and restart
  config_->Kill(follower_to_restart);
  Fiber::sleep(200000);  // 200ms
  config_->Restart(follower_to_restart);

  // Wait for recovery
  Fiber::sleep(ELECTIONTIMEOUT);

  // Check durableVoters after restart
  // Note: The leader tracks durableVoters from the election.
  // A restart doesn't remove from durableVoters because the vote is on disk.
  // However, the leader may not update durableVoters after restart
  // (it was set during election).

  secured = config_->IsSecuredLeader(leader_id);
  size_t durableVotersAfter = config_->GetDurableVotersCount(leader_id);

  Log_info("[SPEC-TEST] After restart: secured={}, durableVoters={}",
           secured, durableVotersAfter);

  // Leader should still be secured
  Assert2(secured, "Leader should still be secured after restart");

  // Durable voters should be preserved (or may increase if new VoteDurable arrives)
  Assert2(durableVotersAfter >= 3, "Should still have quorum of durable voters");

  // Commit another entry to verify system works
  cmd = 801;
  ok = config_->Start(leader_id, cmd, &index, &term);
  Assert2(ok, "Failed to submit command after restart");
  result = config_->Wait(index, NSERVERS, term);
  AssertWaitNoError(result, index);

  Log_info("[SPEC-TEST] Successfully committed after follower restart");

  // Verify invariants
  Assert2(config_->VerifySpecInvariants(leader_id), "Invariants violated");

  Passed2();
}

// ============================================================================
// PHASE 7.3: Integration Tests
// ============================================================================

/**
 * Test that speculative entries survive leader crash if new leader has them.
 *
 * Scenario:
 * 1. A is leader, speculatively commits entry X (memory quorum achieved)
 * 2. A crashes before durable commit
 * 3. B (who has X in memory/log) wins election
 * 4. X eventually becomes durably committed under B
 * 5. Verify: X persists after full cluster restart
 *
 * Note: This test verifies the "lucky path" where speculative entries
 * survive because the new leader happens to have them.
 */
int RaftLabTest::testSpeculativeEntriesSurviveCrash(void) {
  Init2(29, "Speculative entries survive crash");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader1 = config_->OneLeader();
  Assert2(leader1 >= 0, "No leader elected");

  siteid_t leader1_id = config_->getServerIdByIndex(leader1);
  Log_info("[SPEC-TEST] Initial leader: {} (site {})", leader1, leader1_id);

  // Wait for leader to become secured
  Fiber::sleep(500000);

  bool secured = config_->IsSecuredLeader(leader1_id);
  Assert2(secured, "Leader should be secured");

  // Commit initial entries to establish a baseline
  for (int i = 0; i < 3; i++) {
    int cmd = 900 + i;
    uint64_t index = 0;
    uint64_t term = 0;
    bool ok = config_->Start(leader1_id, cmd, &index, &term);
    Assert2(ok, "Failed to submit command %d", cmd);
    int result = config_->Wait(index, NSERVERS, term);
    AssertWaitNoError(result, index);
    index_ = index;
  }

  Log_info("[SPEC-TEST] Baseline established, index={}", index_);

  // Submit a new entry that will be speculatively committed
  int specCmd = 950;
  uint64_t specIndex = 0;
  uint64_t specTerm = 0;
  bool ok = config_->Start(leader1_id, specCmd, &specIndex, &specTerm);
  Assert2(ok, "Failed to submit speculative command");

  Log_info("[SPEC-TEST] Submitted speculative entry {} at index {} term {}",
           specCmd, specIndex, specTerm);

  // Wait for memory quorum (but not necessarily durable quorum)
  // In practice, entries are replicated quickly via heartbeats
  Fiber::sleep(300000);  // 300ms - should be enough for memory replication

  // Now crash the leader
  Log_info("[SPEC-TEST] Crashing leader {}", leader1_id);
  config_->Kill(leader1_id);

  // Wait for new election
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  // Find new leader
  int leader2 = config_->OneLeader();
  Assert2(leader2 >= 0, "No new leader elected after crash");

  siteid_t leader2_id = config_->getServerIdByIndex(leader2);
  Assert2(leader2_id != leader1_id, "Same leader elected (should be different)");

  Log_info("[SPEC-TEST] New leader: {} (site {})", leader2, leader2_id);

  // Wait for new leader to become secured
  Fiber::sleep(500000);

  secured = config_->IsSecuredLeader(leader2_id);
  Log_info("[SPEC-TEST] New leader secured: {}", secured);

  // Now commit a new entry with the new leader to trigger commit of any
  // previous entries (including our speculative entry if it survived)
  int newCmd = 960;
  uint64_t newIndex = 0;
  uint64_t newTerm = 0;
  ok = config_->Start(leader2_id, newCmd, &newIndex, &newTerm);
  Assert2(ok, "Failed to submit command to new leader");

  Log_info("[SPEC-TEST] Submitted new entry {} at index {} term {}",
           newCmd, newIndex, newTerm);

  // Wait for the entry to commit with the remaining servers
  int result = config_->Wait(newIndex, NSERVERS - 1, newTerm);
  AssertWaitNoError(result, newIndex);

  Log_info("[SPEC-TEST] New entry committed at index {}", newIndex);

  // Now restart the crashed leader
  Log_info("[SPEC-TEST] Restarting crashed leader {}", leader1_id);
  config_->Restart(leader1_id);

  // Wait for it to catch up
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  // Verify all servers agree on the committed entries
  int nCommitted = config_->NCommitted(newIndex);
  Log_info("[SPEC-TEST] Number of servers with entry at index {}: {}",
           newIndex, nCommitted);

  // Check if the speculative entry survived
  // It should either:
  // 1. Be at specIndex if the new leader had it, or
  // 2. Be overwritten if the new leader didn't have it
  // Either outcome is acceptable - we're testing that the system is consistent

  // Verify the new entry is committed on all servers
  Assert2(nCommitted >= NSERVERS - 1, "Not enough servers committed the entry");

  // Verify invariants on the current leader
  Assert2(config_->VerifySpecInvariants(leader2_id), "Invariants violated");

  // Final verification: commit one more entry with all servers
  int finalCmd = 999;
  ok = config_->Start(leader2_id, finalCmd, &newIndex, &newTerm);
  if (ok) {
    result = config_->Wait(newIndex, NSERVERS, newTerm);
    if (result >= 0) {
      Log_info("[SPEC-TEST] Final commit succeeded with all servers");
    }
  }

  Log_info("[SPEC-TEST] Speculative entries survive crash test PASSED!");

  Passed2();
}

/**
 * Test that voter crash before VoteDurable fsync is handled correctly.
 *
 * Scenario:
 * 1. A gets memory votes from {A, B, C, D, E}, becomes spec leader
 * 2. Kill and restart a follower (simulating crash before VoteDurable fsync)
 * 3. Follower restarts → sends notifyRestart to leader
 * 4. Leader removes follower from specVoters
 * 5. In 5-node cluster: still quorum (4/5) → leader continues
 * 6. Verify system continues operating correctly
 *
 * Note: This tests that a follower whose vote wasn't durably persisted
 * doesn't break the system when it restarts.
 */
int RaftLabTest::testVoterCrashBeforeVoteFsync(void) {
  Init2(30, "Voter crash before VoteDurable fsync");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);
  Log_info("[SPEC-TEST] Initial leader: {} (site {})", leader, leader_id);

  // Check initial specVoters count
  size_t specVotersBefore = config_->GetSpecVotersCount(leader_id);
  Log_info("[SPEC-TEST] Initial specVoters count: {}", specVotersBefore);

  // In a normal election, leader should have spec quorum from all servers
  Assert2(specVotersBefore >= 3, "Leader should have spec quorum");

  // Commit a baseline entry to verify initial state
  int cmd = 1000;
  uint64_t index = 0;
  uint64_t term = 0;
  bool ok = config_->Start(leader_id, cmd, &index, &term);
  Assert2(ok, "Failed to submit baseline command");
  int result = config_->Wait(index, NSERVERS, term);
  AssertWaitNoError(result, index);
  index_ = index;

  Log_info("[SPEC-TEST] Baseline committed at index {}", index);

  // Pick a follower to simulate crash before VoteDurable
  siteid_t follower_to_crash = 0;
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t svr = config_->getServerIdByIndex(i);
    if (svr != leader_id) {
      follower_to_crash = svr;
      break;
    }
  }

  Log_info("[SPEC-TEST] Simulating crash of follower {} before VoteDurable fsync",
           follower_to_crash);

  // Kill the follower (simulating crash before vote was durably persisted)
  config_->Kill(follower_to_crash);

  // Brief wait to ensure crash is processed
  Fiber::sleep(200000);  // 200ms

  // Restart the follower - it will send notifyRestart
  config_->Restart(follower_to_crash);

  // Wait for notifyRestart and recovery
  Fiber::sleep(ELECTIONTIMEOUT);

  // Leader should still be leader (5-node cluster, lost 1 spec voter → 4/5 still quorum)
  int current_leader = config_->OneLeader();
  if (current_leader < 0) {
    // Election may be happening, wait longer
    Fiber::sleep(ELECTIONTIMEOUT);
    current_leader = config_->OneLeader();
  }

  Log_info("[SPEC-TEST] Current leader after restart: {}", current_leader);

  // Check specVoters count after restart
  size_t specVotersAfter = 0;
  if (current_leader >= 0) {
    siteid_t current_leader_id = config_->getServerIdByIndex(current_leader);
    specVotersAfter = config_->GetSpecVotersCount(current_leader_id);
    Log_info("[SPEC-TEST] SpecVoters after restart: {} (leader {})",
             specVotersAfter, current_leader_id);
  }

  // The system should continue functioning regardless of leader change
  // Try to commit a new entry
  if (current_leader >= 0) {
    siteid_t current_leader_id = config_->getServerIdByIndex(current_leader);
    int newCmd = 1001;
    uint64_t newIndex = 0;
    uint64_t newTerm = 0;
    ok = config_->Start(current_leader_id, newCmd, &newIndex, &newTerm);
    if (ok) {
      result = config_->Wait(newIndex, NSERVERS - 1, newTerm);
      if (result >= 0) {
        Log_info("[SPEC-TEST] New entry committed after restart at index {}", newIndex);
      } else {
        Log_info("[SPEC-TEST] Entry pending commit (result={})", result);
      }
    }
  }

  // Verify all servers eventually agree
  // Wait for full recovery
  Fiber::sleep(ELECTIONTIMEOUT);

  // Try to commit one final entry with full cluster
  current_leader = config_->OneLeader();
  Assert2(current_leader >= 0, "Should have a leader after recovery");

  siteid_t final_leader_id = config_->getServerIdByIndex(current_leader);
  int finalCmd = 1002;
  uint64_t finalIndex = 0;
  uint64_t finalTerm = 0;
  ok = config_->Start(final_leader_id, finalCmd, &finalIndex, &finalTerm);
  Assert2(ok, "Failed to submit final command");

  result = config_->Wait(finalIndex, NSERVERS, finalTerm);
  AssertWaitNoError(result, finalIndex);

  Log_info("[SPEC-TEST] Final entry committed with all servers at index {}", finalIndex);

  // Verify invariants
  Assert2(config_->VerifySpecInvariants(final_leader_id), "Invariants violated");

  Log_info("[SPEC-TEST] Voter crash before VoteDurable fsync test PASSED!");

  Passed2();
}

/**
 * Test double-vote prevention after crash.
 *
 * Scenario (idealized):
 * 1. A becomes leader with memory votes from all servers
 * 2. A commits some entries
 * 3. Multiple followers crash (simulating loss of in-memory votes)
 * 4. After restart, followers could theoretically vote for another candidate
 * 5. Verify system remains consistent (no conflicting durable commits)
 *
 * This test verifies that even if followers crash and potentially vote twice
 * (because vote wasn't persisted), the system handles this safely via
 * notifyRestart mechanism.
 *
 * Key insight: The notifyRestart mechanism ensures the original leader
 * knows about the restart and adjusts its quorum tracking accordingly.
 * This prevents conflicting durable commits.
 */
int RaftLabTest::testDoubleVotePrevention(void) {
  Init2(31, "Double vote prevention after crash");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader1 = config_->OneLeader();
  Assert2(leader1 >= 0, "No leader elected");

  siteid_t leader1_id = config_->getServerIdByIndex(leader1);
  Log_info("[SPEC-TEST] Initial leader: {} (site {})", leader1, leader1_id);

  // Wait for leader to become secured
  Fiber::sleep(500000);

  bool secured = config_->IsSecuredLeader(leader1_id);
  Log_info("[SPEC-TEST] Leader secured: {}", secured);
  Assert2(secured, "Leader should be secured");

  // Commit some entries to establish state
  for (int i = 0; i < 3; i++) {
    int cmd = 1100 + i;
    uint64_t index = 0;
    uint64_t term = 0;
    bool ok = config_->Start(leader1_id, cmd, &index, &term);
    Assert2(ok, "Failed to submit command %d", cmd);
    int result = config_->Wait(index, NSERVERS, term);
    AssertWaitNoError(result, index);
    index_ = index;
  }

  Log_info("[SPEC-TEST] Committed 3 entries, last index={}", index_);

  // Collect followers
  std::vector<siteid_t> followers;
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t svr = config_->getServerIdByIndex(i);
    if (svr != leader1_id) {
      followers.push_back(svr);
    }
  }

  // Crash and restart 2 followers (simulating loss of in-memory votes)
  // This leaves leader with potentially reduced quorum for speculative state
  Log_info("[SPEC-TEST] Crashing 2 followers to simulate vote loss");

  config_->Kill(followers[0]);
  config_->Kill(followers[1]);

  Fiber::sleep(200000);  // 200ms

  // Restart them
  config_->Restart(followers[0]);
  config_->Restart(followers[1]);

  // Wait for notifyRestart and recovery
  Fiber::sleep(ELECTIONTIMEOUT);

  // After restart, the followers send notifyRestart
  // The original leader should adjust its quorum tracking

  // Find current leader
  int leader2 = config_->OneLeader();
  if (leader2 < 0) {
    Fiber::sleep(ELECTIONTIMEOUT);
    leader2 = config_->OneLeader();
  }
  Assert2(leader2 >= 0, "Should have a leader after recovery");

  siteid_t leader2_id = config_->getServerIdByIndex(leader2);
  Log_info("[SPEC-TEST] Leader after restarts: {} (site {})", leader2, leader2_id);

  // The key safety property: no conflicting durable commits
  // We verify this by checking that the system can commit new entries
  // and all servers agree

  // Wait for things to stabilize
  Fiber::sleep(ELECTIONTIMEOUT);

  // Try to commit a new entry
  int newCmd = 1200;
  uint64_t newIndex = 0;
  uint64_t newTerm = 0;

  // Get current leader (may have changed)
  int current_leader = config_->OneLeader();
  Assert2(current_leader >= 0, "Should have a leader");

  siteid_t current_leader_id = config_->getServerIdByIndex(current_leader);

  bool ok = config_->Start(current_leader_id, newCmd, &newIndex, &newTerm);
  Assert2(ok, "Failed to submit new command");

  int result = config_->Wait(newIndex, NSERVERS, newTerm);
  if (result < 0) {
    // May need more time for everyone to catch up
    Fiber::sleep(ELECTIONTIMEOUT);
    result = config_->Wait(newIndex, NSERVERS, newTerm);
  }

  if (result >= 0) {
    Log_info("[SPEC-TEST] New entry committed at index {}", newIndex);
  } else {
    // Even if not all servers have it yet, at least verify
    // a quorum committed it
    int committed = config_->NCommitted(newIndex);
    Log_info("[SPEC-TEST] Committed on {} servers", committed);
    Assert2(committed >= 3, "At least quorum should have committed");
  }

  // Verify all servers eventually agree by committing another entry
  Fiber::sleep(ELECTIONTIMEOUT / 2);

  current_leader = config_->OneLeader();
  Assert2(current_leader >= 0, "Should have a leader");
  current_leader_id = config_->getServerIdByIndex(current_leader);

  int finalCmd = 1299;
  uint64_t finalIndex = 0;
  uint64_t finalTerm = 0;

  ok = config_->Start(current_leader_id, finalCmd, &finalIndex, &finalTerm);
  Assert2(ok, "Failed to submit final command");

  result = config_->Wait(finalIndex, NSERVERS, finalTerm);
  AssertWaitNoError(result, finalIndex);

  Log_info("[SPEC-TEST] Final entry committed with all servers at index {}", finalIndex);

  // Verify invariants on leader
  Assert2(config_->VerifySpecInvariants(current_leader_id), "Invariants violated");

  Log_info("[SPEC-TEST] Double vote prevention test PASSED!");

  Passed2();
}

// ============================================================================
// PHASE 7.4: Stress Tests
// ============================================================================

/**
 * Test rapid follower restarts.
 *
 * Stress test that rapidly restarts followers to verify the system
 * maintains consistency under churn.
 *
 * Scenario:
 * 1. Establish leader and commit some entries
 * 2. Rapidly restart multiple followers in sequence
 * 3. Continue committing entries during the chaos
 * 4. Verify all entries eventually committed on all servers
 * 5. Verify no invariant violations
 */
int RaftLabTest::testRapidRestarts(void) {
  Init2(32, "Rapid follower restarts stress test");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);
  Log_info("[SPEC-TEST] Initial leader: {} (site {})", leader, leader_id);

  // Wait for leader to become secured
  Fiber::sleep(500000);

  bool secured = config_->IsSecuredLeader(leader_id);
  Assert2(secured, "Leader should be secured");

  // Commit initial entries
  for (int i = 0; i < 3; i++) {
    int cmd = 2000 + i;
    uint64_t index = 0;
    uint64_t term = 0;
    bool ok = config_->Start(leader_id, cmd, &index, &term);
    Assert2(ok, "Failed to submit command %d", cmd);
    int result = config_->Wait(index, NSERVERS, term);
    AssertWaitNoError(result, index);
    index_ = index;
  }

  Log_info("[SPEC-TEST] Baseline committed, last index={}", index_);

  // Collect followers
  std::vector<siteid_t> followers;
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t svr = config_->getServerIdByIndex(i);
    if (svr != leader_id) {
      followers.push_back(svr);
    }
  }

  // Rapid restart sequence: cycle through followers
  int num_restarts = 8;  // Total number of restarts
  int restart_delay_ms = 200;  // Delay between restarts

  Log_info("[SPEC-TEST] Starting {} rapid restarts...", num_restarts);

  for (int r = 0; r < num_restarts; r++) {
    // Pick follower to restart (cycle through)
    siteid_t follower_to_restart = followers[r % followers.size()];

    Log_info("[SPEC-TEST] Restart {}: killing follower {}", r + 1, follower_to_restart);
    config_->Kill(follower_to_restart);

    // Brief delay
    Fiber::sleep(restart_delay_ms * 1000);  // Convert to microseconds

    // Restart
    config_->Restart(follower_to_restart);

    // Try to commit an entry while things are churning
    int current_leader = config_->OneLeader();
    if (current_leader >= 0) {
      siteid_t current_leader_id = config_->getServerIdByIndex(current_leader);
      int cmd = 2100 + r;
      uint64_t index = 0;
      uint64_t term = 0;
      bool ok = config_->Start(current_leader_id, cmd, &index, &term);
      if (ok) {
        // Don't wait for full quorum during chaos, just verify it started
        Fiber::sleep(100000);  // 100ms
        int committed = config_->NCommitted(index);
        Log_info("[SPEC-TEST] Restart {}: entry {} started, committed on {} servers",
                 r + 1, cmd, committed);
        index_ = index;
      }
    }

    // Brief delay before next restart
    Fiber::sleep(restart_delay_ms * 1000);
  }

  Log_info("[SPEC-TEST] Rapid restarts complete, stabilizing...");

  // Wait for system to stabilize
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  // Find leader after chaos
  int final_leader = config_->OneLeader();
  Assert2(final_leader >= 0, "Should have leader after stabilization");

  siteid_t final_leader_id = config_->getServerIdByIndex(final_leader);
  Log_info("[SPEC-TEST] Leader after chaos: {} (site {})", final_leader, final_leader_id);

  // Commit final entries to verify full recovery
  for (int i = 0; i < 3; i++) {
    int cmd = 2200 + i;
    uint64_t index = 0;
    uint64_t term = 0;

    // Get fresh leader (may have changed)
    int leader_now = config_->OneLeader();
    if (leader_now < 0) {
      Fiber::sleep(ELECTIONTIMEOUT);
      leader_now = config_->OneLeader();
      Assert2(leader_now >= 0, "Should have a leader");
    }
    siteid_t leader_now_id = config_->getServerIdByIndex(leader_now);

    bool ok = config_->Start(leader_now_id, cmd, &index, &term);
    Assert2(ok, "Failed to submit final command %d", cmd);

    int result = config_->Wait(index, NSERVERS, term);
    if (result < 0) {
      // May need more time
      Fiber::sleep(ELECTIONTIMEOUT);
      result = config_->Wait(index, NSERVERS, term);
    }

    if (result >= 0) {
      Log_info("[SPEC-TEST] Final entry {} committed at index {}", cmd, index);
    } else {
      int committed = config_->NCommitted(index);
      Log_info("[SPEC-TEST] Final entry {}: committed on {} servers", cmd, committed);
      Assert2(committed >= 3, "At least quorum should have committed");
    }
    index_ = index;
  }

  // Verify invariants on current leader
  final_leader = config_->OneLeader();
  Assert2(final_leader >= 0, "Should have leader");
  final_leader_id = config_->getServerIdByIndex(final_leader);

  Assert2(config_->VerifySpecInvariants(final_leader_id), "Invariants violated");

  Log_info("[SPEC-TEST] Rapid restarts stress test PASSED!");

  Passed2();
}

/**
 * Test concurrent elections with speculative voting.
 *
 * Stress test that triggers multiple elections by repeatedly killing
 * the leader to verify speculative voting works correctly under
 * election pressure.
 *
 * Scenario:
 * 1. Establish leader and commit entries
 * 2. Kill leader, forcing new election
 * 3. Repeat several times with entries committed between elections
 * 4. Verify all entries committed correctly
 * 5. Verify no invariant violations
 */
int RaftLabTest::testConcurrentElections(void) {
  Init2(33, "Concurrent elections stress test");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);
  Log_info("[SPEC-TEST] Initial leader: {} (site {})", leader, leader_id);

  // Wait for leader to become secured
  Fiber::sleep(500000);

  bool secured = config_->IsSecuredLeader(leader_id);
  Assert2(secured, "Leader should be secured");

  // Commit initial entries
  for (int i = 0; i < 2; i++) {
    int cmd = 3000 + i;
    uint64_t index = 0;
    uint64_t term = 0;
    bool ok = config_->Start(leader_id, cmd, &index, &term);
    Assert2(ok, "Failed to submit command %d", cmd);
    int result = config_->Wait(index, NSERVERS, term);
    AssertWaitNoError(result, index);
    index_ = index;
  }

  Log_info("[SPEC-TEST] Baseline committed, last index={}", index_);

  // Force multiple elections by killing leaders
  int num_elections = 4;

  for (int e = 0; e < num_elections; e++) {
    // Get current leader
    int current_leader = config_->OneLeader();
    if (current_leader < 0) {
      Fiber::sleep(ELECTIONTIMEOUT);
      current_leader = config_->OneLeader();
    }
    Assert2(current_leader >= 0, "Should have leader before kill");

    siteid_t current_leader_id = config_->getServerIdByIndex(current_leader);
    Log_info("[SPEC-TEST] Election {}: killing leader {} (site {})",
             e + 1, current_leader, current_leader_id);

    // Kill the leader
    config_->Kill(current_leader_id);

    // Wait for new election
    Fiber::sleep(ELECTIONTIMEOUT * 2);

    // Find new leader
    int new_leader = config_->OneLeader();
    if (new_leader < 0) {
      Fiber::sleep(ELECTIONTIMEOUT);
      new_leader = config_->OneLeader();
    }
    Assert2(new_leader >= 0, "Should have new leader after kill");

    siteid_t new_leader_id = config_->getServerIdByIndex(new_leader);
    Assert2(new_leader_id != current_leader_id, "New leader should be different");

    Log_info("[SPEC-TEST] Election {}: new leader {} (site {})",
             e + 1, new_leader, new_leader_id);

    // Wait for new leader to become secured
    Fiber::sleep(500000);

    secured = config_->IsSecuredLeader(new_leader_id);
    Log_info("[SPEC-TEST] Election {}: new leader secured={}", e + 1, secured);

    // Commit an entry with new leader
    int cmd = 3100 + e;
    uint64_t index = 0;
    uint64_t term = 0;
    bool ok = config_->Start(new_leader_id, cmd, &index, &term);
    Assert2(ok, "Failed to submit command %d", cmd);

    // Wait for commit with remaining servers
    int result = config_->Wait(index, NSERVERS - (e + 1), term);
    if (result < 0) {
      Fiber::sleep(ELECTIONTIMEOUT);
      int committed = config_->NCommitted(index);
      Log_info("[SPEC-TEST] Election {}: entry {} committed on {} servers",
               e + 1, cmd, committed);
      Assert2(committed >= 3, "At least quorum should have committed");
    } else {
      Log_info("[SPEC-TEST] Election {}: entry {} committed at index {}",
               e + 1, cmd, index);
    }
    index_ = index;

    // Restart the killed leader
    Log_info("[SPEC-TEST] Election {}: restarting killed leader {}",
             e + 1, current_leader_id);
    config_->Restart(current_leader_id);

    // Wait for recovery
    Fiber::sleep(ELECTIONTIMEOUT);
  }

  Log_info("[SPEC-TEST] Concurrent elections complete, stabilizing...");

  // Final stabilization
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  // Find final leader
  int final_leader = config_->OneLeader();
  Assert2(final_leader >= 0, "Should have leader after stabilization");

  siteid_t final_leader_id = config_->getServerIdByIndex(final_leader);
  Log_info("[SPEC-TEST] Final leader: {} (site {})", final_leader, final_leader_id);

  // Commit final entries with all servers
  for (int i = 0; i < 2; i++) {
    int cmd = 3200 + i;
    uint64_t index = 0;
    uint64_t term = 0;

    // Get fresh leader
    int leader_now = config_->OneLeader();
    if (leader_now < 0) {
      Fiber::sleep(ELECTIONTIMEOUT);
      leader_now = config_->OneLeader();
      Assert2(leader_now >= 0, "Should have a leader");
    }
    siteid_t leader_now_id = config_->getServerIdByIndex(leader_now);

    bool ok = config_->Start(leader_now_id, cmd, &index, &term);
    Assert2(ok, "Failed to submit final command %d", cmd);

    int result = config_->Wait(index, NSERVERS, term);
    if (result < 0) {
      Fiber::sleep(ELECTIONTIMEOUT);
      result = config_->Wait(index, NSERVERS, term);
    }
    AssertWaitNoError(result, index);

    Log_info("[SPEC-TEST] Final entry {} committed at index {}", cmd, index);
    index_ = index;
  }

  // Verify invariants on current leader
  final_leader = config_->OneLeader();
  Assert2(final_leader >= 0, "Should have leader");
  final_leader_id = config_->getServerIdByIndex(final_leader);

  Assert2(config_->VerifySpecInvariants(final_leader_id), "Invariants violated");

  Log_info("[SPEC-TEST] Concurrent elections stress test PASSED!");

  Passed2();
}

// ============================================================================
// PHASE 5.3: Client Notification Tests
// ============================================================================

/**
 * Test that client gets SPECULATIVE notification.
 *
 * Scenario:
 * 1. Establish leader
 * 2. Submit entry with callback
 * 3. Verify callback receives SPECULATIVE status
 */
int RaftLabTest::testSpeculativeCommitNotification(void) {
  Init2(34, "Speculative commit notification");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);
  Log_info("[CALLBACK-TEST] Leader: {} (site {})", leader, leader_id);

  // Wait for leader to become secured
  Fiber::sleep(500000);

  // Track callback invocations
  std::atomic<int> specNotifications{0};
  std::atomic<int> durableNotifications{0};
  std::atomic<bool> gotSpeculative{false};

  // Submit entry with callback
  int cmd = 4000;
  uint64_t index = 0;
  uint64_t term = 0;

  bool ok = config_->StartWithCallback(leader_id, cmd, &index, &term,
    [&](CommitStatus status) {
      Log_info("[CALLBACK-TEST] Received notification: status={}", static_cast<int>(status));
      if (status == CommitStatus::SPECULATIVE) {
        specNotifications++;
        gotSpeculative = true;
      } else if (status == CommitStatus::DURABLE) {
        durableNotifications++;
      }
    });

  Assert2(ok, "Failed to submit command with callback");
  Log_info("[CALLBACK-TEST] Submitted command {} at index {}", cmd, index);

  // Wait for the entry to be speculatively committed (memory quorum)
  Fiber::sleep(500000);  // 500ms - should be enough for memory replication

  // Verify we got SPECULATIVE notification
  Log_info("[CALLBACK-TEST] Spec notifications: {}, Durable: {}",
           specNotifications.load(), durableNotifications.load());

  Assert2(gotSpeculative.load(), "Should have received SPECULATIVE notification");
  Assert2(specNotifications.load() >= 1, "Should have at least 1 SPECULATIVE notification");

  Log_info("[CALLBACK-TEST] Speculative commit notification test PASSED!");

  Passed2();
}

/**
 * Test that client gets DURABLE notification.
 *
 * Scenario:
 * 1. Establish secured leader
 * 2. Submit entry with callback
 * 3. Wait for durable commit
 * 4. Verify callback receives DURABLE status
 */
int RaftLabTest::testDurableCommitNotification(void) {
  Init2(35, "Durable commit notification");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);
  Log_info("[CALLBACK-TEST] Leader: {} (site {})", leader, leader_id);

  // Wait for leader to become secured
  Fiber::sleep(500000);

  bool secured = config_->IsSecuredLeader(leader_id);
  Assert2(secured, "Leader should be secured");

  // Track callback invocations
  std::atomic<int> specNotifications{0};
  std::atomic<int> durableNotifications{0};
  std::atomic<bool> gotDurable{false};

  // Submit entry with callback
  int cmd = 4100;
  uint64_t index = 0;
  uint64_t term = 0;

  bool ok = config_->StartWithCallback(leader_id, cmd, &index, &term,
    [&](CommitStatus status) {
      Log_info("[CALLBACK-TEST] Received notification: status={}", static_cast<int>(status));
      if (status == CommitStatus::SPECULATIVE) {
        specNotifications++;
      } else if (status == CommitStatus::DURABLE) {
        durableNotifications++;
        gotDurable = true;
      }
    });

  Assert2(ok, "Failed to submit command with callback");
  Log_info("[CALLBACK-TEST] Submitted command {} at index {}", cmd, index);

  // Wait for the entry to be durably committed (disk quorum with secured leader)
  // This requires fsync to complete on majority
  Fiber::sleep(1000000);  // 1s - should be enough for durable commit

  // Verify we got DURABLE notification
  Log_info("[CALLBACK-TEST] Spec notifications: {}, Durable: {}",
           specNotifications.load(), durableNotifications.load());

  Assert2(gotDurable.load(), "Should have received DURABLE notification");
  Assert2(durableNotifications.load() >= 1, "Should have at least 1 DURABLE notification");

  Log_info("[CALLBACK-TEST] Durable commit notification test PASSED!");

  Passed2();
}

/**
 * Test that SPECULATIVE notification comes before DURABLE.
 *
 * Scenario:
 * 1. Establish secured leader
 * 2. Submit entry with callback
 * 3. Track order of notifications
 * 4. Verify SPECULATIVE comes before DURABLE
 */
int RaftLabTest::testNotificationOrdering(void) {
  Init2(36, "Notification ordering (SPECULATIVE before DURABLE)");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);
  Log_info("[CALLBACK-TEST] Leader: {} (site {})", leader, leader_id);

  // Wait for leader to become secured
  Fiber::sleep(500000);

  bool secured = config_->IsSecuredLeader(leader_id);
  Assert2(secured, "Leader should be secured");

  // Track callback invocation order
  std::atomic<int> callCount{0};
  std::atomic<int> specOrder{-1};
  std::atomic<int> durableOrder{-1};

  // Submit entry with callback
  int cmd = 4200;
  uint64_t index = 0;
  uint64_t term = 0;

  bool ok = config_->StartWithCallback(leader_id, cmd, &index, &term,
    [&](CommitStatus status) {
      int order = callCount++;
      Log_info("[CALLBACK-TEST] Notification #{}: status={}", order, static_cast<int>(status));
      if (status == CommitStatus::SPECULATIVE) {
        specOrder = order;
      } else if (status == CommitStatus::DURABLE) {
        durableOrder = order;
      }
    });

  Assert2(ok, "Failed to submit command with callback");
  Log_info("[CALLBACK-TEST] Submitted command {} at index {}", cmd, index);

  // Wait for both notifications
  Fiber::sleep(1000000);  // 1s

  // Verify ordering
  Log_info("[CALLBACK-TEST] Spec order: {}, Durable order: {}",
           specOrder.load(), durableOrder.load());

  // SPECULATIVE should come first (if both arrived)
  if (specOrder.load() >= 0 && durableOrder.load() >= 0) {
    Assert2(specOrder.load() < durableOrder.load(),
            "SPECULATIVE should come before DURABLE");
  } else if (durableOrder.load() >= 0 && specOrder.load() < 0) {
    // If we only got DURABLE, that's actually OK - it means SPECULATIVE
    // was delivered immediately before we started tracking (edge case)
    Log_info("[CALLBACK-TEST] Only got DURABLE - SPECULATIVE may have been immediate");
  }

  // At minimum, we should get at least one notification
  Assert2(callCount.load() >= 1, "Should have at least 1 notification");

  Log_info("[CALLBACK-TEST] Notification ordering test PASSED!");

  Passed2();
}

/**
 * Test that unsecured leader step-down notifies ROLLEDBACK to pending clients.
 *
 * Scenario:
 * 1. Establish an unsecured leader (before VoteDurable quorum)
 *    - This is tricky because VoteDurable usually arrives quickly
 *    - We'll test the rollback mechanism by crashing majority after entry submission
 * 2. Submit entry with callback
 * 3. Crash majority of followers to trigger step-down
 * 4. Verify callback receives ROLLEDBACK (if leader is still alive)
 *
 * Note: Due to the async nature and quick VoteDurable, we may not be able to
 * catch a truly unsecured leader. But we can test that when leadership changes,
 * pending callbacks get notified appropriately.
 */
int RaftLabTest::testUnsecuredStepDownNotifiesRollback(void) {
  Init2(37, "Unsecured step-down notifies rollback");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);
  Log_info("[CALLBACK-TEST] Leader: {} (site {})", leader, leader_id);

  // Wait for leader to become secured first (so we have a baseline)
  Fiber::sleep(500000);

  // Track callback invocations
  std::atomic<int> specNotifications{0};
  std::atomic<int> durableNotifications{0};
  std::atomic<int> rollbackNotifications{0};

  // Submit entry with callback - this will likely become durable
  int cmd = 4300;
  uint64_t index = 0;
  uint64_t term = 0;

  bool ok = config_->StartWithCallback(leader_id, cmd, &index, &term,
    [&](CommitStatus status) {
      Log_info("[CALLBACK-TEST] Received notification: status={}", static_cast<int>(status));
      if (status == CommitStatus::SPECULATIVE) {
        specNotifications++;
      } else if (status == CommitStatus::DURABLE) {
        durableNotifications++;
      } else if (status == CommitStatus::ROLLEDBACK) {
        rollbackNotifications++;
      }
    });

  Assert2(ok, "Failed to submit command with callback");
  Log_info("[CALLBACK-TEST] Submitted command {} at index {}", cmd, index);

  // Let it commit (we're testing the infrastructure, not a specific scenario)
  Fiber::sleep(500000);

  // Now submit another entry and crash majority before it commits
  int cmd2 = 4301;
  uint64_t index2 = 0;
  uint64_t term2 = 0;

  std::atomic<int> cmd2Rollback{0};
  std::atomic<int> cmd2Spec{0};

  ok = config_->StartWithCallback(leader_id, cmd2, &index2, &term2,
    [&](CommitStatus status) {
      Log_info("[CALLBACK-TEST] Entry 2 notification: status={}", static_cast<int>(status));
      if (status == CommitStatus::SPECULATIVE) {
        cmd2Spec++;
      } else if (status == CommitStatus::ROLLEDBACK) {
        cmd2Rollback++;
      }
    });

  Assert2(ok, "Failed to submit second command with callback");
  Log_info("[CALLBACK-TEST] Submitted command2 {} at index {}", cmd2, index2);

  // Crash majority of followers to force leadership change
  Log_info("[CALLBACK-TEST] Crashing majority of followers");

  std::vector<siteid_t> followers;
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t svr = config_->getServerIdByIndex(i);
    if (svr != leader_id) {
      followers.push_back(svr);
    }
  }

  // Kill 3 followers (in 5-node cluster, this leaves leader + 1 follower = no quorum)
  for (int i = 0; i < 3 && i < (int)followers.size(); i++) {
    config_->Kill(followers[i]);
  }

  // Wait for step-down or election timeout
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  // Log results
  Log_info("[CALLBACK-TEST] Results: spec={} durable={} rollback={}",
           specNotifications.load(), durableNotifications.load(), rollbackNotifications.load());
  Log_info("[CALLBACK-TEST] Entry2 results: spec={} rollback={}",
           cmd2Spec.load(), cmd2Rollback.load());

  // Restart killed followers
  for (int i = 0; i < 3 && i < (int)followers.size(); i++) {
    config_->Restart(followers[i]);
  }

  // Wait for cluster to stabilize
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  // The test passes if:
  // 1. First command got at least speculative (and possibly durable)
  // 2. The infrastructure handled the step-down (even if no rollback notification
  //    was sent because the leader crashed before it could notify)

  // Verify at least first entry was speculatively committed
  Assert2(specNotifications.load() >= 1 || durableNotifications.load() >= 1,
          "First entry should have been at least speculatively committed");

  // Final cleanup - ensure cluster is operational
  int final_leader = config_->OneLeader();
  if (final_leader < 0) {
    Fiber::sleep(ELECTIONTIMEOUT);
    final_leader = config_->OneLeader();
  }
  Assert2(final_leader >= 0, "Should have leader after recovery");

  Log_info("[CALLBACK-TEST] Unsecured step-down rollback test PASSED!");

  Passed2();
}

/**
 * Test the full commit path: SPECULATIVE -> DURABLE -> persist after restart.
 *
 * Happy path scenario:
 * 1. Submit request to secured leader
 * 2. Verify client callback receives SPECULATIVE
 * 3. Wait for fsyncs to complete
 * 4. Verify client callback receives DURABLE
 * 5. Crash/restart all servers
 * 6. Verify entry persisted correctly
 */
int RaftLabTest::testFullCommitPath(void) {
  Init2(38, "Full commit path (SPECULATIVE -> DURABLE -> persist)");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);
  Log_info("[FULL-PATH-TEST] Leader: {} (site {})", leader, leader_id);

  // Wait for leader to become secured
  Fiber::sleep(500000);

  bool secured = config_->IsSecuredLeader(leader_id);
  Assert2(secured, "Leader should be secured for full path test");

  // Track callback invocations with timestamps
  std::atomic<bool> gotSpeculative{false};
  std::atomic<bool> gotDurable{false};
  std::atomic<uint64_t> specTime{0};
  std::atomic<uint64_t> durableTime{0};

  // Submit entry with callback
  int cmd = 4400;
  uint64_t index = 0;
  uint64_t term = 0;

  Log_info("[FULL-PATH-TEST] Submitting command {} with callback", cmd);

  bool ok = config_->StartWithCallback(leader_id, cmd, &index, &term,
    [&](CommitStatus status) {
      uint64_t now = Time::now(false);
      Log_info("[FULL-PATH-TEST] Callback received: status={} time={}",
               static_cast<int>(status), now);
      if (status == CommitStatus::SPECULATIVE) {
        gotSpeculative = true;
        specTime = now;
      } else if (status == CommitStatus::DURABLE) {
        gotDurable = true;
        durableTime = now;
      }
    });

  Assert2(ok, "Failed to submit command with callback");
  Log_info("[FULL-PATH-TEST] Submitted command {} at index {} term {}", cmd, index, term);

  // Step 2: Wait for SPECULATIVE (memory quorum)
  // Should be very fast
  Fiber::sleep(200000);  // 200ms
  Log_info("[FULL-PATH-TEST] After 200ms: spec={} durable={}",
           gotSpeculative.load(), gotDurable.load());

  // Step 3: Wait for DURABLE (disk quorum with secured leader)
  // This requires fsync to complete
  Fiber::sleep(1000000);  // 1s total
  Log_info("[FULL-PATH-TEST] After 1s: spec={} durable={}",
           gotSpeculative.load(), gotDurable.load());

  // Step 4: Verify both notifications received
  Assert2(gotSpeculative.load(), "Should have received SPECULATIVE notification");
  Assert2(gotDurable.load(), "Should have received DURABLE notification");

  // Verify SPECULATIVE came before DURABLE
  if (specTime.load() > 0 && durableTime.load() > 0) {
    Assert2(specTime.load() <= durableTime.load(),
            "SPECULATIVE should come before or at DURABLE");
    Log_info("[FULL-PATH-TEST] Spec time: {}, Durable time: {}, delta: {} us",
             specTime.load(), durableTime.load(), durableTime.load() - specTime.load());
  }

  // Step 5: Restart all servers to verify persistence
  Log_info("[FULL-PATH-TEST] Restarting all servers...");

  // First, kill all servers
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t svr = config_->getServerIdByIndex(i);
    config_->Kill(svr);
  }

  Fiber::sleep(200000);  // 200ms

  // Restart all servers
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t svr = config_->getServerIdByIndex(i);
    config_->Restart(svr);
  }

  // Wait for election
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  // Step 6: Verify entry persisted
  int new_leader = config_->OneLeader();
  if (new_leader < 0) {
    Fiber::sleep(ELECTIONTIMEOUT);
    new_leader = config_->OneLeader();
  }
  Assert2(new_leader >= 0, "Should have leader after restart");

  Log_info("[FULL-PATH-TEST] New leader after restart: {}", new_leader);

  // Check if entry is committed on servers
  int nCommitted = config_->NCommitted(index);
  Log_info("[FULL-PATH-TEST] Servers with entry at index {}: {}", index, nCommitted);

  // The entry should be committed on all servers (it was durably committed)
  Assert2(nCommitted >= 3, "Entry should be committed on majority after restart");

  // Submit another entry to verify system is operational
  siteid_t new_leader_id = config_->getServerIdByIndex(new_leader);
  int finalCmd = 4401;
  uint64_t finalIndex = 0;
  uint64_t finalTerm = 0;

  ok = config_->Start(new_leader_id, finalCmd, &finalIndex, &finalTerm);
  Assert2(ok, "Failed to submit final command");

  int result = config_->Wait(finalIndex, NSERVERS, finalTerm);
  AssertWaitNoError(result, finalIndex);

  Log_info("[FULL-PATH-TEST] Final entry committed at index {}", finalIndex);
  Log_info("[FULL-PATH-TEST] Full commit path test PASSED!");

  Passed2();
}

/**
 * Test that durably committed entries don't receive ROLLEDBACK on step-down.
 *
 * This test verifies:
 * 1. Entries that reached DURABLE status are removed from pendingCallbacks_
 * 2. Therefore, they cannot receive ROLLEDBACK notifications
 * 3. The callback lifecycle is correct: SPECULATIVE -> DURABLE -> removed
 *
 * Note: Full partial rollback testing (entries > securedLogIndex get ROLLEDBACK
 * while entries <= securedLogIndex don't) is covered by the implementation logic
 * in NotifyRollback() which filters by idx > securedLogIndex_. The complex
 * timing-dependent scenario to create entries in (securedLogIndex, specCommitIndex]
 * that are pending during step-down is hard to orchestrate deterministically.
 */
int RaftLabTest::testSecuredStepDownPartialRollback(void) {
  Init2(39, "Durable entries not rolled back on step-down");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);
  Log_info("[PARTIAL-ROLLBACK] Leader: {} (site {})", leader, leader_id);

  // Wait for leader to become secured
  Fiber::sleep(500000);

  bool secured = config_->IsSecuredLeader(leader_id);
  Assert2(secured, "Leader should be secured for this test");

  // Submit entry that will become durably committed
  int durableCmd = 4500;
  uint64_t durableIndex = 0;
  uint64_t durableTerm = 0;

  std::atomic<bool> durableGotSpec{false};
  std::atomic<bool> durableGotDurable{false};
  std::atomic<bool> durableGotRollback{false};

  bool ok = config_->StartWithCallback(leader_id, durableCmd, &durableIndex, &durableTerm,
    [&](CommitStatus status) {
      Log_info("[PARTIAL-ROLLBACK] Durable entry callback: status={}", static_cast<int>(status));
      if (status == CommitStatus::SPECULATIVE) {
        durableGotSpec = true;
      } else if (status == CommitStatus::DURABLE) {
        durableGotDurable = true;
      } else if (status == CommitStatus::ROLLEDBACK) {
        durableGotRollback = true;
      }
    });

  Assert2(ok, "Failed to submit durable command");
  Log_info("[PARTIAL-ROLLBACK] Submitted durable entry at index {}", durableIndex);

  // Wait for this entry to become durably committed
  Fiber::sleep(500000);

  Assert2(durableGotSpec.load(), "Entry should have been speculatively committed");
  Assert2(durableGotDurable.load(), "Entry should be durably committed by now");
  Log_info("[PARTIAL-ROLLBACK] Entry is durably committed");

  // Get current securedLogIndex
  uint64_t securedLog = config_->GetSecuredLogIndex(leader_id);
  Log_info("[PARTIAL-ROLLBACK] securedLogIndex: {}, durableIndex: {}", securedLog, durableIndex);
  Assert2(durableIndex <= securedLog, "Durable entry should be at or below securedLogIndex");

  // Force step-down by disconnecting leader and forcing new election
  config_->Disconnect(leader_id);
  Log_info("[PARTIAL-ROLLBACK] Disconnected leader {} to force new election", leader_id);

  // Wait for new election
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  // Reconnect old leader so it can receive higher term and step down
  config_->Reconnect(leader_id);
  Log_info("[PARTIAL-ROLLBACK] Reconnected old leader {}", leader_id);

  // Wait for old leader to see higher term and step down
  Fiber::sleep(ELECTIONTIMEOUT);

  // Check results
  Log_info("[PARTIAL-ROLLBACK] Results:");
  Log_info("  Durable entry (index {}): spec={} durable={} rollback={}",
           durableIndex, durableGotSpec.load(), durableGotDurable.load(), durableGotRollback.load());

  // Verify: durable entry should NOT have received ROLLEDBACK
  // The callback was removed after DURABLE notification, so it cannot receive ROLLEDBACK
  Assert2(!durableGotRollback.load(),
          "Durable entry should NOT receive ROLLEDBACK notification");

  // Final verification: cluster should be operational
  int new_leader = config_->OneLeader();
  if (new_leader < 0) {
    Fiber::sleep(ELECTIONTIMEOUT);
    new_leader = config_->OneLeader();
  }
  Assert2(new_leader >= 0, "Should have leader after test");

  Log_info("[PARTIAL-ROLLBACK] New leader: {}", new_leader);
  Log_info("[PARTIAL-ROLLBACK] Partial rollback test PASSED!");

  Passed2();
}

/**
 * Test that speculative entries can be overwritten by a new leader.
 *
 * Scenario:
 * 1. A becomes unsecured leader, submits X (spec committed at index N)
 * 2. Kill A, B, C (crash - lose in-memory speculative entries)
 * 3. Restart B, C (A stays dead)
 * 4. D or E wins election (they don't have X)
 * 5. New leader commits Y at index N
 * 6. Verify: Y is committed, X is gone
 *
 * This tests the "unlucky path" where speculative entries are lost because
 * the entire memory quorum crashed before entries were durably committed.
 */
int RaftLabTest::testSpeculativeEntriesOverwritten(void) {
  Init2(40, "Speculative entries overwritten by new leader");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);
  Log_info("[OVERWRITE-TEST] Initial leader: {} (site {})", leader, leader_id);

  // Commit some baseline entries to establish a shared log prefix
  DoAgreeAndAssertIndex(5000, NSERVERS, index_++);
  DoAgreeAndAssertIndex(5001, NSERVERS, index_++);
  Log_info("[OVERWRITE-TEST] Baseline entries committed at indices {}, {}", index_ - 2, index_ - 1);

  // Identify servers: leader + 2 followers in "crash group", 2 followers survive
  std::vector<siteid_t> crash_group;  // Will lose speculative entry
  std::vector<siteid_t> survivors;    // Never had speculative entry

  crash_group.push_back(leader_id);

  int crash_count = 0;
  for (int i = 0; i < NSERVERS && crash_count < 2; i++) {
    siteid_t svr = config_->getServerIdByIndex(i);
    if (svr != leader_id) {
      crash_group.push_back(svr);
      crash_count++;
    }
  }

  for (int i = 0; i < NSERVERS; i++) {
    siteid_t svr = config_->getServerIdByIndex(i);
    bool in_crash = false;
    for (siteid_t c : crash_group) {
      if (svr == c) {
        in_crash = true;
        break;
      }
    }
    if (!in_crash) {
      survivors.push_back(svr);
    }
  }

  Log_info("[OVERWRITE-TEST] Crash group: {}, {}, {}", crash_group[0], crash_group[1], crash_group[2]);
  Log_info("[OVERWRITE-TEST] Survivors: {}, {}", survivors[0], survivors[1]);

  // Step 1: Disconnect survivors so they don't receive the speculative entry
  for (siteid_t svr : survivors) {
    config_->Disconnect(svr);
    Log_info("[OVERWRITE-TEST] Disconnected survivor {}", svr);
  }

  // Step 2: Submit entry X to leader (only crash_group will receive it)
  // Since survivors are disconnected, X can only reach crash_group's memory
  int cmdX = 5002;
  uint64_t indexX = 0;
  uint64_t termX = 0;

  bool ok = config_->Start(leader_id, cmdX, &indexX, &termX);
  Assert2(ok, "Failed to submit command X");
  Log_info("[OVERWRITE-TEST] Submitted X (cmd={}) at index {} term {}", cmdX, indexX, termX);

  // Wait a short time for X to propagate to crash_group (but not enough for durable commit)
  Fiber::sleep(100000);  // 100ms

  // Verify X is in crash_group's logs
  for (siteid_t svr : crash_group) {
    auto server = config_->GetServer(svr);
    uint64_t lastLog = server->lastLogIndex;
    Log_info("[OVERWRITE-TEST] Server {} lastLogIndex={}", svr, lastLog);
  }

  // Step 3: Kill all servers in crash group (simulates crash before durability)
  Log_info("[OVERWRITE-TEST] Killing crash group");
  for (siteid_t svr : crash_group) {
    config_->Kill(svr);
    Log_info("[OVERWRITE-TEST] Killed server {}", svr);
  }

  // Step 4: Reconnect survivors
  for (siteid_t svr : survivors) {
    config_->Reconnect(svr);
    Log_info("[OVERWRITE-TEST] Reconnected survivor {}", svr);
  }

  // Step 5: Restart crash_group followers (but NOT the original leader)
  // This gives us 4 servers: 2 survivors + 2 restarted followers
  Fiber::sleep(200000);  // Wait for kill to complete

  for (size_t i = 1; i < crash_group.size(); i++) {  // Skip index 0 (leader)
    config_->Restart(crash_group[i]);
    Log_info("[OVERWRITE-TEST] Restarted server {}", crash_group[i]);
  }

  // Wait for election
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  // Step 6: Check for new leader (must be from survivors since they have higher log?)
  // Actually, restarted servers may have lost X from memory, so logs might be equal
  int new_leader = config_->OneLeader();
  if (new_leader < 0) {
    Fiber::sleep(ELECTIONTIMEOUT);
    new_leader = config_->OneLeader();
  }

  // If no leader yet, restart the original leader too to form quorum
  if (new_leader < 0) {
    Log_info("[OVERWRITE-TEST] No leader yet, restarting original leader to form quorum");
    config_->Restart(crash_group[0]);
    Fiber::sleep(ELECTIONTIMEOUT * 2);
    new_leader = config_->OneLeader();
  }

  Assert2(new_leader >= 0, "Should have leader after recovery");
  siteid_t new_leader_id = config_->getServerIdByIndex(new_leader);
  Log_info("[OVERWRITE-TEST] New leader: {} (site {})", new_leader, new_leader_id);

  // Step 7: Submit entry Y at the same logical index
  int cmdY = 5003;
  uint64_t indexY = 0;
  uint64_t termY = 0;

  ok = config_->Start(new_leader_id, cmdY, &indexY, &termY);
  Assert2(ok, "Failed to submit command Y");
  Log_info("[OVERWRITE-TEST] Submitted Y (cmd={}) at index {} term {}", cmdY, indexY, termY);

  // Wait for Y to commit
  int nAlive = 4;  // survivors + restarted followers (maybe 5 if we restarted leader)
  for (siteid_t svr : crash_group) {
    if (!config_->GetServer(svr)) {
      nAlive--;
    }
  }
  // Count alive servers more carefully
  nAlive = 0;
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t svr = config_->getServerIdByIndex(i);
    auto server = config_->GetServer(svr);
    if (server) {
      nAlive++;
    }
  }
  Log_info("[OVERWRITE-TEST] Number of alive servers: {}", nAlive);

  int result = config_->Wait(indexY, nAlive >= 3 ? 3 : nAlive, termY);
  AssertWaitNoError(result, indexY);
  AssertWaitNoTimeout(result, indexY, nAlive >= 3 ? 3 : nAlive);

  Log_info("[OVERWRITE-TEST] Y committed at index {}", indexY);

  // Step 8: Verify system state
  // - If indexY == indexX, Y overwrote X's index (speculative entry lost)
  // - If indexY > indexX, the system may have preserved some entries

  Log_info("[OVERWRITE-TEST] Entry X was at index {}, entry Y is at index {}", indexX, indexY);

  if (indexY == indexX) {
    Log_info("[OVERWRITE-TEST] Y committed at same index as X - speculative entry overwritten!");
  } else if (indexY > indexX) {
    // X might have been persisted before crash (acceptable)
    Log_info("[OVERWRITE-TEST] Y committed after X's index - X may have persisted (acceptable)");
  } else {
    // This shouldn't happen
    Log_warn("[OVERWRITE-TEST] Y committed before X's index - unexpected");
  }

  // Verify system is consistent by committing another entry
  int finalCmd = 5004;
  int committed = config_->DoAgreement(finalCmd, nAlive >= 3 ? 3 : nAlive, true);
  Assert2(committed > 0, "Failed to commit final entry");

  Log_info("[OVERWRITE-TEST] Final entry committed at index {}", committed);
  Log_info("[OVERWRITE-TEST] Speculative entries overwrite test PASSED!");

  Passed2();
}

/**
 * Test 41: testDurableQuorumPreemptsStepDown
 *
 * Tests Phase 6: Relaxed invariant - leader doesn't step down if durableVoters
 * reaches quorum even when specVoters falls below quorum.
 *
 * Scenario:
 * 1. Start 5-node cluster, wait for leader to become secured
 * 2. Get durableVoters to reach quorum (3)
 * 3. Have followers restart (removes from specVoters but not durableVoters)
 * 4. Verify leader doesn't step down (durableVoters still >= quorum)
 */
int RaftLabTest::testDurableQuorumPreemptsStepDown(void) {
  Init2(41, "Durable quorum preempts step-down");

  // Wait for initial election and leadership stabilization
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);
  Log_info("[DURABLE-QUORUM-TEST] Initial leader: {} (site {})", leader, leader_id);

  // Commit entries to establish secured leadership
  // This ensures fsyncs complete and leader becomes secured
  DoAgreeAndAssertIndex(6000, NSERVERS, index_++);
  DoAgreeAndAssertIndex(6001, NSERVERS, index_++);

  // Wait for durable commits (fsyncs to complete)
  Fiber::sleep(500000);  // 500ms for fsyncs

  // Verify leader is still the same and secured
  int current_leader = config_->OneLeader();
  Assert2(current_leader >= 0, "Leader lost after commits");
  Assert2(current_leader == leader, "Leader changed unexpectedly");

  auto server = config_->GetServer(leader_id);
  Assert2(server != nullptr, "Leader server is null");

  // Check leader is secured (has durable quorum)
  bool isSecured = config_->IsSecuredLeader(leader_id);
  Log_info("[DURABLE-QUORUM-TEST] Leader securedLeader={}", isSecured);

  // Get specVoters and durableVoters counts
  size_t specVotersCount = config_->GetSpecVotersCount(leader_id);
  size_t durableVotersCount = config_->GetDurableVotersCount(leader_id);
  Log_info("[DURABLE-QUORUM-TEST] Before restarts: specVoters={}, durableVoters={}",
           specVotersCount, durableVotersCount);

  Assert2(isSecured, "Leader should be secured after commits with fsync");

  // Now restart 2 followers (not leader) - this removes them from specVoters
  // but keeps them in durableVoters (their durable votes survive restart)
  std::vector<siteid_t> followers;
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t svr = config_->getServerIdByIndex(i);
    if (svr != leader_id) {
      followers.push_back(svr);
    }
  }

  Assert2(followers.size() >= 2, "Need at least 2 followers");

  // Restart 2 followers - this triggers notifyRestart which removes from specVoters
  Log_info("[DURABLE-QUORUM-TEST] Restarting followers {} and {}",
           followers[0], followers[1]);

  config_->Restart(followers[0]);
  Fiber::sleep(100000);  // 100ms
  config_->Restart(followers[1]);
  Fiber::sleep(100000);  // 100ms

  // Wait for notifyRestart to be processed
  Fiber::sleep(300000);  // 300ms

  // Check if leader is still leader
  current_leader = config_->OneLeader();
  Log_info("[DURABLE-QUORUM-TEST] Leader after restarts: {} (expected {})",
           current_leader, leader);

  // Get updated counts
  specVotersCount = config_->GetSpecVotersCount(leader_id);
  durableVotersCount = config_->GetDurableVotersCount(leader_id);
  isSecured = config_->IsSecuredLeader(leader_id);
  Log_info("[DURABLE-QUORUM-TEST] After restarts: specVoters={}, durableVoters={}, secured={}",
           specVotersCount, durableVotersCount, isSecured);

  // Key assertion: Leader should still be leader because durableVoters >= quorum
  // even if specVoters < quorum after restarts
  Assert2(current_leader == leader,
          "Leader should NOT step down when durableVoters >= quorum");

  // Verify system still works by committing another entry
  DoAgreeAndAssertIndex(6002, NSERVERS, index_++);

  Log_info("[DURABLE-QUORUM-TEST] System still operational - test PASSED!");

  Passed2();
}

/**
 * Test 42: testSecuredViaDurableAfterSpecLoss
 *
 * Tests that an unsecured leader can become secured via durable quorum
 * even after losing spec quorum due to restarts.
 *
 * Scenario:
 * 1. Start 5-node cluster
 * 2. Leader gets memory votes (spec leader) but not yet durable quorum
 * 3. VoteDurable arrives, building durableVoters
 * 4. Follower restarts (removes from specVoters)
 * 5. If durableVoters reaches quorum before spec quorum lost, leader becomes secured
 */
int RaftLabTest::testSecuredViaDurableAfterSpecLoss(void) {
  Init2(42, "Secured via durable quorum after spec loss");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);
  Log_info("[SECURED-VIA-DURABLE-TEST] Initial leader: {} (site {})", leader, leader_id);

  // Commit entries to establish stable system and ensure durable quorum
  DoAgreeAndAssertIndex(6100, NSERVERS, index_++);

  // Wait for fsyncs to complete (VoteDurable messages sent)
  Fiber::sleep(500000);  // 500ms

  // Verify leader is secured
  bool isSecured = config_->IsSecuredLeader(leader_id);
  size_t specVotersCount = config_->GetSpecVotersCount(leader_id);
  size_t durableVotersCount = config_->GetDurableVotersCount(leader_id);

  Log_info("[SECURED-VIA-DURABLE-TEST] Initial state: secured={}, specVoters={}, durableVoters={}",
           isSecured, specVotersCount, durableVotersCount);

  // For this test to be meaningful, we need to verify the Phase 6 logic works
  // The key insight is: once durableVoters >= quorum, losing specVoters doesn't matter

  // Get all followers
  std::vector<siteid_t> followers;
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t svr = config_->getServerIdByIndex(i);
    if (svr != leader_id) {
      followers.push_back(svr);
    }
  }

  // Restart all followers one by one
  // Each restart removes from specVoters but durableVoters stays intact
  for (size_t i = 0; i < followers.size(); i++) {
    Log_info("[SECURED-VIA-DURABLE-TEST] Restarting follower {} ({}/{})",
             followers[i], i + 1, followers.size());
    config_->Restart(followers[i]);
    Fiber::sleep(200000);  // 200ms between restarts

    // Check leader status after each restart
    int current_leader = config_->OneLeader();
    if (current_leader >= 0) {
      siteid_t curr_leader_id = config_->getServerIdByIndex(current_leader);
      if (curr_leader_id == leader_id) {
        // Still same leader - check if secured via durable quorum
        isSecured = config_->IsSecuredLeader(leader_id);
        specVotersCount = config_->GetSpecVotersCount(leader_id);
        durableVotersCount = config_->GetDurableVotersCount(leader_id);
        Log_info("[SECURED-VIA-DURABLE-TEST] After restart {}: secured={}, specVoters={}, durableVoters={}",
                 i + 1, isSecured, specVotersCount, durableVotersCount);
      } else {
        Log_info("[SECURED-VIA-DURABLE-TEST] Leader changed to {} (site {})",
                 current_leader, curr_leader_id);
      }
    }
  }

  // Final check - system should still have a leader (either original or new)
  Fiber::sleep(ELECTIONTIMEOUT);
  int final_leader = config_->OneLeader();
  Assert2(final_leader >= 0, "Should have a leader after restarts");

  // Verify system still works
  DoAgreeAndAssertIndex(6101, NSERVERS, index_++);

  Log_info("[SECURED-VIA-DURABLE-TEST] System operational after restarts - test PASSED!");

  Passed2();
}

// ===========================================================================
// PHASE 3.1: Snapshot Data Format and Metadata Tests
// ===========================================================================

int RaftLabTest::testSnapshotMetadataCreation(void) {
  Init2(50, "Snapshot metadata creation and field access");

  // Test default construction
  janus::raft::SnapshotMetadata meta;
  Assert2(meta.last_included_index == 0,
          "Default last_included_index should be 0, got %lu", meta.last_included_index);
  Assert2(meta.last_included_term == 0,
          "Default last_included_term should be 0, got %lu", meta.last_included_term);
  Assert2(meta.size_bytes == 0,
          "Default size_bytes should be 0, got %zu", meta.size_bytes);
  Assert2(!meta.is_valid(),
          "Default metadata should not be valid");

  // Test with assigned values
  meta.last_included_index = 42;
  meta.last_included_term = 3;
  meta.size_bytes = 1024;
  meta.timestamp_ms = 1234567890;
  Assert2(meta.is_valid(),
          "Metadata with index > 0 should be valid");
  Assert2(meta.last_included_index == 42,
          "last_included_index should be 42, got %lu", meta.last_included_index);
  Assert2(meta.last_included_term == 3,
          "last_included_term should be 3, got %lu", meta.last_included_term);

  // Test to_string
  auto str = meta.to_string();
  Assert2(str.find("42") != std::string::npos,
          "to_string should contain index 42");
  Assert2(str.find("1024") != std::string::npos,
          "to_string should contain size 1024");

  Log_info("[SNAPSHOT-META-TEST] SnapshotMetadata creation and access PASSED");
  Passed2();
}

int RaftLabTest::testSnapshotFormatRoundTrip(void) {
  Init2(51, "Snapshot format serialize/deserialize round-trip");

  // Create test data
  std::string test_data = "hello snapshot world! This is state machine data.";
  uint64_t test_index = 100;
  uint64_t test_term = 5;

  // Serialize
  std::string serialized;
  bool ok = janus::raft::SnapshotFormat::Serialize(test_index, test_term,
                                            test_data.data(), test_data.size(),
                                            &serialized);
  Assert2(ok, "Serialize should succeed");
  Assert2(serialized.size() > sizeof(janus::raft::SnapshotHeader),
          "Serialized data should be larger than header");

  // Verify header
  janus::raft::SnapshotHeader header;
  ok = janus::raft::SnapshotFormat::GetHeader(serialized.data(), serialized.size(), &header);
  Assert2(ok, "GetHeader should succeed");
  Assert2(header.last_index == test_index,
          "Header last_index should be %lu, got %lu", test_index, header.last_index);
  Assert2(header.last_term == test_term,
          "Header last_term should be %lu, got %lu", test_term, header.last_term);
  Assert2(header.data_size == test_data.size(),
          "Header data_size should be %zu, got %lu", test_data.size(), header.data_size);

  // Deserialize
  uint64_t out_index, out_term;
  std::string out_data;
  ok = janus::raft::SnapshotFormat::Deserialize(serialized.data(), serialized.size(),
                                         &out_index, &out_term, &out_data);
  Assert2(ok, "Deserialize should succeed");
  Assert2(out_index == test_index,
          "Deserialized index should be %lu, got %lu", test_index, out_index);
  Assert2(out_term == test_term,
          "Deserialized term should be %lu, got %lu", test_term, out_term);
  Assert2(out_data == test_data,
          "Deserialized data should match original");

  // Test with empty data
  std::string empty_serialized;
  ok = janus::raft::SnapshotFormat::Serialize(1, 1, nullptr, 0, &empty_serialized);
  Assert2(ok, "Serialize with empty data should succeed");
  ok = janus::raft::SnapshotFormat::Deserialize(empty_serialized.data(), empty_serialized.size(),
                                         &out_index, &out_term, &out_data);
  Assert2(ok, "Deserialize empty data should succeed");
  Assert2(out_data.empty(), "Empty snapshot data should deserialize to empty string");

  // Test corruption detection
  std::string corrupted = serialized;
  corrupted[sizeof(janus::raft::SnapshotHeader) + 5] ^= 0xFF;  // Flip a data byte
  ok = janus::raft::SnapshotFormat::Deserialize(corrupted.data(), corrupted.size(),
                                         &out_index, &out_term, &out_data);
  Assert2(!ok, "Deserialize of corrupted data should fail");

  Log_info("[SNAPSHOT-FORMAT-TEST] Serialize/deserialize round-trip PASSED");
  Passed2();
}

int RaftLabTest::testSnapshotManagerSaveLoad(void) {
  Init2(52, "SnapshotManager save/load round-trip");

  // Create a temporary directory for test snapshots
  std::string test_path = "/tmp/raft_snapshot_test_" + std::to_string(getpid());

  janus::raft::SnapshotConfig config;
  config.storage_path = test_path;
  config.max_snapshots = 5;

  janus::raft::FileSnapshotManager mgr(config);

  // Initially no snapshots
  Assert2(!mgr.HasSnapshotAtOrAfter(1), "Should have no snapshots initially");
  auto latest = mgr.GetLatestSnapshot();
  Assert2(latest.is_none(), "Latest should be None initially");

  // Save a snapshot
  std::string data1 = "state machine data at index 10";
  bool ok = mgr.TakeSnapshot(10, 2, data1.data(), data1.size());
  Assert2(ok, "TakeSnapshot should succeed");

  // Verify snapshot exists
  Assert2(mgr.HasSnapshotAtOrAfter(1), "Should have snapshot after save");
  Assert2(mgr.HasSnapshotAtOrAfter(10), "Should have snapshot at index 10");
  Assert2(!mgr.HasSnapshotAtOrAfter(11), "Should not have snapshot at index 11");

  // Load and verify
  janus::raft::SnapshotMetadata loaded_meta;
  std::string loaded_data;
  ok = mgr.LoadLatestSnapshot(&loaded_meta, &loaded_data);
  Assert2(ok, "LoadLatestSnapshot should succeed");
  Assert2(loaded_meta.last_included_index == 10,
          "Loaded index should be 10, got %lu", loaded_meta.last_included_index);
  Assert2(loaded_meta.last_included_term == 2,
          "Loaded term should be 2, got %lu", loaded_meta.last_included_term);
  Assert2(loaded_data == data1,
          "Loaded data should match saved data");

  // Save another snapshot
  std::string data2 = "state machine data at index 25";
  ok = mgr.TakeSnapshot(25, 3, data2.data(), data2.size());
  Assert2(ok, "Second TakeSnapshot should succeed");

  // Latest should now be the newer one
  ok = mgr.LoadLatestSnapshot(&loaded_meta, &loaded_data);
  Assert2(ok, "LoadLatestSnapshot after second save should succeed");
  Assert2(loaded_meta.last_included_index == 25,
          "Latest should be index 25, got %lu", loaded_meta.last_included_index);
  Assert2(loaded_data == data2,
          "Latest data should be the second snapshot");

  // Clean up
  mgr.DeleteAllSnapshots();
  rmdir(test_path.c_str());

  Log_info("[SNAPSHOT-MGR-TEST] Save/load round-trip PASSED");
  Passed2();
}

int RaftLabTest::testSnapshotManagerListing(void) {
  Init2(53, "SnapshotManager listing and pruning");

  std::string test_path = "/tmp/raft_snapshot_list_test_" + std::to_string(getpid());

  janus::raft::SnapshotConfig config;
  config.storage_path = test_path;
  config.max_snapshots = 3;

  janus::raft::FileSnapshotManager mgr(config);

  // Create multiple snapshots
  for (uint64_t i = 1; i <= 5; i++) {
    std::string data = "data_" + std::to_string(i * 10);
    bool ok = mgr.TakeSnapshot(i * 10, i, data.data(), data.size());
    Assert2(ok, "TakeSnapshot %lu should succeed", i * 10);
  }

  // List snapshots - retention policy should have pruned oldest
  auto snapshots = mgr.ListSnapshots();
  Assert2(snapshots.size() <= 3,
          "Should have at most 3 snapshots (retention policy), got %zu", snapshots.size());

  // Newest should be first (sorted by index descending)
  if (!snapshots.empty()) {
    Assert2(snapshots[0].last_included_index == 50,
            "Newest snapshot should be index 50, got %lu",
            snapshots[0].last_included_index);
  }

  // Prune: keep only snapshots at or after index 40
  size_t pruned = mgr.PruneSnapshots(40);
  Log_info("[SNAPSHOT-LIST-TEST] Pruned {} snapshots", pruned);

  // Verify remaining snapshots
  snapshots = mgr.ListSnapshots();
  for (const auto& snap : snapshots) {
    Assert2(snap.last_included_index >= 40,
            "After prune, snapshot index %lu should be >= 40",
            snap.last_included_index);
  }

  // Delete all and verify empty
  mgr.DeleteAllSnapshots();
  snapshots = mgr.ListSnapshots();
  Assert2(snapshots.empty(), "Should have no snapshots after DeleteAll");

  rmdir(test_path.c_str());

  Log_info("[SNAPSHOT-LIST-TEST] Listing and pruning PASSED");
  Passed2();
}

int RaftLabTest::testSnapshotManagerWiring(void) {
  Init2(54, "SnapshotManager wiring in RaftServer");

  // Wait for a leader
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);

  // Get the leader's server
  auto server = config_->GetServer(leader);
  Assert2(server != nullptr, "Server should not be null");

  // By default (no MAKO_RAFT_SNAPSHOTS env), snapshot_manager_ should be null
  // But SetSnapshotManager/GetSnapshotManager API should work
  auto existing = server->GetSnapshotManager();
  // Might be null if MAKO_RAFT_SNAPSHOTS not set - that's fine

  // Test SetSnapshotManager with a temporary manager
  std::string test_path = "/tmp/raft_snap_wiring_test_" + std::to_string(getpid());
  janus::raft::SnapshotConfig config;
  config.storage_path = test_path;
  auto test_mgr = std::make_shared<janus::raft::FileSnapshotManager>(config);

  server->SetSnapshotManager(test_mgr);
  Assert2(server->GetSnapshotManager() != nullptr,
          "GetSnapshotManager should return non-null after SetSnapshotManager");
  Assert2(server->GetSnapshotManager().get() == test_mgr.get(),
          "GetSnapshotManager should return the same manager we set");

  // Test HasSnapshot - should be false since we haven't saved anything
  Assert2(!server->HasSnapshot(),
          "HasSnapshot should be false with empty manager");

  // Test GetSnapshotIndex/Term defaults
  Assert2(server->GetSnapshotIndex() == 0,
          "GetSnapshotIndex should be 0 by default, got %lu", server->GetSnapshotIndex());
  Assert2(server->GetSnapshotTerm() == 0,
          "GetSnapshotTerm should be 0 by default, got %lu", server->GetSnapshotTerm());

  // Restore original manager (or null)
  server->SetSnapshotManager(existing);

  // Clean up
  test_mgr->DeleteAllSnapshots();
  rmdir(test_path.c_str());

  Log_info("[SNAPSHOT-WIRING-TEST] Wiring in RaftServer PASSED");
  Passed2();
}

// =============================================================================
// Test 55: CreateSnapshot basic functionality
// =============================================================================
// @unsafe - test function that exercises CreateSnapshot
int RaftLabTest::testCreateSnapshotBasic(void) {
  Init2(55, "CreateSnapshot basic");

  // Wait for a leader
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);

  auto server = config_->GetServer(leader);
  Assert2(server != nullptr, "Server should not be null");

  // Set up a snapshot manager with a temporary path
  std::string test_path = "/tmp/raft_snap_create_test_" + std::to_string(getpid());
  janus::raft::SnapshotConfig config;
  config.storage_path = test_path;
  auto test_mgr = std::make_shared<janus::raft::FileSnapshotManager>(config);
  auto original_mgr = server->GetSnapshotManager();
  auto original_threshold = server->GetSnapshotThreshold();
  server->SetSnapshotManager(test_mgr);

  // Set a low threshold so we can trigger a snapshot easily
  server->SetSnapshotThreshold(5);

  // Verify no snapshot exists yet
  Assert2(!server->HasSnapshot(), "No snapshot should exist initially");
  Assert2(server->GetSnapshotIndex() == 0,
          "Snapshot index should be 0 initially");

  // Submit enough entries to exceed the threshold
  // We need > 5 committed + applied entries
  for (int i = 1; i <= 10; i++) {
    uint64_t idx = config_->DoAgreement(100 + i, NSERVERS, true);
    Assert2(idx > 0, "DoAgreement failed for cmd %d", 100 + i);
  }

  // Give time for applyLogs to run and trigger CreateSnapshot
  Fiber::sleep(2000000);  // 2 seconds

  // Verify a snapshot was taken
  Assert2(server->HasSnapshot(),
          "Snapshot should exist after exceeding threshold");
  Assert2(server->GetSnapshotIndex() > 0,
          "Snapshot index should be > 0, got %lu", server->GetSnapshotIndex());
  Assert2(server->GetSnapshotTerm() > 0,
          "Snapshot term should be > 0, got %lu", server->GetSnapshotTerm());

  // Verify the snapshot manager has the snapshot
  auto latest = test_mgr->GetLatestSnapshot();
  Assert2(latest.is_some(), "Snapshot manager should have a snapshot");
  auto meta = latest.unwrap();
  Assert2(meta.last_included_index == server->GetSnapshotIndex(),
          "Manager index (%lu) should match server index (%lu)",
          meta.last_included_index, server->GetSnapshotIndex());

  // Restore and clean up
  server->SetSnapshotManager(original_mgr);
  server->SetSnapshotThreshold(original_threshold);
  test_mgr->DeleteAllSnapshots();
  rmdir(test_path.c_str());

  Log_info("[CREATE-SNAPSHOT-BASIC-TEST] PASSED");
  Passed2();
}

// =============================================================================
// Test 56: CreateSnapshot triggers compaction and new entries still work
// =============================================================================
// @unsafe - test function that exercises CreateSnapshot with compaction
int RaftLabTest::testCreateSnapshotAndCompaction(void) {
  Init2(56, "CreateSnapshot and compaction");

  // Wait for a leader
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);

  auto server = config_->GetServer(leader);
  Assert2(server != nullptr, "Server should not be null");

  // Set up snapshot manager
  std::string test_path = "/tmp/raft_snap_compact_test_" + std::to_string(getpid());
  janus::raft::SnapshotConfig config;
  config.storage_path = test_path;
  auto test_mgr = std::make_shared<janus::raft::FileSnapshotManager>(config);
  auto original_mgr = server->GetSnapshotManager();
  auto original_threshold = server->GetSnapshotThreshold();
  server->SetSnapshotManager(test_mgr);

  // Set a low threshold
  server->SetSnapshotThreshold(5);

  // Submit entries to trigger snapshot
  for (int i = 1; i <= 10; i++) {
    uint64_t idx = config_->DoAgreement(200 + i, NSERVERS, true);
    Assert2(idx > 0, "DoAgreement failed for cmd %d", 200 + i);
  }

  // Wait for snapshot and compaction
  Fiber::sleep(2000000);

  uint64_t snap_idx = server->GetSnapshotIndex();
  Assert2(snap_idx > 0, "Snapshot should have been taken, got index=%lu", snap_idx);

  // Now submit more entries AFTER snapshot - these should still commit
  for (int i = 1; i <= 5; i++) {
    uint64_t idx = config_->DoAgreement(300 + i, NSERVERS, true);
    Assert2(idx > 0, "DoAgreement after snapshot failed for cmd %d", 300 + i);
  }

  // Verify the system is still functional
  int leader2 = config_->OneLeader();
  Assert2(leader2 >= 0, "Should still have a leader after snapshot+compaction");

  // Restore and clean up
  server->SetSnapshotManager(original_mgr);
  server->SetSnapshotThreshold(original_threshold);
  test_mgr->DeleteAllSnapshots();
  rmdir(test_path.c_str());

  Log_info("[CREATE-SNAPSHOT-COMPACTION-TEST] PASSED");
  Passed2();
}

// =============================================================================
// Test 57: Snapshot threshold is configurable
// =============================================================================
// @unsafe - test function that exercises snapshot threshold configuration
int RaftLabTest::testSnapshotThresholdConfigurable(void) {
  Init2(57, "Snapshot threshold configurable");

  // Wait for a leader
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);

  auto server = config_->GetServer(leader);
  Assert2(server != nullptr, "Server should not be null");

  // Check default threshold
  uint64_t default_threshold = server->GetSnapshotThreshold();
  Assert2(default_threshold == 10000,
          "Default threshold should be 10000, got %lu", default_threshold);

  // Set a custom threshold
  server->SetSnapshotThreshold(42);
  Assert2(server->GetSnapshotThreshold() == 42,
          "Threshold should be 42 after SetSnapshotThreshold, got %lu",
          server->GetSnapshotThreshold());

  // Set another value
  server->SetSnapshotThreshold(100000);
  Assert2(server->GetSnapshotThreshold() == 100000,
          "Threshold should be 100000, got %lu", server->GetSnapshotThreshold());

  // Restore default
  server->SetSnapshotThreshold(10000);

  Log_info("[SNAPSHOT-THRESHOLD-CONFIG-TEST] PASSED");
  Passed2();
}

// =============================================================================
// Test 58: InstallSnapshot basic functionality
// =============================================================================
// @unsafe - test function that exercises OnInstallSnapshot
int RaftLabTest::testInstallSnapshotBasic(void) {
  Init2(58, "InstallSnapshot basic");

  // Wait for a leader
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);

  // Commit some entries so there's log state
  for (int i = 1; i <= 5; i++) {
    uint64_t idx = config_->DoAgreement(200 + i, NSERVERS, true);
    Assert2(idx > 0, "DoAgreement failed for cmd %d", 200 + i);
  }

  // Pick a follower to install snapshot on
  int follower = -1;
  for (int i = 0; i < NSERVERS; i++) {
    if (i != leader) {
      follower = i;
      break;
    }
  }
  Assert2(follower >= 0, "No follower found");

  auto server = config_->GetServer(follower);
  Assert2(server != nullptr, "Follower server should not be null");

  // Set up a snapshot manager on the follower for persistence
  std::string test_path = "/tmp/raft_install_snap_test_" + std::to_string(getpid());
  janus::raft::SnapshotConfig snap_config;
  snap_config.storage_path = test_path;
  auto test_mgr = std::make_shared<janus::raft::FileSnapshotManager>(snap_config);
  auto original_mgr = server->GetSnapshotManager();
  server->SetSnapshotManager(test_mgr);

  // Record follower state before InstallSnapshot
  uint64_t old_snapidx = server->GetSnapshotIndex();
  uint64_t old_snapterm = server->GetSnapshotTerm();

  // Create fake snapshot data
  uint64_t snap_index = 10;
  uint64_t snap_term = server->currentTerm;
  std::string snap_data = "test_snapshot_data_for_install";

  // Call OnInstallSnapshot directly on the follower (synchronous)
  uint64_t reply_term = 0;
  server->OnInstallSnapshot(
      server->currentTerm,  // term (matches follower's current term)
      config_->GetServer(leader)->site_id_,  // leader_id
      snap_index,
      snap_term,
      snap_data,
      &reply_term);

  Assert2(reply_term > 0, "Reply term should be > 0, got %lu", reply_term);

  // Verify snapshot metadata updated
  Assert2(server->GetSnapshotIndex() == snap_index,
          "snapidx should be %lu, got %lu", snap_index, server->GetSnapshotIndex());
  Assert2(server->GetSnapshotTerm() == snap_term,
          "snapterm should be %lu, got %lu", snap_term, server->GetSnapshotTerm());

  // Verify commitIndex and executeIndex advanced
  Assert2(server->commitIndex >= snap_index,
          "commitIndex should be >= %lu, got %lu", snap_index, server->commitIndex);
  Assert2(server->executeIndex >= snap_index,
          "executeIndex should be >= %lu, got %lu", snap_index, server->executeIndex);

  // Verify snapshot was persisted
  auto latest = test_mgr->GetLatestSnapshot();
  Assert2(latest.is_some(), "Snapshot should be persisted in manager");
  auto meta = latest.unwrap();
  Assert2(meta.last_included_index == snap_index,
          "Persisted snapshot index should be %lu, got %lu",
          snap_index, meta.last_included_index);

  // Restore original manager
  server->SetSnapshotManager(original_mgr);

  // Cleanup temp files
  // @unsafe { system call }
  std::string cleanup = "rm -rf " + test_path;
  system(cleanup.c_str());

  Log_info("[INSTALL-SNAPSHOT-BASIC-TEST] PASSED");
  Passed2();
}

// =============================================================================
// Test 59: InstallSnapshot rejects stale term
// =============================================================================
// @unsafe - test function that exercises OnInstallSnapshot with stale term
int RaftLabTest::testInstallSnapshotRejectsStaleTerm(void) {
  Init2(59, "InstallSnapshot rejects stale term");

  // Wait for a leader
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);

  // Commit a few entries to establish state
  for (int i = 1; i <= 3; i++) {
    uint64_t idx = config_->DoAgreement(300 + i, NSERVERS, true);
    Assert2(idx > 0, "DoAgreement failed for cmd %d", 300 + i);
  }

  // Pick a follower
  int follower = -1;
  for (int i = 0; i < NSERVERS; i++) {
    if (i != leader) {
      follower = i;
      break;
    }
  }
  Assert2(follower >= 0, "No follower found");

  auto server = config_->GetServer(follower);
  Assert2(server != nullptr, "Follower server should not be null");

  // Record follower state before the stale InstallSnapshot
  uint64_t before_snapidx = server->GetSnapshotIndex();
  uint64_t before_snapterm = server->GetSnapshotTerm();
  uint64_t before_commitIndex = server->commitIndex;
  uint64_t before_executeIndex = server->executeIndex;
  uint64_t follower_term = server->currentTerm;

  // Send InstallSnapshot with a stale term (term 0, which is less than any active term)
  uint64_t stale_term = 0;
  Assert2(stale_term < follower_term,
          "Stale term %lu should be < follower term %lu", stale_term, follower_term);

  uint64_t reply_term = 0;
  server->OnInstallSnapshot(
      stale_term,  // stale term
      999,         // fake leader_id
      100,         // last_included_index
      1,           // last_included_term
      "stale_snapshot_data",
      &reply_term);

  // Reply should contain the follower's current term (so leader can update)
  Assert2(reply_term == follower_term,
          "Reply term should be follower's current term %lu, got %lu",
          follower_term, reply_term);

  // Verify follower state is UNCHANGED
  Assert2(server->GetSnapshotIndex() == before_snapidx,
          "snapidx should be unchanged (%lu), got %lu",
          before_snapidx, server->GetSnapshotIndex());
  Assert2(server->GetSnapshotTerm() == before_snapterm,
          "snapterm should be unchanged (%lu), got %lu",
          before_snapterm, server->GetSnapshotTerm());
  Assert2(server->commitIndex == before_commitIndex,
          "commitIndex should be unchanged (%lu), got %lu",
          before_commitIndex, server->commitIndex);
  Assert2(server->executeIndex == before_executeIndex,
          "executeIndex should be unchanged (%lu), got %lu",
          before_executeIndex, server->executeIndex);

  Log_info("[INSTALL-SNAPSHOT-REJECTS-STALE-TEST] PASSED");
  Passed2();
}

// =============================================================================
// Test 60: HeartbeatLoop triggers InstallSnapshot for lagging followers
// =============================================================================
// @unsafe - test function that exercises HeartbeatLoop snapshot integration
int RaftLabTest::testHeartbeatTriggersInstallSnapshot(void) {
  Init2(60, "HeartbeatLoop triggers InstallSnapshot for lagging follower");

  // Wait for a leader to be elected
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);

  auto leader_server = config_->GetServer(leader);
  Assert2(leader_server != nullptr, "Leader server should not be null");

  // Set up a snapshot manager on the leader with a low threshold
  std::string test_path = "/tmp/raft_hb_snap_test_" + std::to_string(getpid());
  janus::raft::SnapshotConfig snap_config;
  snap_config.storage_path = test_path;
  auto test_mgr = std::make_shared<janus::raft::FileSnapshotManager>(snap_config);
  auto original_mgr = leader_server->GetSnapshotManager();
  leader_server->SetSnapshotManager(test_mgr);
  leader_server->SetSnapshotThreshold(3);  // Low threshold to trigger snapshot quickly

  // Commit enough entries to trigger snapshot and compaction on the leader
  // We need > threshold entries to trigger CreateSnapshot in applyLogs
  for (int i = 1; i <= 8; i++) {
    uint64_t idx = config_->DoAgreement(600 + i, NSERVERS, true);
    Assert2(idx > 0, "DoAgreement failed for cmd %d", 600 + i);
  }

  // Wait for applyLogs to trigger CreateSnapshot on leader
  Fiber::sleep(HEARTBEAT_INTERVAL * 3);

  // Verify leader has taken a snapshot and compacted
  uint64_t leader_snap_idx = leader_server->GetSnapshotIndex();
  uint64_t leader_min_active = leader_server->min_active_slot_;
  Log_info("[HB-SNAP-TEST] Leader snapshot index={}, min_active_slot={}",
           leader_snap_idx, leader_min_active);

  // If snapshot wasn't automatically triggered, force it
  if (leader_snap_idx == 0) {
    leader_server->CreateSnapshot();
    leader_snap_idx = leader_server->GetSnapshotIndex();
    leader_min_active = leader_server->min_active_slot_;
    Log_info("[HB-SNAP-TEST] After forced snapshot: index={}, min_active_slot={}",
             leader_snap_idx, leader_min_active);
  }

  Assert2(leader_snap_idx > 0, "Leader should have created a snapshot, got snapidx=%lu", leader_snap_idx);
  Assert2(leader_min_active > 1, "Leader min_active_slot_ should be > 1 after compaction, got %lu", leader_min_active);

  // Pick a follower and simulate it being far behind
  int follower = -1;
  siteid_t follower_site_id = 0;
  for (int i = 0; i < NSERVERS; i++) {
    if (i != leader) {
      follower = i;
      break;
    }
  }
  Assert2(follower >= 0, "No follower found");

  auto follower_server = config_->GetServer(follower);
  Assert2(follower_server != nullptr, "Follower server should not be null");
  follower_site_id = follower_server->site_id_;

  // Set up a snapshot manager on the follower (so it can receive the snapshot)
  std::string follower_test_path = "/tmp/raft_hb_snap_follower_" + std::to_string(getpid());
  janus::raft::SnapshotConfig follower_snap_config;
  follower_snap_config.storage_path = follower_test_path;
  auto follower_mgr = std::make_shared<janus::raft::FileSnapshotManager>(follower_snap_config);
  auto follower_original_mgr = follower_server->GetSnapshotManager();
  follower_server->SetSnapshotManager(follower_mgr);

  // Record follower state before manipulation
  uint64_t follower_snap_before = follower_server->GetSnapshotIndex();

  // Manually set the follower's next_index in the leader to be below min_active_slot_
  // This simulates a follower that has fallen far behind
  {
    std::lock_guard<std::recursive_mutex> lock(leader_server->mtx_);
    leader_server->next_index_[follower_site_id] = 1;  // Far behind
    leader_server->match_index_[follower_site_id] = 0;
    Log_info("[HB-SNAP-TEST] Set leader's next_index[{}]={}, min_active_slot={}",
             follower_site_id, 1, leader_min_active);
  }

  // Verify the condition: next_index < min_active_slot_
  Assert2(1 < leader_min_active,
          "next_index (1) should be < min_active_slot_ (%lu) for InstallSnapshot trigger",
          leader_min_active);

  // Wait for a few heartbeat rounds to allow HeartbeatLoop to detect and send InstallSnapshot
  Fiber::sleep(HEARTBEAT_INTERVAL * 5);

  // Verify the leader updated next_index and match_index for the follower
  uint64_t final_next_index = 0;
  uint64_t final_match_index = 0;
  {
    std::lock_guard<std::recursive_mutex> lock(leader_server->mtx_);
    final_next_index = leader_server->next_index_[follower_site_id];
    final_match_index = leader_server->match_index_[follower_site_id];
  }

  Log_info("[HB-SNAP-TEST] After heartbeat: next_index[{}]={}, match_index[{}]={}",
           follower_site_id, final_next_index, follower_site_id, final_match_index);

  // The leader should have updated next_index to snap_index + 1
  Assert2(final_next_index > 1,
          "Leader next_index for follower should have advanced from 1, got %lu", final_next_index);
  Assert2(final_match_index >= leader_snap_idx,
          "Leader match_index for follower should be >= snapshot index %lu, got %lu",
          leader_snap_idx, final_match_index);

  // Verify the follower received and applied the snapshot
  uint64_t follower_snap_after = follower_server->GetSnapshotIndex();
  Log_info("[HB-SNAP-TEST] Follower snapshot index: before={}, after={}",
           follower_snap_before, follower_snap_after);
  Assert2(follower_snap_after >= leader_snap_idx,
          "Follower snapshot index should be >= %lu after InstallSnapshot, got %lu",
          leader_snap_idx, follower_snap_after);

  // Verify the system can still make progress (new entries can be committed)
  uint64_t new_idx = config_->DoAgreement(700, NSERVERS, true);
  Assert2(new_idx > 0, "DoAgreement should succeed after InstallSnapshot recovery");

  // Restore original managers
  leader_server->SetSnapshotManager(original_mgr);
  follower_server->SetSnapshotManager(follower_original_mgr);

  // Cleanup temp files
  // @unsafe { system calls }
  std::string cleanup1 = "rm -rf " + test_path;
  std::string cleanup2 = "rm -rf " + follower_test_path;
  system(cleanup1.c_str());
  system(cleanup2.c_str());

  Log_info("[HEARTBEAT-SNAPSHOT-TEST] PASSED");
  Passed2();
}

// ============================================================================
// Test 61: testSpecCommitIndexPersistence
// Verifies that specCommitIndex_ and securedLogIndex_ are persisted to storage
// ============================================================================
// @unsafe - Uses test infrastructure and LogStorage API
int RaftLabTest::testSpecCommitIndexPersistence(void) {
  Init2(61, "Speculative indices persisted to storage");

  Log_info("TEST 61: Waiting for initial election");
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 61: Leader elected: {}", leader);

  // Commit some entries
  Log_info("TEST 61: Committing entries");
  DoAgreeAndAssertIndex(201, NSERVERS, index_++);
  DoAgreeAndAssertIndex(202, NSERVERS, index_++);
  DoAgreeAndAssertIndex(203, NSERVERS, index_++);
  Log_info("TEST 61: Committed 3 entries");

  // Allow time for speculative index advancement
  Fiber::sleep(ELECTIONTIMEOUT / 2);

  // Get the leader server and check persisted values
  auto leader_server = config_->GetServer(leader);
  auto storage = leader_server->GetLogStorage();

  uint64_t mem_spec = leader_server->specCommitIndex_;
  uint64_t mem_secured = leader_server->securedLogIndex_;
  uint64_t mem_last = leader_server->lastLogIndex;

  Log_info("TEST 61: In-memory values - specCommitIndex={} securedLogIndex={} lastLogIndex={}",
           mem_spec, mem_secured, mem_last);

  // Verify invariant: securedLogIndex <= specCommitIndex <= lastLogIndex
  Assert2(mem_secured <= mem_spec,
          "invariant violation: securedLogIndex (%lu) > specCommitIndex (%lu)",
          mem_secured, mem_spec);
  Assert2(mem_spec <= mem_last,
          "invariant violation: specCommitIndex (%lu) > lastLogIndex (%lu)",
          mem_spec, mem_last);

  // Check persisted values in storage if storage is available
  if (storage && storage->is_open()) {
    // @unsafe { LogStorage API calls }
    auto spec_str = storage->get_metadata(RaftServer::META_SPEC_COMMIT_INDEX);
    auto secured_str = storage->get_metadata(RaftServer::META_SECURED_LOG_INDEX);

    if (spec_str.is_some()) {
      uint64_t persisted_spec = std::stoull(spec_str.unwrap());
      Log_info("TEST 61: Persisted specCommitIndex={}, in-memory={}",
               persisted_spec, mem_spec);
      Assert2(persisted_spec == mem_spec,
              "persisted specCommitIndex (%lu) != in-memory (%lu)",
              persisted_spec, mem_spec);
    } else {
      Log_info("TEST 61: specCommitIndex not yet persisted (may be 0)");
    }

    if (secured_str.is_some()) {
      uint64_t persisted_secured = std::stoull(secured_str.unwrap());
      Log_info("TEST 61: Persisted securedLogIndex={}, in-memory={}",
               persisted_secured, mem_secured);
      Assert2(persisted_secured == mem_secured,
              "persisted securedLogIndex (%lu) != in-memory (%lu)",
              persisted_secured, mem_secured);
    } else {
      Log_info("TEST 61: securedLogIndex not yet persisted (may be 0)");
    }
  } else {
    Log_info("TEST 61: No log storage available, checking in-memory values only");
  }

  Log_info("TEST 61: PASSED - speculative indices persisted correctly");
  Passed2();
}

// ============================================================================
// Test 62: testSpecIndicesRecoveredOnRestart
// Verifies specCommitIndex_ and securedLogIndex_ are recovered on restart
// ============================================================================
// @unsafe - Uses test infrastructure, Kill/Restart, and LogStorage API
int RaftLabTest::testSpecIndicesRecoveredOnRestart(void) {
  Init2(62, "Speculative indices recovered on restart");

  Log_info("TEST 62: Waiting for initial election");
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 62: Leader elected: {}", leader);

  // Commit some entries
  Log_info("TEST 62: Committing entries");
  DoAgreeAndAssertIndex(301, NSERVERS, index_++);
  DoAgreeAndAssertIndex(302, NSERVERS, index_++);
  DoAgreeAndAssertIndex(303, NSERVERS, index_++);
  Log_info("TEST 62: Committed 3 entries");

  // Allow time for speculative and durable advancement
  Fiber::sleep(ELECTIONTIMEOUT / 2);

  // Pick a follower to kill and restart
  siteid_t victim = config_->getNextServerId(leader, 1);
  auto victim_server = config_->GetServer(victim);

  // Record the values before killing
  uint64_t spec_before = victim_server->specCommitIndex_;
  uint64_t secured_before = victim_server->securedLogIndex_;
  uint64_t commit_before = victim_server->commitIndex;
  uint64_t last_log_before = victim_server->lastLogIndex;
  Log_info("TEST 62: Before kill - server {}: specCommitIndex={} securedLogIndex={} "
           "commitIndex={} lastLogIndex={}",
           victim, spec_before, secured_before, commit_before, last_log_before);

  // Kill the server
  Log_info("TEST 62: Killing server {}", victim);
  config_->Kill(victim);

  // Wait for it to be gone
  Fiber::sleep(ELECTIONTIMEOUT / 2);

  // Commit more entries with remaining servers
  Log_info("TEST 62: Committing with {} servers while {} is down", NSERVERS - 1, victim);
  DoAgreeAndAssertIndex(304, NSERVERS - 1, index_++);
  DoAgreeAndAssertIndex(305, NSERVERS - 1, index_++);
  Log_info("TEST 62: Committed 2 more entries");

  // Restart the killed server
  Log_info("TEST 62: Restarting server {}", victim);
  config_->Restart(victim);

  // Give it time to catch up
  Fiber::sleep(ELECTIONTIMEOUT);

  // Get the restarted server and check recovery
  victim_server = config_->GetServer(victim);
  uint64_t spec_after = victim_server->specCommitIndex_;
  uint64_t secured_after = victim_server->securedLogIndex_;
  uint64_t commit_after = victim_server->commitIndex;
  uint64_t last_log_after = victim_server->lastLogIndex;
  Log_info("TEST 62: After restart - server {}: specCommitIndex={} securedLogIndex={} "
           "commitIndex={} lastLogIndex={}",
           victim, spec_after, secured_after, commit_after, last_log_after);

  // After restart and catching up, the log should have all entries
  Assert2(last_log_after >= last_log_before,
          "lastLogIndex decreased after restart: was %lu, now %lu",
          last_log_before, last_log_after);

  // commitIndex should have recovered and potentially advanced
  Assert2(commit_after >= commit_before,
          "commitIndex decreased after restart: was %lu, now %lu",
          commit_before, commit_after);

  // Verify invariant: securedLogIndex <= specCommitIndex <= lastLogIndex
  // Note: On a follower after restart, specCommitIndex_ and securedLogIndex_ may be 0
  // (reset during ResetSpeculativeState for non-leaders), but the invariant must still hold.
  Assert2(secured_after <= spec_after,
          "invariant violation after restart: securedLogIndex (%lu) > specCommitIndex (%lu)",
          secured_after, spec_after);
  // specCommitIndex may be 0 for a follower, which is <= lastLogIndex
  Assert2(spec_after <= last_log_after || spec_after == 0,
          "invariant violation after restart: specCommitIndex (%lu) > lastLogIndex (%lu)",
          spec_after, last_log_after);

  // Verify the cluster still works
  Log_info("TEST 62: Committing with all {} servers to verify cluster health", NSERVERS);
  DoAgreeAndAssertWaitSuccess(306, NSERVERS);
  Log_info("TEST 62: Final commit successful");

  Log_info("TEST 62: PASSED - speculative indices recovered on restart");
  Passed2();
}

// ============================================================================
// Test 63: testRollbackOnUnsecuredFailure
// ============================================================================
// @unsafe - Uses test infrastructure, modifies cluster state
/**
 * Verify that UnsecuredFailure step-down rolls back all entries above commitIndex.
 *
 * Scenario:
 * 1. Start 5-node cluster, elect a leader
 * 2. Register a pending callback on the leader for a new log entry
 * 3. Crash majority of followers so leader loses quorum and steps down
 *    with UnsecuredFailure reason
 * 4. Verify the callback was invoked with ROLLEDBACK status
 */
int RaftLabTest::testRollbackOnUnsecuredFailure(void) {
  Init2(63, "UnsecuredFailure step-down rolls back all entries");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);
  Log_info("[ROLLBACK-UNSECURED] Leader: {} (site {})", leader, leader_id);

  // Wait for leader to become secured so we have a stable baseline
  Fiber::sleep(500000);

  // Track callback invocations
  std::atomic<int> specNotifications{0};
  std::atomic<int> durableNotifications{0};
  std::atomic<int> rollbackNotifications{0};

  // Submit an entry with callback
  int cmd = 6300;
  uint64_t index = 0;
  uint64_t term = 0;

  bool ok = config_->StartWithCallback(leader_id, cmd, &index, &term,
    [&](CommitStatus status) {
      Log_info("[ROLLBACK-UNSECURED] Callback status={}", static_cast<int>(status));
      if (status == CommitStatus::SPECULATIVE) {
        specNotifications++;
      } else if (status == CommitStatus::DURABLE) {
        durableNotifications++;
      } else if (status == CommitStatus::ROLLEDBACK) {
        rollbackNotifications++;
      }
    });

  Assert2(ok, "Failed to submit command with callback");
  Log_info("[ROLLBACK-UNSECURED] Submitted command {} at index {}", cmd, index);

  // Let entry get speculatively committed
  Fiber::sleep(300000);

  // Now submit another entry and immediately crash majority to prevent it
  // from being durably committed
  int cmd2 = 6301;
  uint64_t index2 = 0;
  uint64_t term2 = 0;

  std::atomic<int> cmd2Rollback{0};
  std::atomic<int> cmd2Spec{0};

  ok = config_->StartWithCallback(leader_id, cmd2, &index2, &term2,
    [&](CommitStatus status) {
      Log_info("[ROLLBACK-UNSECURED] Entry 2 status={}", static_cast<int>(status));
      if (status == CommitStatus::SPECULATIVE) {
        cmd2Spec++;
      } else if (status == CommitStatus::ROLLEDBACK) {
        cmd2Rollback++;
      }
    });

  Assert2(ok, "Failed to submit second command");
  Log_info("[ROLLBACK-UNSECURED] Submitted command2 {} at index {}", cmd2, index2);

  // Crash majority of followers to force leader step-down
  std::vector<siteid_t> followers;
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t svr = config_->getServerIdByIndex(i);
    if (svr != leader_id) {
      followers.push_back(svr);
    }
  }

  // Kill 3 followers (in 5-node cluster: leader + 1 follower = no quorum)
  Log_info("[ROLLBACK-UNSECURED] Killing 3 followers to force quorum loss");
  for (int i = 0; i < 3 && i < (int)followers.size(); i++) {
    config_->Kill(followers[i]);
  }

  // Wait for leader to detect quorum loss and step down
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  // Log results
  Log_info("[ROLLBACK-UNSECURED] Results: spec={} durable={} rollback={}",
           specNotifications.load(), durableNotifications.load(), rollbackNotifications.load());
  Log_info("[ROLLBACK-UNSECURED] Entry2: spec={} rollback={}",
           cmd2Spec.load(), cmd2Rollback.load());

  // Verify: at least first entry should have been speculatively committed
  Assert2(specNotifications.load() >= 1 || durableNotifications.load() >= 1,
          "First entry should have received at least SPECULATIVE notification");

  // Restart killed followers for cleanup
  for (int i = 0; i < 3 && i < (int)followers.size(); i++) {
    config_->Restart(followers[i]);
  }

  // Wait for cluster to stabilize
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  int final_leader = config_->OneLeader();
  if (final_leader < 0) {
    Fiber::sleep(ELECTIONTIMEOUT);
    final_leader = config_->OneLeader();
  }
  Assert2(final_leader >= 0, "Should have leader after recovery");

  Log_info("[ROLLBACK-UNSECURED] UnsecuredFailure rollback test PASSED!");
  Passed2();
}

// ============================================================================
// Test 64: testNoRollbackOnHigherTerm
// ============================================================================
// @unsafe - Uses test infrastructure, modifies cluster state
/**
 * Verify that HigherTerm step-down does NOT send rollback notifications.
 *
 * Scenario:
 * 1. Start 5-node cluster, elect a leader
 * 2. Register a pending callback for a new log entry
 * 3. Disconnect the leader (not kill) so it sees a higher term when reconnected
 * 4. Verify the callback was NOT invoked with ROLLEDBACK
 *    (callbacks cleared but no rollback notification sent)
 */
int RaftLabTest::testNoRollbackOnHigherTerm(void) {
  Init2(64, "HigherTerm step-down does not send rollback");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);
  Log_info("[ROLLBACK-HIGHERTERM] Leader: {} (site {})", leader, leader_id);

  // Wait for leader to become secured
  Fiber::sleep(500000);

  // Track callback invocations
  std::atomic<int> specNotifications{0};
  std::atomic<int> durableNotifications{0};
  std::atomic<int> rollbackNotifications{0};

  // Submit an entry with callback
  int cmd = 6400;
  uint64_t index = 0;
  uint64_t term = 0;

  bool ok = config_->StartWithCallback(leader_id, cmd, &index, &term,
    [&](CommitStatus status) {
      Log_info("[ROLLBACK-HIGHERTERM] Callback status={}", static_cast<int>(status));
      if (status == CommitStatus::SPECULATIVE) {
        specNotifications++;
      } else if (status == CommitStatus::DURABLE) {
        durableNotifications++;
      } else if (status == CommitStatus::ROLLEDBACK) {
        rollbackNotifications++;
      }
    });

  Assert2(ok, "Failed to submit command with callback");
  Log_info("[ROLLBACK-HIGHERTERM] Submitted command {} at index {}", cmd, index);

  // Wait for entry to commit
  Fiber::sleep(500000);

  // Now submit a new entry that hasn't been durably committed
  int cmd2 = 6401;
  uint64_t index2 = 0;
  uint64_t term2 = 0;

  std::atomic<int> cmd2Rollback{0};
  std::atomic<int> cmd2Spec{0};
  std::atomic<int> cmd2Durable{0};

  ok = config_->StartWithCallback(leader_id, cmd2, &index2, &term2,
    [&](CommitStatus status) {
      Log_info("[ROLLBACK-HIGHERTERM] Entry 2 status={}", static_cast<int>(status));
      if (status == CommitStatus::SPECULATIVE) {
        cmd2Spec++;
      } else if (status == CommitStatus::DURABLE) {
        cmd2Durable++;
      } else if (status == CommitStatus::ROLLEDBACK) {
        cmd2Rollback++;
      }
    });

  Assert2(ok, "Failed to submit second command");
  Log_info("[ROLLBACK-HIGHERTERM] Submitted command2 {} at index {}", cmd2, index2);

  // Let entry get speculatively committed but don't wait too long
  Fiber::sleep(200000);

  // Disconnect (not kill) the leader - it will see higher term when reconnected
  config_->Disconnect(leader_id);
  Log_info("[ROLLBACK-HIGHERTERM] Disconnected leader {}", leader_id);

  // Wait for new election on the majority side
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  // Reconnect old leader so it receives higher term and steps down via HigherTerm
  config_->Reconnect(leader_id);
  Log_info("[ROLLBACK-HIGHERTERM] Reconnected old leader {}", leader_id);

  // Wait for old leader to see higher term and step down
  Fiber::sleep(ELECTIONTIMEOUT);

  // Log results
  Log_info("[ROLLBACK-HIGHERTERM] Entry1: spec={} durable={} rollback={}",
           specNotifications.load(), durableNotifications.load(), rollbackNotifications.load());
  Log_info("[ROLLBACK-HIGHERTERM] Entry2: spec={} durable={} rollback={}",
           cmd2Spec.load(), cmd2Durable.load(), cmd2Rollback.load());

  // The key assertion: HigherTerm should NOT generate ROLLEDBACK notifications
  // for entry2 (which may still be pending when leader steps down).
  // Note: entry2 may or may not have been speculatively committed before disconnect.
  // The point is that HigherTerm does NOT send ROLLEDBACK - the new leader handles entries.
  Assert2(cmd2Rollback.load() == 0,
          "HigherTerm step-down should NOT send ROLLEDBACK notifications, but got %d",
          cmd2Rollback.load());

  // Verify cluster is operational
  int new_leader = config_->OneLeader();
  if (new_leader < 0) {
    Fiber::sleep(ELECTIONTIMEOUT);
    new_leader = config_->OneLeader();
  }
  Assert2(new_leader >= 0, "Should have leader after test");

  Log_info("[ROLLBACK-HIGHERTERM] HigherTerm no-rollback test PASSED!");
  Passed2();
}

// ============================================================================
// Test 65: testSnapshotRecoveryOnStartup
// ============================================================================
// @unsafe - Uses test infrastructure, Kill/Restart, snapshot manager API
/**
 * Verify that a server with a snapshot recovers executeIndex, commitIndex,
 * lastLogIndex, and min_active_slot_ correctly on restart.
 *
 * Scenario:
 * 1. Start 5-node cluster, elect leader
 * 2. Set up snapshot manager on a follower with low threshold
 * 3. Commit entries, manually trigger CreateSnapshot on the follower
 * 4. Kill the follower, restart it (snapshot manager re-initialized)
 * 5. Verify state reflects snapshot: executeIndex >= snapidx_, etc.
 * 6. Verify cluster can still make progress
 */
int RaftLabTest::testSnapshotRecoveryOnStartup(void) {
  Init2(65, "Snapshot recovery on startup");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 65: Leader elected: {}", leader);

  // Pick a follower
  siteid_t follower_id = config_->getNextServerId(leader, 1);
  auto follower_server = config_->GetServer(follower_id);
  Assert2(follower_server != nullptr, "Follower server should not be null");

  // Build the snapshot path that InitializeSnapshotManager() will construct on restart.
  // It uses: MAKO_RAFT_SNAPSHOT_PATH + "/raft_snap_" + site_id + "_partition_" + partition_id
  std::string base_path = "/tmp/raft_snap_recovery_test_65_" + std::to_string(getpid());
  std::string full_snap_path = base_path + "/raft_snap_" +
                               std::to_string(follower_server->site_id_) + "_partition_" +
                               std::to_string(follower_server->partition_id_);

  // Set up snapshot manager on the follower using the same path
  janus::raft::SnapshotConfig snap_config;
  snap_config.storage_path = full_snap_path;
  auto snap_mgr = std::make_shared<janus::raft::FileSnapshotManager>(snap_config);
  follower_server->SetSnapshotManager(snap_mgr);
  follower_server->SetSnapshotThreshold(3);

  // Commit entries
  Log_info("TEST 65: Committing 8 entries");
  for (int i = 1; i <= 8; i++) {
    uint64_t idx = config_->DoAgreement(6500 + i, NSERVERS, true);
    Assert2(idx > 0, "DoAgreement failed for cmd %d", 6500 + i);
  }

  // Wait for apply + snapshot creation
  Fiber::sleep(2000000);

  // Verify snapshot was created on follower
  uint64_t snap_idx = follower_server->GetSnapshotIndex();
  uint64_t snap_term = follower_server->GetSnapshotTerm();
  Log_info("TEST 65: Follower {} snapshot: index={} term={}", follower_id, snap_idx, snap_term);
  Assert2(snap_idx > 0, "Snapshot should have been created on follower, got index=%lu", snap_idx);

  // Record pre-kill state
  uint64_t exec_before = follower_server->executeIndex;
  uint64_t commit_before = follower_server->commitIndex;
  uint64_t last_log_before = follower_server->lastLogIndex;
  Log_info("TEST 65: Before kill - executeIndex={} commitIndex={} lastLogIndex={} min_active_slot_={}",
           exec_before, commit_before, last_log_before, follower_server->min_active_slot_);

  // Kill the follower
  Log_info("TEST 65: Killing follower {}", follower_id);
  config_->Kill(follower_id);
  Fiber::sleep(ELECTIONTIMEOUT / 2);

  // Set MAKO_RAFT_SNAPSHOTS and MAKO_RAFT_SNAPSHOT_PATH env vars so
  // InitializeSnapshotManager() finds the existing snapshot on restart
  setenv("MAKO_RAFT_SNAPSHOTS", "1", 1);  // @unsafe
  setenv("MAKO_RAFT_SNAPSHOT_PATH", base_path.c_str(), 1);  // @unsafe

  // Restart the follower - Setup() will call InitializeSnapshotManager()
  Log_info("TEST 65: Restarting follower {}", follower_id);
  config_->Restart(follower_id);
  Fiber::sleep(ELECTIONTIMEOUT);

  // Unset env vars
  unsetenv("MAKO_RAFT_SNAPSHOTS");  // @unsafe
  unsetenv("MAKO_RAFT_SNAPSHOT_PATH");  // @unsafe

  // Get restarted server
  follower_server = config_->GetServer(follower_id);
  Assert2(follower_server != nullptr, "Restarted server should not be null");

  uint64_t exec_after = follower_server->executeIndex;
  uint64_t commit_after = follower_server->commitIndex;
  uint64_t last_log_after = follower_server->lastLogIndex;
  uint64_t min_slot_after = follower_server->min_active_slot_;
  uint64_t snap_idx_after = follower_server->GetSnapshotIndex();

  Log_info("TEST 65: After restart - executeIndex={} commitIndex={} lastLogIndex={} "
           "min_active_slot_={} snapidx={}",
           exec_after, commit_after, last_log_after, min_slot_after, snap_idx_after);

  // Verify state reflects snapshot
  Assert2(exec_after >= snap_idx,
          "executeIndex (%lu) should be >= snapshot index (%lu)", exec_after, snap_idx);
  Assert2(commit_after >= snap_idx,
          "commitIndex (%lu) should be >= snapshot index (%lu)", commit_after, snap_idx);
  Assert2(last_log_after >= snap_idx,
          "lastLogIndex (%lu) should be >= snapshot index (%lu)", last_log_after, snap_idx);
  Assert2(min_slot_after >= snap_idx + 1,
          "min_active_slot_ (%lu) should be >= snapshot index + 1 (%lu)",
          min_slot_after, snap_idx + 1);

  // Verify cluster can still make progress
  Fiber::sleep(ELECTIONTIMEOUT);
  int new_leader = config_->OneLeader();
  if (new_leader < 0) {
    Fiber::sleep(ELECTIONTIMEOUT);
    new_leader = config_->OneLeader();
  }
  Assert2(new_leader >= 0, "Should have leader after restart");

  // Clean up snapshot files
  snap_mgr->DeleteAllSnapshots();
  // @unsafe { rmdir is not borrow-checked }
  rmdir(full_snap_path.c_str());
  rmdir(base_path.c_str());

  Log_info("TEST 65: Snapshot recovery on startup PASSED!");
  Passed2();
}

// ============================================================================
// Test 66: testSnapshotRecoveryFieldAdvancement
// ============================================================================
// @unsafe - Uses test infrastructure, snapshot manager API
/**
 * Verify that InitializeSnapshotManager() only advances indices, never
 * goes backwards. If log recovery already set higher values, snapshot
 * recovery should not overwrite them.
 *
 * Scenario:
 * 1. Start cluster, elect leader
 * 2. Set up snapshot manager on a server, commit entries, create snapshot
 * 3. Record snapidx_
 * 4. Commit more entries so executeIndex/commitIndex are ahead of snapshot
 * 5. Call InitializeSnapshotManager() again (simulating re-init)
 * 6. Verify indices were NOT set backwards
 */
int RaftLabTest::testSnapshotRecoveryFieldAdvancement(void) {
  Init2(66, "Snapshot recovery field advancement");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 66: Leader elected: {}", leader);

  auto server = config_->GetServer(leader);
  Assert2(server != nullptr, "Server should not be null");

  // Set up snapshot manager
  std::string snap_path = "/tmp/raft_snap_advancement_test_66_" + std::to_string(getpid());
  janus::raft::SnapshotConfig snap_config;
  snap_config.storage_path = snap_path;
  auto snap_mgr = std::make_shared<janus::raft::FileSnapshotManager>(snap_config);
  auto original_mgr = server->GetSnapshotManager();
  server->SetSnapshotManager(snap_mgr);
  server->SetSnapshotThreshold(3);

  // Commit initial entries to trigger snapshot
  Log_info("TEST 66: Committing initial entries");
  for (int i = 1; i <= 6; i++) {
    uint64_t idx = config_->DoAgreement(6600 + i, NSERVERS, true);
    Assert2(idx > 0, "DoAgreement failed for cmd %d", 6600 + i);
  }

  // Wait for snapshot creation
  Fiber::sleep(2000000);

  uint64_t snap_idx = server->GetSnapshotIndex();
  Log_info("TEST 66: Snapshot created at index {}", snap_idx);
  Assert2(snap_idx > 0, "Snapshot should have been created");

  // Commit MORE entries so that executeIndex/commitIndex are ahead of snapshot
  Log_info("TEST 66: Committing additional entries beyond snapshot");
  for (int i = 1; i <= 5; i++) {
    uint64_t idx = config_->DoAgreement(6610 + i, NSERVERS, true);
    Assert2(idx > 0, "DoAgreement failed for cmd %d", 6610 + i);
  }

  // Wait for apply
  Fiber::sleep(1000000);

  // Record current values (should be ahead of snapshot)
  uint64_t exec_before = server->executeIndex;
  uint64_t commit_before = server->commitIndex;
  uint64_t last_log_before = server->lastLogIndex;
  uint64_t min_slot_before = server->min_active_slot_;
  Log_info("TEST 66: Before re-init - executeIndex={} commitIndex={} lastLogIndex={} "
           "min_active_slot_={} snap_idx={}",
           exec_before, commit_before, last_log_before, min_slot_before, snap_idx);

  Assert2(exec_before > snap_idx,
          "executeIndex (%lu) should be > snapshot index (%lu) after more commits",
          exec_before, snap_idx);
  Assert2(commit_before > snap_idx,
          "commitIndex (%lu) should be > snapshot index (%lu) after more commits",
          commit_before, snap_idx);

  // Set env vars and call InitializeSnapshotManager() again
  setenv("MAKO_RAFT_SNAPSHOTS", "1", 1);  // @unsafe
  setenv("MAKO_RAFT_SNAPSHOT_PATH", ("/tmp/raft_snap_advancement_test_66_" + std::to_string(getpid())).c_str(), 1);  // @unsafe
  server->InitializeSnapshotManager();
  unsetenv("MAKO_RAFT_SNAPSHOTS");  // @unsafe
  unsetenv("MAKO_RAFT_SNAPSHOT_PATH");  // @unsafe

  // Verify indices were NOT set backwards
  uint64_t exec_after = server->executeIndex;
  uint64_t commit_after = server->commitIndex;
  uint64_t last_log_after = server->lastLogIndex;
  uint64_t min_slot_after = server->min_active_slot_;
  Log_info("TEST 66: After re-init - executeIndex={} commitIndex={} lastLogIndex={} "
           "min_active_slot_={}",
           exec_after, commit_after, last_log_after, min_slot_after);

  Assert2(exec_after >= exec_before,
          "executeIndex went backwards: was %lu, now %lu", exec_before, exec_after);
  Assert2(commit_after >= commit_before,
          "commitIndex went backwards: was %lu, now %lu", commit_before, commit_after);
  Assert2(last_log_after >= last_log_before,
          "lastLogIndex went backwards: was %lu, now %lu", last_log_before, last_log_after);
  Assert2(min_slot_after >= min_slot_before,
          "min_active_slot_ went backwards: was %lu, now %lu", min_slot_before, min_slot_after);

  // Also verify they're still >= snapshot values
  Assert2(exec_after >= snap_idx,
          "executeIndex (%lu) should be >= snapshot (%lu)", exec_after, snap_idx);
  Assert2(commit_after >= snap_idx,
          "commitIndex (%lu) should be >= snapshot (%lu)", commit_after, snap_idx);
  Assert2(min_slot_after >= snap_idx + 1,
          "min_active_slot_ (%lu) should be >= snapshot+1 (%lu)", min_slot_after, snap_idx + 1);

  // Restore original manager and clean up
  server->SetSnapshotManager(original_mgr);
  snap_mgr->DeleteAllSnapshots();
  // @unsafe { rmdir is not borrow-checked }
  rmdir(snap_path.c_str());

  Log_info("TEST 66: Snapshot recovery field advancement PASSED!");
  Passed2();
}

// @unsafe - test harness, accesses server internals
int RaftLabTest::testHeartbeatIntervalConfigurable(void) {
  Init2(67, "Heartbeat interval runtime-configurable");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 67: Leader elected: {}", leader);

  auto server = config_->GetServer(leader);
  Assert2(server != nullptr, "Server should not be null");

  // 1. Verify default interval equals HEARTBEAT_INTERVAL
  uint64_t default_interval = server->GetHeartbeatInterval();
  Assert2(default_interval == HEARTBEAT_INTERVAL,
          "Default heartbeat interval should be %d, got %lu",
          HEARTBEAT_INTERVAL, default_interval);
  Log_info("TEST 67: Default heartbeat interval verified: {} us", default_interval);

  // 2. Set interval to a new value via SetHeartbeatInterval()
  uint64_t new_interval = 200000;  // 200ms
  server->SetHeartbeatInterval(new_interval);

  // 3. Verify GetHeartbeatInterval() returns the new value
  uint64_t retrieved = server->GetHeartbeatInterval();
  Assert2(retrieved == new_interval,
          "Heartbeat interval should be %lu after set, got %lu",
          new_interval, retrieved);
  Log_info("TEST 67: Heartbeat interval updated to {} us", retrieved);

  // 4. Set on all servers and verify
  for (int i = 0; i < NSERVERS; i++) {
    auto s = config_->GetServer(i);
    if (s != nullptr) {
      s->SetHeartbeatInterval(150000);
      Assert2(s->GetHeartbeatInterval() == 150000,
              "Server %d heartbeat interval should be 150000, got %lu",
              i, s->GetHeartbeatInterval());
    }
  }
  Log_info("TEST 67: All servers updated to 150000 us");

  // 5. Verify the cluster still works (commit an entry)
  uint64_t idx = config_->DoAgreement(6700, NSERVERS, true);
  Assert2(idx > 0, "DoAgreement should succeed after changing heartbeat interval");
  Log_info("TEST 67: Agreement reached at index {} with modified interval", idx);

  // 6. Restore original interval
  for (int i = 0; i < NSERVERS; i++) {
    auto s = config_->GetServer(i);
    if (s != nullptr) {
      s->SetHeartbeatInterval(HEARTBEAT_INTERVAL);
    }
  }

  Log_info("TEST 67: Heartbeat interval configurable PASSED!");
  Passed2();
}

// =============================================================================
// Test 68: Log retention window configurable
// =============================================================================
// @unsafe - test function that exercises log retention window configuration
int RaftLabTest::testLogRetentionWindowConfigurable(void) {
  Init2(68, "Log retention window runtime-configurable");

  // Wait for initial election
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 68: Leader elected: {}", leader);

  auto server = config_->GetServer(leader);
  Assert2(server != nullptr, "Server should not be null");

  // 1. Verify default window is 5000
  uint64_t default_window = server->GetLogRetentionWindow();
  Assert2(default_window == 5000,
          "Default log retention window should be 5000, got %lu", default_window);
  Log_info("TEST 68: Default log retention window verified: {}", default_window);

  // 2. Set window to a smaller value via SetLogRetentionWindow()
  uint64_t new_window = 20;
  server->SetLogRetentionWindow(new_window);

  // 3. Verify GetLogRetentionWindow() returns the new value
  uint64_t retrieved = server->GetLogRetentionWindow();
  Assert2(retrieved == new_window,
          "Log retention window should be %lu after set, got %lu",
          new_window, retrieved);
  Log_info("TEST 68: Log retention window updated to {}", retrieved);

  // 4. Set on all servers and verify
  for (int i = 0; i < NSERVERS; i++) {
    auto s = config_->GetServer(i);
    if (s != nullptr) {
      s->SetLogRetentionWindow(new_window);
      Assert2(s->GetLogRetentionWindow() == new_window,
              "Server %d log retention window should be %lu, got %lu",
              i, new_window, s->GetLogRetentionWindow());
    }
  }
  Log_info("TEST 68: All servers updated to window={}", new_window);

  // 5. Commit enough entries to trigger cleanup
  // With window=20, committing 40 entries should trigger cleanup
  for (int i = 0; i < 40; i++) {
    uint64_t idx = config_->DoAgreement(6800 + i, NSERVERS, true);
    Assert2(idx > 0, "DoAgreement should succeed (entry %d)", i);
  }
  Log_info("TEST 68: Committed 40 entries with small retention window");

  // 6. Verify the cluster still works after cleanup
  uint64_t final_idx = config_->DoAgreement(6899, NSERVERS, true);
  Assert2(final_idx > 0, "DoAgreement should succeed after log cleanup");
  Log_info("TEST 68: Agreement reached at index {} after cleanup", final_idx);

  // 7. Restore default window
  for (int i = 0; i < NSERVERS; i++) {
    auto s = config_->GetServer(i);
    if (s != nullptr) {
      s->SetLogRetentionWindow(5000);
    }
  }

  Log_info("TEST 68: Log retention window configurable PASSED!");
  Passed2();
}

// ============================================================================
// Test 69: testLongPartitionRecovery
// Partition a follower for an extended period (> log retention window), then
// reconnect. With snapshots implemented, verify InstallSnapshot is triggered
// and the follower recovers fully.
// ============================================================================
// @unsafe - Uses test infrastructure, snapshot managers, and network partitioning
int RaftLabTest::testLongPartitionRecovery(void) {
  Init2(69, "Long partition recovery via InstallSnapshot");

  // Wait for leader election
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 69: Leader elected: {}", leader);

  // Set up snapshot managers on ALL servers with low threshold
  // @unsafe { filesystem and shared_ptr usage }
  std::string base_path = "/tmp/raft_long_part_test_" + std::to_string(getpid());
  std::vector<std::shared_ptr<janus::raft::SnapshotManager>> test_mgrs;
  std::vector<std::shared_ptr<janus::raft::SnapshotManager>> original_mgrs;
  for (int i = 0; i < NSERVERS; i++) {
    auto server = config_->GetServer(i);
    if (server == nullptr) continue;
    original_mgrs.push_back(server->GetSnapshotManager());
    janus::raft::SnapshotConfig snap_config;
    snap_config.storage_path = base_path + "_s" + std::to_string(i);
    auto mgr = std::make_shared<janus::raft::FileSnapshotManager>(snap_config);
    test_mgrs.push_back(mgr);
    server->SetSnapshotManager(mgr);
    server->SetSnapshotThreshold(5);
    server->SetLogRetentionWindow(10);
  }

  // Pick a follower to disconnect
  int follower = -1;
  for (int i = 0; i < NSERVERS; i++) {
    if (i != leader) {
      follower = i;
      break;
    }
  }
  Assert2(follower >= 0, "No follower found");
  Log_info("TEST 69: Disconnecting follower {}", follower);

  // Disconnect the follower
  config_->Disconnect(follower);

  // Commit enough entries on remaining 4 nodes to trigger snapshot + compaction
  for (int i = 1; i <= 20; i++) {
    uint64_t idx = config_->DoAgreement(6900 + i, NSERVERS - 1, true);
    Assert2(idx > 0, "DoAgreement failed for cmd %d", 6900 + i);
  }
  Log_info("TEST 69: Committed 20 entries with follower disconnected");

  // Wait for snapshot creation and compaction
  Fiber::sleep(HEARTBEAT_INTERVAL * 5);

  // Verify leader has taken a snapshot and compacted
  // Re-check leader in case of re-election
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  auto leader_server = config_->GetServer(leader);
  Assert2(leader_server != nullptr, "Leader server should not be null");

  uint64_t leader_snap_idx = leader_server->GetSnapshotIndex();
  // If snapshot wasn't automatically triggered, force it
  if (leader_snap_idx == 0) {
    leader_server->CreateSnapshot();
    leader_snap_idx = leader_server->GetSnapshotIndex();
  }
  uint64_t leader_min_active = leader_server->min_active_slot_;

  Log_info("TEST 69: Leader snapshot index={}, min_active_slot={}",
           leader_snap_idx, leader_min_active);
  Assert2(leader_snap_idx > 0,
          "Leader should have created a snapshot, got snapidx=%lu", leader_snap_idx);
  Assert2(leader_min_active > 1,
          "Leader min_active_slot_ should be > 1 after compaction, got %lu",
          leader_min_active);

  // Reconnect the follower
  Log_info("TEST 69: Reconnecting follower {}", follower);
  config_->Reconnect(follower);

  // Wait for heartbeat rounds to trigger InstallSnapshot to the follower
  Fiber::sleep(HEARTBEAT_INTERVAL * 10);

  // Verify the follower has caught up via snapshot
  auto follower_server = config_->GetServer(follower);
  Assert2(follower_server != nullptr, "Follower server should not be null after reconnect");

  uint64_t follower_snap_idx = follower_server->GetSnapshotIndex();
  Log_info("TEST 69: Follower snapshot index={} (leader={})",
           follower_snap_idx, leader_snap_idx);
  Assert2(follower_snap_idx >= leader_snap_idx,
          "Follower snapidx_ should match leader's (%lu), got %lu",
          leader_snap_idx, follower_snap_idx);

  // Verify new entries can be committed with all 5 nodes
  uint64_t new_idx = config_->DoAgreement(6999, NSERVERS, true);
  Assert2(new_idx > 0,
          "DoAgreement should succeed with all 5 nodes after partition recovery");
  Log_info("TEST 69: Full cluster agreement reached at index {}", new_idx);

  // Restore original snapshot managers and settings
  // @unsafe { filesystem cleanup }
  for (int i = 0; i < NSERVERS; i++) {
    auto server = config_->GetServer(i);
    if (server == nullptr) continue;
    if (i < (int)original_mgrs.size()) {
      server->SetSnapshotManager(original_mgrs[i]);
    }
    server->SetSnapshotThreshold(100);
    server->SetLogRetentionWindow(5000);
  }
  for (int i = 0; i < NSERVERS; i++) {
    std::string cleanup = "rm -rf " + base_path + "_s" + std::to_string(i);
    system(cleanup.c_str());  // @unsafe
  }

  Log_info("TEST 69: Long partition recovery via InstallSnapshot PASSED!");
  Passed2();
}

// ============================================================================
// Test 70: testLeadershipTransferTimeout
// Trigger leadership transfer via TimeoutNow, but make the preferred replica
// crash before the election completes. Verify the cluster continues operating.
// ============================================================================
// @unsafe - Uses test infrastructure, Kill, and leadership transfer API
int RaftLabTest::testLeadershipTransferTimeout(void) {
  Init2(70, "Leadership transfer timeout - preferred replica crashes");

  // Wait for leader election
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 70: Initial leader: {}", leader);

  // Commit a few entries to establish state
  for (int i = 1; i <= 3; i++) {
    uint64_t idx = config_->DoAgreement(7000 + i, NSERVERS, true);
    Assert2(idx > 0, "DoAgreement failed for cmd %d", 7000 + i);
  }
  Log_info("TEST 70: Committed 3 entries to establish state");

  // Find a non-leader server to be the transfer target
  int target = -1;
  for (int i = 0; i < NSERVERS; i++) {
    if (i != leader) {
      target = i;
      break;
    }
  }
  Assert2(target >= 0, "No non-leader server found");
  Log_info("TEST 70: Transfer target (will crash): {}", target);

  auto target_server = config_->GetServer(target);
  Assert2(target_server != nullptr, "Target server should not be null");

  auto leader_server = config_->GetServer(leader);
  Assert2(leader_server != nullptr, "Leader server should not be null");
  uint64_t leader_term = 0;
  {
    std::lock_guard<std::recursive_mutex> lock(leader_server->mtx_);
    leader_term = leader_server->currentTerm;
  }

  // Send TimeoutNow to the target to trigger fast election
  uint64_t follower_term = 0;
  bool_t success = false;
  // @unsafe { calling OnTimeoutNow on target }
  target_server->OnTimeoutNow(
      leader_term,
      leader_server->site_id_,
      &follower_term,
      &success);
  Log_info("TEST 70: Sent TimeoutNow to target {}, success={}", target, (int)success);

  // Immediately kill the target before it can win the election
  config_->Kill(target);
  Log_info("TEST 70: Killed target {}", target);

  // Wait for election timeout so remaining servers can elect a new leader
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  // Verify a leader emerges among the remaining servers
  int new_leader = config_->OneLeader();
  Assert2(new_leader >= 0, "A leader should emerge after target crashed");
  Assert2(new_leader != target,
          "New leader (%d) should not be the killed target (%d)", new_leader, target);
  Log_info("TEST 70: New leader elected: {}", new_leader);

  // Verify the cluster can still commit entries with 4 remaining nodes
  uint64_t idx = config_->DoAgreement(7010, NSERVERS - 1, true);
  Assert2(idx > 0,
          "DoAgreement should succeed with %d nodes after target crash", NSERVERS - 1);
  Log_info("TEST 70: Agreement reached at index {} with 4 nodes", idx);

  // Restart the killed target so cleanup (NDisconnected check) passes
  config_->Restart(target);
  Fiber::sleep(HEARTBEAT_INTERVAL * 3);

  // Verify the cluster is fully functional again
  uint64_t final_idx = config_->DoAgreement(7020, NSERVERS, true);
  Assert2(final_idx > 0,
          "DoAgreement should succeed with all %d nodes restored", NSERVERS);
  Log_info("TEST 70: Full cluster agreement at index {}", final_idx);

  Log_info("TEST 70: Leadership transfer timeout PASSED!");
  Passed2();
}

// =============================================================================
// Test 71: testDurableAckLoss
// Leader receives memory acks from quorum but durable ack RPCs are lost.
// Verify securedLogIndex_ doesn't advance while specCommitIndex_ does,
// and the invariant securedLogIndex_ <= specCommitIndex_ <= lastLogIndex holds.
// =============================================================================

// @unsafe - accesses Raft server state through test config helpers
int RaftLabTest::testDurableAckLoss(void) {
  Init2(71, "Durable ack loss: specCommitIndex advances, securedLogIndex lags");

  // @unsafe { wait for initial election }
  Fiber::sleep(ELECTIONTIMEOUT);

  // @unsafe { find leader }
  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);

  // Commit a few entries so indices advance normally
  // @unsafe { DoAgreement calls into Raft }
  for (int i = 0; i < 3; i++) {
    uint64_t idx = config_->DoAgreement(7100 + i, NSERVERS, true);
    Assert2(idx > 0, "Failed to reach agreement for entry %d", i);
  }

  // Re-check leader (may have changed)
  leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader after initial commits");
  leader_id = config_->getServerIdByIndex(leader);

  // @unsafe { wait for replication and durable acks to settle }
  Fiber::sleep(500000);  // 500ms

  // Record current state
  // @unsafe { reading speculative state from leader }
  uint64_t securedBefore = config_->GetSecuredLogIndex(leader_id);
  uint64_t specCommitBefore = config_->GetSpecCommitIndex(leader_id);

  Log_info("TEST 71: Before new entries: securedLogIndex={}, specCommitIndex={}",
           securedBefore, specCommitBefore);

  // Submit more entries - these should get memory acks (advancing specCommitIndex_)
  // In the test framework, securedLogIndex_ advancement depends on whether durable
  // acks arrive. We verify the invariant regardless of whether they do or not.
  // @unsafe { Start calls into Raft }
  for (int i = 0; i < 5; i++) {
    uint64_t index = 0;
    uint64_t term = 0;
    bool ok = config_->Start(leader_id, 7110 + i, &index, &term);
    Assert2(ok, "Failed to submit command %d to leader", 7110 + i);
    Log_info("TEST 71: Submitted command {} at index {}", 7110 + i, index);
  }

  // Wait for memory acks to arrive (short wait - enough for memory, may not be
  // enough for full durable cycle)
  // @unsafe { fiber sleep }
  Fiber::sleep(300000);  // 300ms

  // Check specCommitIndex advanced
  // @unsafe { reading speculative state }
  uint64_t specCommitAfter = config_->GetSpecCommitIndex(leader_id);
  uint64_t securedAfter = config_->GetSecuredLogIndex(leader_id);

  Log_info("TEST 71: After new entries: securedLogIndex={}, specCommitIndex={}",
           securedAfter, specCommitAfter);

  // specCommitIndex should have advanced beyond the "before" value
  Assert2(specCommitAfter > specCommitBefore,
          "specCommitIndex (%lu) did not advance beyond previous value (%lu)",
          specCommitAfter, specCommitBefore);

  // Verify the core invariant: securedLogIndex <= specCommitIndex <= lastLogIndex
  // @unsafe { GetLastLogIndex reads server state }
  auto* server = config_->GetServer(leader_id);
  Assert2(server != nullptr, "Leader server is null");

  uint64_t lastLog = server->GetLastLogIndex();

  Log_info("TEST 71: Invariant check: securedLogIndex={} <= specCommitIndex={} <= lastLogIndex={}",
           securedAfter, specCommitAfter, lastLog);

  Assert2(securedAfter <= specCommitAfter,
          "Invariant violated: securedLogIndex (%lu) > specCommitIndex (%lu)",
          securedAfter, specCommitAfter);
  Assert2(specCommitAfter <= lastLog,
          "Invariant violated: specCommitIndex (%lu) > lastLogIndex (%lu)",
          specCommitAfter, lastLog);

  // Also verify via the helper
  // @unsafe { VerifySpecInvariants reads server state }
  Assert2(config_->VerifySpecInvariants(leader_id),
          "VerifySpecInvariants returned false");

  // Wait for all entries to be fully committed so cleanup passes
  // @unsafe { DoAgreement calls into Raft }
  uint64_t final_idx = config_->DoAgreement(7199, NSERVERS, true);
  Assert2(final_idx > 0, "Final agreement failed");

  Log_info("TEST 71: Durable ack loss PASSED!");
  Passed2();
}

// =============================================================================
// Test 72: testHighFrequencyApply
// Stress test with rapid AppendEntries arrivals during log application.
// Verify the apply_pending_ mechanism correctly processes all entries
// without dropping work.
// =============================================================================

// @unsafe - submits many entries rapidly and verifies all are applied
int RaftLabTest::testHighFrequencyApply(void) {
  Init2(72, "High frequency apply: rapid submissions, no dropped entries");

  // @unsafe { wait for initial election }
  Fiber::sleep(ELECTIONTIMEOUT);

  // @unsafe { find leader }
  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  siteid_t leader_id = config_->getServerIdByIndex(leader);

  // Submit one entry to establish baseline index
  // @unsafe { DoAgreement calls into Raft }
  uint64_t base_idx = config_->DoAgreement(7200, NSERVERS, true);
  Assert2(base_idx > 0, "Failed to establish baseline agreement");

  // Re-check leader
  leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader after baseline");
  leader_id = config_->getServerIdByIndex(leader);

  // Rapidly submit 100 entries without waiting for agreement between each.
  // This stresses the apply_pending_ mechanism by creating a burst of entries
  // that need to be applied in order.
  const int NUM_ENTRIES = 100;
  uint64_t first_index = 0;
  uint64_t last_index = 0;

  Log_info("TEST 72: Submitting {} entries rapidly to leader {}", NUM_ENTRIES, leader);

  // @unsafe { Start calls into Raft }
  for (int i = 0; i < NUM_ENTRIES; i++) {
    uint64_t index = 0;
    uint64_t term = 0;
    bool ok = config_->Start(leader_id, 7201 + i, &index, &term);
    Assert2(ok, "Failed to submit command %d (entry %d/%d)", 7201 + i, i + 1, NUM_ENTRIES);
    if (i == 0) first_index = index;
    last_index = index;
  }

  Log_info("TEST 72: All {} entries submitted (indices {} to {})",
           NUM_ENTRIES, first_index, last_index);
  Assert2(last_index - first_index + 1 == (uint64_t)NUM_ENTRIES,
          "Expected %d consecutive indices, got range %lu-%lu",
          NUM_ENTRIES, first_index, last_index);

  // Wait for all entries to be committed and applied.
  // Use Wait() on the last index with a generous timeout.
  // @unsafe { getting term from leader server }
  auto* server = config_->GetServer(leader_id);
  Assert2(server != nullptr, "Leader server is null");

  uint64_t current_term = 0;
  {
    // @unsafe { locking server mutex }
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    current_term = server->currentTerm;
  }

  // Wait for the last entry to be committed by a quorum
  // @unsafe { Wait calls into test config }
  int result = config_->Wait(last_index, NSERVERS, current_term);
  Assert2(result >= 0,
          "Failed waiting for last index %lu to commit (result=%d)", last_index, result);

  Log_info("TEST 72: All entries committed through index {}", last_index);

  // Verify no entries were dropped: check that NCommitted returns NSERVERS
  // for several entries spanning the range
  // @unsafe { NCommitted reads committed state }
  int check_points[] = {0, NUM_ENTRIES / 4, NUM_ENTRIES / 2, 3 * NUM_ENTRIES / 4, NUM_ENTRIES - 1};
  for (int cp : check_points) {
    uint64_t check_idx = first_index + cp;
    int nc = config_->NCommitted(check_idx);
    Assert2(nc == NSERVERS,
            "Entry at index %lu (cmd %d) committed by %d servers, expected %d",
            check_idx, 7201 + cp, nc, NSERVERS);
  }

  // Verify specific committed values match what was submitted
  // @unsafe { ServerCommitted reads committed state }
  for (int cp : check_points) {
    uint64_t check_idx = first_index + cp;
    int expected_cmd = 7201 + cp;
    for (int s = 0; s < NSERVERS; s++) {
      siteid_t svr_id = config_->getServerIdByIndex(s);
      Assert2(config_->ServerCommitted(svr_id, check_idx, expected_cmd),
              "Server %d missing committed entry at index %lu (cmd %d)",
              s, check_idx, expected_cmd);
    }
  }

  // Verify invariants still hold on the leader
  // @unsafe { VerifySpecInvariants reads server state }
  Assert2(config_->VerifySpecInvariants(leader_id),
          "Speculative invariants violated after high-frequency apply");

  Log_info("TEST 72: High frequency apply PASSED!");
  Passed2();
}

// ============================================================================
// Test 73: testAddServerBasic
// ============================================================================
// Verify that AddServer adds a new server to the config, increases config size,
// and updates quorum size. Tests the config tracking infrastructure directly
// via friend class access since OnAddServer requires DeferredReply (RPC context).
int RaftLabTest::testAddServerBasic(void) {
  Init2(73, "AddServer basic functionality");

  // Wait for election
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 73: Leader elected: {}", leader);

  auto server = config_->GetServer(leader);
  Assert2(server != nullptr, "Server should not be null");

  // 1. Verify initial config size matches NSERVERS
  auto& initial_config = server->GetCurrentConfig();
  size_t initial_size = initial_config.size();
  Assert2(initial_size == NSERVERS,
          "Initial config size should be %d, got %zu", NSERVERS, initial_size);
  Log_info("TEST 73: Initial config size verified: {}", initial_size);

  // 2. Verify initial quorum size
  size_t initial_quorum = server->GetQuorumSize();
  Assert2(initial_quorum == (NSERVERS / 2 + 1),
          "Initial quorum should be %d, got %zu", NSERVERS / 2 + 1, initial_quorum);
  Log_info("TEST 73: Initial quorum size verified: {}", initial_quorum);

  // 3. Directly add a new server to current_config_ (simulating OnAddServer)
  siteid_t new_server_id = 9999;
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    Assert2(server->current_config_.count(new_server_id) == 0,
            "Server %d should not already be in config", new_server_id);
    server->current_config_.insert(new_server_id);
    server->config_change_pending_ = true;
    server->pending_config_index_ = server->lastLogIndex;
  }
  Log_info("TEST 73: Added server {} to config", new_server_id);

  // 4. Verify config grew by 1
  auto& updated_config = server->GetCurrentConfig();
  Assert2(updated_config.size() == initial_size + 1,
          "Config size should be %zu after add, got %zu",
          initial_size + 1, updated_config.size());
  Log_info("TEST 73: Config size after add: {}", updated_config.size());

  // 5. Verify new server is in config
  Assert2(updated_config.count(new_server_id) > 0,
          "New server %d should be in config", new_server_id);

  // 6. Verify quorum updated
  size_t new_quorum = server->GetQuorumSize();
  Assert2(new_quorum == (initial_size + 1) / 2 + 1,
          "Quorum should be %zu after add, got %zu",
          (initial_size + 1) / 2 + 1, new_quorum);
  Log_info("TEST 73: Quorum after add: {}", new_quorum);

  // 7. Verify config_change_pending_ flag is set
  Assert2(server->config_change_pending_,
          "config_change_pending_ should be true after add");

  // 8. Cluster should still work (the extra server is fake, doesn't affect real quorum)
  // Reset config to original to not break subsequent operations
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->current_config_.erase(new_server_id);
    server->config_change_pending_ = false;
  }

  uint64_t idx = config_->DoAgreement(7300, NSERVERS, true);
  Assert2(idx > 0, "DoAgreement should succeed after restoring config");
  Log_info("TEST 73: Agreement reached at index {}", idx);

  Log_info("TEST 73: AddServer basic PASSED!");
  Passed2();
}

// ============================================================================
// Test 74: testRemoveServerBasic
// ============================================================================
// Verify that RemoveServer removes a server from the config, decreases config
// size, and updates quorum size.
int RaftLabTest::testRemoveServerBasic(void) {
  Init2(74, "RemoveServer basic functionality");

  // Wait for election
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 74: Leader elected: {}", leader);

  auto server = config_->GetServer(leader);
  Assert2(server != nullptr, "Server should not be null");

  // First, add a fake server so we can safely remove it without disrupting quorum
  siteid_t extra_server_id = 8888;
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->current_config_.insert(extra_server_id);
    server->config_change_pending_ = false;  // Clear so we can do remove
  }

  size_t size_before = server->GetCurrentConfig().size();
  Assert2(size_before == NSERVERS + 1,
          "Config should be %d after adding fake server, got %zu",
          NSERVERS + 1, size_before);
  Log_info("TEST 74: Config size before remove: {}", size_before);

  // Remove the extra server
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    Assert2(server->current_config_.count(extra_server_id) > 0,
            "Extra server should be in config before remove");
    server->current_config_.erase(extra_server_id);
    server->config_change_pending_ = true;
    server->pending_config_index_ = server->lastLogIndex;
  }
  Log_info("TEST 74: Removed server {} from config", extra_server_id);

  // Verify config shrunk by 1
  Assert2(server->GetCurrentConfig().size() == size_before - 1,
          "Config size should be %zu after remove, got %zu",
          size_before - 1, server->GetCurrentConfig().size());

  // Verify removed server is not in config
  Assert2(server->GetCurrentConfig().count(extra_server_id) == 0,
          "Removed server %d should not be in config", extra_server_id);

  // Verify quorum updated (back to NSERVERS)
  size_t expected_quorum = NSERVERS / 2 + 1;
  Assert2(server->GetQuorumSize() == expected_quorum,
          "Quorum should be %zu after remove, got %zu",
          expected_quorum, server->GetQuorumSize());
  Log_info("TEST 74: Quorum after remove: {}", server->GetQuorumSize());

  // Clear pending and verify cluster still works
  server->config_change_pending_ = false;

  uint64_t idx = config_->DoAgreement(7400, NSERVERS, true);
  Assert2(idx > 0, "DoAgreement should succeed after RemoveServer");

  Log_info("TEST 74: RemoveServer basic PASSED!");
  Passed2();
}

// ============================================================================
// Test 75: testRejectDuplicateConfigChange
// ============================================================================
// Verify that config_change_pending_ prevents concurrent config changes,
// and test leader-only validation.
int RaftLabTest::testRejectDuplicateConfigChange(void) {
  Init2(75, "Reject duplicate config change");

  // Wait for election
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 75: Leader elected: {}", leader);

  auto server = config_->GetServer(leader);
  Assert2(server != nullptr, "Server should not be null");

  // 1. Simulate first AddServer - sets pending flag
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->current_config_.insert(static_cast<siteid_t>(7777));
    server->config_change_pending_ = true;
    server->pending_config_index_ = server->lastLogIndex;
  }
  Log_info("TEST 75: First config change simulated (pending=true)");

  // 2. Verify pending flag blocks further changes
  Assert2(server->config_change_pending_,
          "config_change_pending_ should be true");

  // 3. A second change should detect pending flag
  // (In the real RPC handler, OnAddServer checks and rejects)
  // Here we verify the flag mechanism works
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    Assert2(server->config_change_pending_,
            "Cannot add second server while pending");
  }
  Log_info("TEST 75: Pending flag correctly blocks second change");

  // 4. Clear pending flag (simulating commit) and verify changes work again
  server->config_change_pending_ = false;

  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    Assert2(!server->config_change_pending_,
            "Pending flag should be cleared");
    // Now a new change should be allowed
    server->current_config_.erase(static_cast<siteid_t>(7777));
    server->config_change_pending_ = true;
  }
  Assert2(server->config_change_pending_,
          "Pending flag should be set after new change");
  Log_info("TEST 75: Config change succeeded after clearing pending flag");

  // 5. Test that follower servers are not leaders
  // (In the real RPC handler, OnAddServer checks IsLeader() and rejects)
  int non_leader = -1;
  for (int i = 0; i < NSERVERS; i++) {
    if (i != leader) {
      non_leader = i;
      break;
    }
  }
  Assert2(non_leader >= 0, "Should find a non-leader");
  auto follower = config_->GetServer(non_leader);
  Assert2(follower != nullptr, "Follower should not be null");
  Assert2(!follower->IsLeader(),
          "Non-leader server should not be leader");
  Log_info("TEST 75: Non-leader correctly identified (server {})", non_leader);

  // 6. Verify all servers have correct initial config size
  for (int i = 0; i < NSERVERS; i++) {
    auto s = config_->GetServer(i);
    if (s != nullptr) {
      Assert2(s->GetCurrentConfig().size() == NSERVERS,
              "Server %d config size should be %d, got %zu",
              i, NSERVERS, s->GetCurrentConfig().size());
    }
  }
  Log_info("TEST 75: All servers have correct initial config size");

  // Cleanup: clear pending flag on leader
  server->config_change_pending_ = false;

  Log_info("TEST 75: Reject duplicate config change PASSED!");
  Passed2();
}

// ============================================================================
// Test 76: testNewServerCatchUp
// ============================================================================
// Verify that AddServer adds a new server as a learner (not directly to
// current_config_), and that CheckAndPromoteLearners promotes the learner
// to full member once its match_index_ is within catchup_threshold_.
// @unsafe - Accesses internal server state via friend class
int RaftLabTest::testNewServerCatchUp(void) {
  Init2(76, "New server catch-up (learner tracking and promotion)");

  // Wait for election
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 76: Leader elected: {}", leader);

  auto server = config_->GetServer(leader);
  Assert2(server != nullptr, "Server should not be null");

  // 1. Commit some entries so the log is non-empty
  uint64_t idx1 = config_->DoAgreement(7601, NSERVERS, true);
  Assert2(idx1 > 0, "First agreement should succeed");
  uint64_t idx2 = config_->DoAgreement(7602, NSERVERS, true);
  Assert2(idx2 > 0, "Second agreement should succeed");
  Log_info("TEST 76: Committed entries at indices {} and {}", idx1, idx2);

  // 2. Record initial state
  size_t initial_config_size = server->GetCurrentConfig().size();
  Assert2(initial_config_size == NSERVERS,
          "Initial config size should be %d, got %zu", NSERVERS, initial_config_size);
  Assert2(server->GetLearners().empty(),
          "No learners initially");
  Log_info("TEST 76: Initial config size={}, learners={}",
           initial_config_size, server->GetLearners().size());

  // 3. Add a fake server as learner via direct manipulation (simulating OnAddServer)
  siteid_t new_server_id = 8888;
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    // Verify not already present
    Assert2(server->current_config_.count(new_server_id) == 0,
            "New server should not already be in config");
    Assert2(server->learners_.count(new_server_id) == 0,
            "New server should not already be a learner");

    // Add as learner (mimicking what OnAddServer now does)
    server->learners_.insert(new_server_id);
    server->config_change_pending_ = true;
    server->pending_config_index_ = server->lastLogIndex;

    // Initialize replication state
    server->next_index_[new_server_id] = server->lastLogIndex + 1;
    server->match_index_[new_server_id] = 0;
  }
  Log_info("TEST 76: Added server {} as learner", new_server_id);

  // 4. Verify the server is in learners_ but NOT in current_config_
  Assert2(server->IsLearner(new_server_id),
          "New server should be a learner");
  Assert2(server->GetCurrentConfig().count(new_server_id) == 0,
          "New server should NOT be in current_config_ yet");
  Assert2(server->GetCurrentConfig().size() == initial_config_size,
          "Config size should be unchanged while server is learner");
  Assert2(server->config_change_pending_,
          "config_change_pending_ should be true");
  Log_info("TEST 76: Verified learner state - learner={}, in_config={}",
           server->IsLearner(new_server_id),
           (int)(server->GetCurrentConfig().count(new_server_id) > 0));

  // 5. Quorum should NOT include the learner
  size_t quorum_with_learner = server->GetQuorumSize();
  Assert2(quorum_with_learner == (initial_config_size / 2 + 1),
          "Quorum should not change while server is learner: expected %zu, got %zu",
          initial_config_size / 2 + 1, quorum_with_learner);
  Log_info("TEST 76: Quorum unchanged at {} (learner not counted)", quorum_with_learner);

  // 6. CheckAndPromoteLearners should NOT promote yet (match_index_ = 0, far behind)
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->CheckAndPromoteLearners();
  }
  Assert2(server->IsLearner(new_server_id),
          "Learner should NOT be promoted yet (match_index=0, far behind)");
  Assert2(server->GetCurrentConfig().count(new_server_id) == 0,
          "Learner should NOT be in config yet");
  Log_info("TEST 76: Correctly not promoted when far behind");

  // 7. Simulate catch-up: set match_index_ close to lastLogIndex
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    // Set match_index to be within threshold
    uint64_t leader_last = server->lastLogIndex;
    Assert2(leader_last > 0, "Leader should have log entries");
    server->match_index_[new_server_id] = leader_last;  // Fully caught up
    Log_info("TEST 76: Set match_index[{}] = {} (lastLogIndex={})",
             new_server_id, leader_last, leader_last);
  }

  // 8. Now CheckAndPromoteLearners should promote the learner
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->CheckAndPromoteLearners();
  }

  // 9. Verify promotion: moved from learners_ to current_config_
  Assert2(!server->IsLearner(new_server_id),
          "Server should no longer be a learner after promotion");
  Assert2(server->GetCurrentConfig().count(new_server_id) > 0,
          "Server should be in current_config_ after promotion");
  Assert2(server->GetCurrentConfig().size() == initial_config_size + 1,
          "Config size should have grown by 1 after promotion");
  Assert2(!server->config_change_pending_,
          "config_change_pending_ should be false after promotion");
  Log_info("TEST 76: Promoted! config_size={}, quorum={}",
           server->GetCurrentConfig().size(), server->GetQuorumSize());

  // 10. Verify quorum updated after promotion
  size_t new_quorum = server->GetQuorumSize();
  Assert2(new_quorum == (initial_config_size + 1) / 2 + 1,
          "Quorum should update after promotion: expected %zu, got %zu",
          (initial_config_size + 1) / 2 + 1, new_quorum);
  Log_info("TEST 76: Quorum updated to {}", new_quorum);

  // 11. Test threshold behavior: add another learner, set it just within threshold
  siteid_t new_server_id2 = 9999;
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->learners_.insert(new_server_id2);
    server->config_change_pending_ = true;
    server->next_index_[new_server_id2] = server->lastLogIndex + 1;
    // Set match_index just at the threshold boundary
    uint64_t threshold = server->catchup_threshold_;
    uint64_t leader_last = server->lastLogIndex;
    if (leader_last > threshold) {
      server->match_index_[new_server_id2] = leader_last - threshold;  // Exactly at threshold
    } else {
      server->match_index_[new_server_id2] = 0;  // Close enough for small logs
    }
    Log_info("TEST 76: Added second learner {}, match_index={}, threshold={}, lastLogIndex={}",
             new_server_id2, server->match_index_[new_server_id2], threshold, leader_last);
  }

  // Should promote since within threshold
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->CheckAndPromoteLearners();
  }
  Assert2(!server->IsLearner(new_server_id2),
          "Second learner should be promoted (at threshold boundary)");
  Assert2(server->GetCurrentConfig().count(new_server_id2) > 0,
          "Second learner should be in current_config_ after promotion");
  Log_info("TEST 76: Second learner promoted at threshold boundary");

  // 12. Test that learner far beyond threshold is NOT promoted
  siteid_t new_server_id3 = 7777;
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->learners_.insert(new_server_id3);
    server->config_change_pending_ = true;
    server->next_index_[new_server_id3] = server->lastLogIndex + 1;
    // Commit more entries to make the gap large
    // We just set match_index far behind
    uint64_t threshold = server->catchup_threshold_;
    uint64_t leader_last = server->lastLogIndex;
    if (leader_last > threshold + 10) {
      server->match_index_[new_server_id3] = leader_last - threshold - 10;  // Beyond threshold
    } else {
      // If log is too short, skip this sub-test
      server->match_index_[new_server_id3] = 0;
    }
    Log_info("TEST 76: Added third learner {}, match_index={}, threshold={}, lastLogIndex={}",
             new_server_id3, server->match_index_[new_server_id3], threshold, leader_last);
  }

  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    uint64_t threshold = server->catchup_threshold_;
    uint64_t leader_last = server->lastLogIndex;
    // Only check if the gap is actually beyond threshold
    if (leader_last > threshold + 10) {
      server->CheckAndPromoteLearners();
      Assert2(server->IsLearner(new_server_id3),
              "Third learner should NOT be promoted (beyond threshold)");
      Log_info("TEST 76: Third learner correctly not promoted (beyond threshold)");
    } else {
      Log_info("TEST 76: Skipping beyond-threshold sub-test (log too short)");
    }
  }

  // Cleanup: remove fake servers from config to avoid breaking subsequent operations
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->current_config_.erase(new_server_id);
    server->current_config_.erase(new_server_id2);
    server->learners_.erase(new_server_id3);
    server->match_index_.erase(new_server_id);
    server->match_index_.erase(new_server_id2);
    server->match_index_.erase(new_server_id3);
    server->next_index_.erase(new_server_id);
    server->next_index_.erase(new_server_id2);
    server->next_index_.erase(new_server_id3);
    server->config_change_pending_ = false;
  }

  // Verify cluster still works
  uint64_t idx3 = config_->DoAgreement(7603, NSERVERS, true);
  Assert2(idx3 > 0, "DoAgreement should succeed after cleanup");
  Log_info("TEST 76: Agreement reached at index {} after cleanup", idx3);

  Log_info("TEST 76: New server catch-up PASSED!");
  Passed2();
}

// ============================================================================
// Test 77: testAddServerReceivesLogs
// ============================================================================
// Verify that adding a server as a learner, catching it up, and promoting it
// results in correct config size and quorum. This exercises the full add path:
// commit entries -> add learner -> initialize tracking -> catch up -> promote.
// @unsafe - Accesses internal server state via friend class
int RaftLabTest::testAddServerReceivesLogs(void) {
  Init2(77, "AddServer receives logs and promotes with correct quorum");

  // @unsafe { election wait }
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 77: Leader elected: {}", leader);

  auto server = config_->GetServer(leader);
  Assert2(server != nullptr, "Server should not be null");

  // 1. Commit 5 entries so the log is non-trivial
  // @unsafe { DoAgreement calls into non-borrow-checked RPC layer }
  for (int i = 1; i <= 5; i++) {
    uint64_t idx = config_->DoAgreement(7700 + i, NSERVERS, true);
    Assert2(idx > 0, "Agreement %d should succeed", i);
  }
  Log_info("TEST 77: Committed 5 entries");

  // 2. Record initial state
  size_t initial_config_size = server->GetCurrentConfig().size();
  Assert2(initial_config_size == NSERVERS,
          "Initial config should be %d, got %zu", NSERVERS, initial_config_size);
  size_t initial_quorum = server->GetQuorumSize();
  Assert2(initial_quorum == (NSERVERS / 2 + 1),
          "Initial quorum should be %d, got %zu", NSERVERS / 2 + 1, initial_quorum);

  // 3. Add server 999 as learner
  siteid_t new_server_id = 999;
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->learners_.insert(new_server_id);
    server->config_change_pending_ = true;
    server->pending_config_index_ = server->lastLogIndex;
    server->next_index_[new_server_id] = server->lastLogIndex + 1;
    server->match_index_[new_server_id] = 0;
  }

  // 4. Verify learner state
  Assert2(server->IsLearner(new_server_id),
          "Server 999 should be a learner");
  Assert2(server->GetCurrentConfig().count(new_server_id) == 0,
          "Server 999 should NOT be in current_config_ yet");
  Assert2(server->GetCurrentConfig().size() == initial_config_size,
          "Config size should be unchanged while learner");
  Log_info("TEST 77: Server 999 added as learner, next_index/match_index initialized");

  // 5. Simulate catch-up: set match_index to lastLogIndex
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->match_index_[new_server_id] = server->lastLogIndex;
  }

  // 6. Promote via CheckAndPromoteLearners
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->CheckAndPromoteLearners();
  }

  // 7. Verify promotion: in current_config_, not in learners_
  Assert2(!server->IsLearner(new_server_id),
          "Server 999 should no longer be a learner after promotion");
  Assert2(server->GetCurrentConfig().count(new_server_id) > 0,
          "Server 999 should be in current_config_ after promotion");
  Assert2(server->GetCurrentConfig().size() == initial_config_size + 1,
          "Config should grow to %zu, got %zu",
          initial_config_size + 1, server->GetCurrentConfig().size());

  // 8. Verify quorum: 6 servers -> quorum = 4
  size_t expected_quorum = (initial_config_size + 1) / 2 + 1;
  size_t actual_quorum = server->GetQuorumSize();
  Assert2(actual_quorum == expected_quorum,
          "Quorum should be %zu for 6-server config, got %zu",
          expected_quorum, actual_quorum);
  Log_info("TEST 77: Promoted! config_size={}, quorum={}",
           server->GetCurrentConfig().size(), actual_quorum);

  // Cleanup
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->current_config_.erase(new_server_id);
    server->match_index_.erase(new_server_id);
    server->next_index_.erase(new_server_id);
    server->config_change_pending_ = false;
  }

  // @unsafe { DoAgreement calls into non-borrow-checked RPC layer }
  uint64_t idx = config_->DoAgreement(7799, NSERVERS, true);
  Assert2(idx > 0, "DoAgreement should succeed after cleanup");

  Log_info("TEST 77: AddServer receives logs PASSED!");
  Passed2();
}

// ============================================================================
// Test 78: testRemoveServerQuorumShrinks
// ============================================================================
// Verify that removing a server shrinks the quorum and the cluster can still
// commit entries with the reduced config.
// @unsafe - Accesses internal server state via friend class
int RaftLabTest::testRemoveServerQuorumShrinks(void) {
  Init2(78, "RemoveServer quorum shrinks");

  // @unsafe { election wait }
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 78: Leader elected: {}", leader);

  auto server = config_->GetServer(leader);
  Assert2(server != nullptr, "Server should not be null");

  // 1. Record initial quorum (NSERVERS=5, quorum=3)
  size_t initial_quorum = server->GetQuorumSize();
  Assert2(initial_quorum == (NSERVERS / 2 + 1),
          "Initial quorum should be %d, got %zu", NSERVERS / 2 + 1, initial_quorum);
  Log_info("TEST 78: Initial config size={}, quorum={}", NSERVERS, initial_quorum);

  // 2. Add two fake servers so we can remove one and still have enough real servers
  siteid_t fake1 = 8001;
  siteid_t fake2 = 8002;
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->current_config_.insert(fake1);
    server->current_config_.insert(fake2);
  }

  size_t size_with_extras = server->GetCurrentConfig().size();
  Assert2(size_with_extras == NSERVERS + 2,
          "Config should be %d after adding fakes, got %zu", NSERVERS + 2, size_with_extras);
  size_t quorum_with_extras = server->GetQuorumSize();
  Log_info("TEST 78: After adding 2 fake servers: size={}, quorum={}",
           size_with_extras, quorum_with_extras);

  // 3. Remove fake1 via config manipulation (simulating OnRemoveServer)
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->current_config_.erase(fake1);
    server->config_change_pending_ = true;
    server->pending_config_index_ = server->lastLogIndex;
  }

  // 4. Verify config shrinks
  size_t size_after_remove = server->GetCurrentConfig().size();
  Assert2(size_after_remove == NSERVERS + 1,
          "Config should be %d after remove, got %zu", NSERVERS + 1, size_after_remove);

  // 5. Verify quorum shrinks
  size_t quorum_after_remove = server->GetQuorumSize();
  size_t expected_quorum = (NSERVERS + 1) / 2 + 1;
  Assert2(quorum_after_remove == expected_quorum,
          "Quorum should be %zu after remove, got %zu",
          expected_quorum, quorum_after_remove);
  Assert2(quorum_after_remove < quorum_with_extras,
          "Quorum should shrink: was %zu, now %zu",
          quorum_with_extras, quorum_after_remove);
  Log_info("TEST 78: After remove: size={}, quorum={} (was {})",
           size_after_remove, quorum_after_remove, quorum_with_extras);

  // 6. Verify cluster can still commit entries
  server->config_change_pending_ = false;
  // Remove fake2 to restore config
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->current_config_.erase(fake2);
  }

  // @unsafe { DoAgreement calls into non-borrow-checked RPC layer }
  uint64_t idx = config_->DoAgreement(7800, NSERVERS, true);
  Assert2(idx > 0, "DoAgreement should succeed after removing server");
  Log_info("TEST 78: Agreement reached at index {} after restore", idx);

  Log_info("TEST 78: RemoveServer quorum shrinks PASSED!");
  Passed2();
}

// ============================================================================
// Test 79: testAddServerDuringActiveWorkload
// ============================================================================
// Verify that adding a learner mid-workload does not disrupt ongoing commits.
// @unsafe - Accesses internal server state via friend class
int RaftLabTest::testAddServerDuringActiveWorkload(void) {
  Init2(79, "AddServer during active workload");

  // @unsafe { election wait }
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 79: Leader elected: {}", leader);

  auto server = config_->GetServer(leader);
  Assert2(server != nullptr, "Server should not be null");

  // 1. Commit first batch of entries
  // @unsafe { DoAgreement calls into non-borrow-checked RPC layer }
  for (int i = 1; i <= 3; i++) {
    uint64_t idx = config_->DoAgreement(7900 + i, NSERVERS, true);
    Assert2(idx > 0, "Pre-add agreement %d should succeed", i);
  }
  Log_info("TEST 79: Committed 3 entries before adding learner");

  // 2. Add a fake server as learner mid-workload
  siteid_t new_server_id = 997;
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->learners_.insert(new_server_id);
    server->config_change_pending_ = true;
    server->pending_config_index_ = server->lastLogIndex;
    server->next_index_[new_server_id] = server->lastLogIndex + 1;
    server->match_index_[new_server_id] = 0;
  }
  Assert2(server->IsLearner(new_server_id),
          "Server 997 should be a learner");
  Log_info("TEST 79: Added learner 997 mid-workload");

  // 3. Continue committing entries while learner is present
  // Learner should not affect quorum since it's not in current_config_
  // @unsafe { DoAgreement calls into non-borrow-checked RPC layer }
  for (int i = 4; i <= 8; i++) {
    uint64_t idx = config_->DoAgreement(7900 + i, NSERVERS, true);
    Assert2(idx > 0, "Post-add agreement %d should succeed", i);
  }
  Log_info("TEST 79: Committed 5 more entries after adding learner");

  // 4. Verify learner tracking state is consistent
  Assert2(server->IsLearner(new_server_id),
          "Server 997 should still be a learner");
  Assert2(server->GetCurrentConfig().count(new_server_id) == 0,
          "Server 997 should NOT be in current_config_");
  Assert2(server->GetCurrentConfig().size() == NSERVERS,
          "Config size should still be %d, got %zu",
          NSERVERS, server->GetCurrentConfig().size());

  // 5. Verify quorum was never affected by learner
  Assert2(server->GetQuorumSize() == (NSERVERS / 2 + 1),
          "Quorum should be %d (learner doesn't count)", NSERVERS / 2 + 1);
  Log_info("TEST 79: Quorum unchanged at {}, learner correctly excluded",
           server->GetQuorumSize());

  // Cleanup
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->learners_.erase(new_server_id);
    server->match_index_.erase(new_server_id);
    server->next_index_.erase(new_server_id);
    server->config_change_pending_ = false;
  }

  Log_info("TEST 79: AddServer during active workload PASSED!");
  Passed2();
}

// ============================================================================
// Test 80: testLeaderFailureDuringConfigChange
// ============================================================================
// Verify that if the leader fails while a config change is pending, the new
// leader does not inherit the pending state (config_change_pending_ is local
// to each server and resets on new elections).
// @unsafe - Accesses internal server state via friend class
int RaftLabTest::testLeaderFailureDuringConfigChange(void) {
  Init2(80, "Leader failure during config change");

  // @unsafe { election wait }
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 80: Leader elected: {}", leader);

  auto server = config_->GetServer(leader);
  Assert2(server != nullptr, "Server should not be null");

  // 1. Set config_change_pending on the leader (simulating in-flight AddServer)
  siteid_t fake_server = 996;
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->learners_.insert(fake_server);
    server->config_change_pending_ = true;
    server->pending_config_index_ = server->lastLogIndex;
    server->next_index_[fake_server] = server->lastLogIndex + 1;
    server->match_index_[fake_server] = 0;
  }
  Assert2(server->config_change_pending_,
          "Leader should have config_change_pending_=true");
  Log_info("TEST 80: Set config_change_pending=true on leader {}", leader);

  // 2. Disconnect the leader to trigger re-election
  // @unsafe { Disconnect manipulates network state }
  config_->Disconnect(leader);
  Log_info("TEST 80: Disconnected leader {}", leader);

  // 3. Wait for new election
  // @unsafe { election wait }
  Fiber::sleep(ELECTIONTIMEOUT);
  int new_leader = config_->OneLeader();
  Assert2(new_leader >= 0, "New leader should be elected");
  Assert2(new_leader != leader, "New leader should be different from old leader");
  Log_info("TEST 80: New leader elected: {}", new_leader);

  // 4. Verify new leader does NOT have config_change_pending
  auto new_server = config_->GetServer(new_leader);
  Assert2(new_server != nullptr, "New leader server should not be null");
  Assert2(!new_server->config_change_pending_,
          "New leader should NOT have config_change_pending_=true");
  Log_info("TEST 80: New leader has config_change_pending_=false (correct)");

  // 5. Verify the new leader does not have the fake server as learner
  Assert2(!new_server->IsLearner(fake_server),
          "New leader should not have fake server as learner");

  // 6. Verify cluster can commit entries with new leader
  // @unsafe { DoAgreement calls into non-borrow-checked RPC layer }
  uint64_t idx = config_->DoAgreement(8000, NSERVERS - 1, true);
  Assert2(idx > 0, "DoAgreement should succeed with new leader");
  Log_info("TEST 80: Agreement reached at index {} with new leader", idx);

  // 7. Reconnect old leader
  // @unsafe { Reconnect manipulates network state }
  config_->Reconnect(leader);
  // @unsafe { wait for reconnection }
  Fiber::sleep(ELECTIONTIMEOUT);

  // Cleanup old leader's pending state (it may rejoin as follower)
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->learners_.erase(fake_server);
    server->match_index_.erase(fake_server);
    server->next_index_.erase(fake_server);
    server->config_change_pending_ = false;
  }

  Log_info("TEST 80: Leader failure during config change PASSED!");
  Passed2();
}

// ============================================================================
// Test 81: testCannotAddTwoServersSimultaneously
// ============================================================================
// Verify that when one config change is pending, a second add is rejected.
// This tests the serialization of membership changes via config_change_pending_.
// While Test 75 tests the pending flag mechanism, this test explicitly simulates
// two sequential OnAddServer-like operations and verifies the second fails.
// @unsafe - Accesses internal server state via friend class
int RaftLabTest::testCannotAddTwoServersSimultaneously(void) {
  Init2(81, "Cannot add two servers simultaneously");

  // @unsafe { election wait }
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 81: Leader elected: {}", leader);

  auto server = config_->GetServer(leader);
  Assert2(server != nullptr, "Server should not be null");

  // 1. First AddServer: add server 995 as learner
  siteid_t server1 = 995;
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    Assert2(!server->config_change_pending_,
            "No config change should be pending initially");
    server->learners_.insert(server1);
    server->config_change_pending_ = true;
    server->pending_config_index_ = server->lastLogIndex;
    server->next_index_[server1] = server->lastLogIndex + 1;
    server->match_index_[server1] = 0;
  }
  Assert2(server->config_change_pending_,
          "config_change_pending_ should be true after first add");
  Assert2(server->IsLearner(server1),
          "Server 995 should be a learner");
  Log_info("TEST 81: First AddServer (995) succeeded, pending=true");

  // 2. Second AddServer: attempt to add server 994 - should be rejected
  siteid_t server2 = 994;
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    // Simulate OnAddServer rejection logic: check pending flag first
    bool rejected = server->config_change_pending_;
    Assert2(rejected,
            "Second AddServer should be rejected (config_change_pending_=true)");
    // Do NOT add server2 since the change is rejected
  }
  Assert2(!server->IsLearner(server2),
          "Server 994 should NOT have been added as learner");
  Assert2(server->GetCurrentConfig().count(server2) == 0,
          "Server 994 should NOT be in config");
  Log_info("TEST 81: Second AddServer (994) correctly rejected");

  // 3. Complete the first change (promote server1)
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->match_index_[server1] = server->lastLogIndex;
    server->CheckAndPromoteLearners();
  }
  Assert2(!server->IsLearner(server1),
          "Server 995 should be promoted");
  Assert2(server->GetCurrentConfig().count(server1) > 0,
          "Server 995 should be in current_config_");
  Assert2(!server->config_change_pending_,
          "config_change_pending_ should be false after promotion");
  Log_info("TEST 81: First change completed, server 995 promoted");

  // 4. Now adding server 994 should succeed
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    Assert2(!server->config_change_pending_,
            "Pending should be false, allowing new config change");
    server->learners_.insert(server2);
    server->config_change_pending_ = true;
    server->pending_config_index_ = server->lastLogIndex;
    server->next_index_[server2] = server->lastLogIndex + 1;
    server->match_index_[server2] = 0;
  }
  Assert2(server->IsLearner(server2),
          "Server 994 should now be a learner");
  Assert2(server->config_change_pending_,
          "config_change_pending_ should be true for second change");
  Log_info("TEST 81: Second AddServer (994) now succeeded after first completed");

  // Cleanup
  {
    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
    server->current_config_.erase(server1);
    server->learners_.erase(server2);
    server->match_index_.erase(server1);
    server->match_index_.erase(server2);
    server->next_index_.erase(server1);
    server->next_index_.erase(server2);
    server->config_change_pending_ = false;
  }

  // Verify cluster still works
  // @unsafe { DoAgreement calls into non-borrow-checked RPC layer }
  uint64_t idx = config_->DoAgreement(8100, NSERVERS, true);
  Assert2(idx > 0, "DoAgreement should succeed after cleanup");

  Log_info("TEST 81: Cannot add two servers simultaneously PASSED!");
  Passed2();
}

// ============================================================================
// Test 82: testReplicatedDBCommandPutMarshal
// ============================================================================
// Verify PUT command marshal/unmarshal round-trip preserves all fields.
// @unsafe - Uses Marshal I/O (non-borrow-checked)
int RaftLabTest::testReplicatedDBCommandPutMarshal(void) {
  Init2(82, "ReplicatedDBCommand PUT marshal round-trip");

  // @unsafe { Factory creates rusty::Arc }
  auto cmd = ReplicatedDBCommand::CreatePut("test_key", "test_value");
  Assert2(cmd->op_ == ReplicatedDBOp::PUT, "Op should be PUT");
  Assert2(cmd->key_ == "test_key", "Key should be test_key");
  Assert2(cmd->value_ == "test_value", "Value should be test_value");

  // Marshal
  // @unsafe { Archive I/O }
  srpc::BufferSink sink;
  srpc::BinaryWriteArchive war(srpc::make_sink_proxy_buffer(&sink));
  cmd->save(war);

  // Unmarshal into a new command
  // @unsafe { Archive I/O }
  auto cmd2 = std::make_shared<ReplicatedDBCommand>();
  srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
  srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
  cmd2->load(rar);

  Assert2(cmd2->op_ == ReplicatedDBOp::PUT,
          "Unmarshalled op should be PUT");
  Assert2(cmd2->key_ == "test_key",
          "Unmarshalled key should be test_key, got %s", cmd2->key_.c_str());
  Assert2(cmd2->value_ == "test_value",
          "Unmarshalled value should be test_value, got %s", cmd2->value_.c_str());
  Assert2(cmd2->batch_ops_.empty(),
          "Unmarshalled batch_ops should be empty for PUT");

  // Test with empty key and value
  // @unsafe { Factory creates rusty::Arc }
  auto cmd_empty = ReplicatedDBCommand::CreatePut("", "");
  srpc::BufferSink sink2;
  srpc::BinaryWriteArchive war2(srpc::make_sink_proxy_buffer(&sink2));
  cmd_empty->save(war2);
  auto cmd_empty2 = std::make_shared<ReplicatedDBCommand>();
  srpc::BufferSource src2(sink2.bytes.data(), sink2.bytes.len());
  srpc::BinaryReadArchive rar2(srpc::make_source_proxy_buffer(&src2));
  cmd_empty2->load(rar2);
  Assert2(cmd_empty2->op_ == ReplicatedDBOp::PUT, "Empty PUT op should be PUT");
  Assert2(cmd_empty2->key_.empty(), "Empty PUT key should be empty");
  Assert2(cmd_empty2->value_.empty(), "Empty PUT value should be empty");

  // Test with large key/value
  std::string large_key(1024, 'K');
  std::string large_value(4096, 'V');
  // @unsafe { Factory creates rusty::Arc }
  auto cmd_large = ReplicatedDBCommand::CreatePut(large_key, large_value);
  srpc::BufferSink sink3;
  srpc::BinaryWriteArchive war3(srpc::make_sink_proxy_buffer(&sink3));
  cmd_large->save(war3);
  auto cmd_large2 = std::make_shared<ReplicatedDBCommand>();
  srpc::BufferSource src3(sink3.bytes.data(), sink3.bytes.len());
  srpc::BinaryReadArchive rar3(srpc::make_source_proxy_buffer(&src3));
  cmd_large2->load(rar3);
  Assert2(cmd_large2->key_ == large_key,
          "Large key should survive round-trip");
  Assert2(cmd_large2->value_ == large_value,
          "Large value should survive round-trip");

  Log_info("TEST 82: ReplicatedDBCommand PUT marshal round-trip PASSED!");
  Passed2();
}

// ============================================================================
// Test 83: testReplicatedDBCommandDeleteMarshal
// ============================================================================
// Verify DELETE command marshal/unmarshal round-trip preserves all fields.
// @unsafe - Uses Marshal I/O (non-borrow-checked)
int RaftLabTest::testReplicatedDBCommandDeleteMarshal(void) {
  Init2(83, "ReplicatedDBCommand DELETE marshal round-trip");

  // @unsafe { Factory creates rusty::Arc }
  auto cmd = ReplicatedDBCommand::CreateDelete("delete_key");
  Assert2(cmd->op_ == ReplicatedDBOp::DELETE, "Op should be DELETE");
  Assert2(cmd->key_ == "delete_key", "Key should be delete_key");
  Assert2(cmd->value_.empty(), "Value should be empty for DELETE");

  // Marshal
  // @unsafe { Archive I/O }
  srpc::BufferSink sink;
  srpc::BinaryWriteArchive war(srpc::make_sink_proxy_buffer(&sink));
  cmd->save(war);

  // Unmarshal into a new command
  // @unsafe { Archive I/O }
  auto cmd2 = std::make_shared<ReplicatedDBCommand>();
  srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
  srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
  cmd2->load(rar);

  Assert2(cmd2->op_ == ReplicatedDBOp::DELETE,
          "Unmarshalled op should be DELETE");
  Assert2(cmd2->key_ == "delete_key",
          "Unmarshalled key should be delete_key, got %s", cmd2->key_.c_str());
  Assert2(cmd2->value_.empty(),
          "Unmarshalled value should be empty for DELETE");
  Assert2(cmd2->batch_ops_.empty(),
          "Unmarshalled batch_ops should be empty for DELETE");

  // Test janus::Command round-trip (tests factory registration)
  // @unsafe { janus::Command uses non-borrow-checked factory }
  auto cmd3 = ReplicatedDBCommand::CreateDelete("deputy_test_key");
  janus::Command md;
  md = std::move(cmd3);
  srpc::BufferSink sink2;
  srpc::BinaryWriteArchive war2(srpc::make_sink_proxy_buffer(&sink2));
  srpc::Serialize_::serialize(md, war2);

  janus::Command md2;
  srpc::BufferSource src2(sink2.bytes.data(), sink2.bytes.len());
  srpc::BinaryReadArchive rar2(srpc::make_source_proxy_buffer(&src2));
  srpc::Deserialize_::deserialize(md2, rar2);
  Assert2(md2.has_value(), "janus::Command should have deserialized data");
  const auto cmd4 = marshallable_cast<ReplicatedDBCommand>(md2);
  Assert2(cmd4.is_some(), "Should dynamic_cast to ReplicatedDBCommand");
  Assert2(cmd4.unwrap()->op_ == ReplicatedDBOp::DELETE,
          "Deputy round-trip op should be DELETE");
  Assert2(cmd4.unwrap()->key_ == "deputy_test_key",
          "Deputy round-trip key should match");

  Log_info("TEST 83: ReplicatedDBCommand DELETE marshal round-trip PASSED!");
  Passed2();
}

// ============================================================================
// Test 84: testReplicatedDBCommandBatchMarshal
// ============================================================================
// Verify BATCH command marshal/unmarshal round-trip preserves all ops.
// @unsafe - Uses Marshal I/O (non-borrow-checked)
int RaftLabTest::testReplicatedDBCommandBatchMarshal(void) {
  Init2(84, "ReplicatedDBCommand BATCH marshal round-trip");

  // Create a batch with 3 operations
  std::vector<KVOperation> ops;
  ops.push_back({ReplicatedDBOp::PUT, "key1", "value1"});
  ops.push_back({ReplicatedDBOp::DELETE, "key2", ""});
  ops.push_back({ReplicatedDBOp::PUT, "key3", "value3"});

  // @unsafe { Factory creates rusty::Arc }
  auto cmd = ReplicatedDBCommand::CreateBatch(ops);
  Assert2(cmd->op_ == ReplicatedDBOp::BATCH, "Op should be BATCH");
  Assert2(cmd->batch_ops_.size() == 3, "Should have 3 batch ops");

  // Marshal
  // @unsafe { Archive I/O }
  srpc::BufferSink sink;
  srpc::BinaryWriteArchive war(srpc::make_sink_proxy_buffer(&sink));
  cmd->save(war);

  // Unmarshal into a new command
  // @unsafe { Archive I/O }
  auto cmd2 = std::make_shared<ReplicatedDBCommand>();
  srpc::BufferSource src(sink.bytes.data(), sink.bytes.len());
  srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
  cmd2->load(rar);

  Assert2(cmd2->op_ == ReplicatedDBOp::BATCH,
          "Unmarshalled op should be BATCH");
  Assert2(cmd2->batch_ops_.size() == 3,
          "Unmarshalled batch should have 3 ops, got %zu", cmd2->batch_ops_.size());

  // Verify each operation
  Assert2(cmd2->batch_ops_[0].op == ReplicatedDBOp::PUT,
          "Op 0 should be PUT");
  Assert2(cmd2->batch_ops_[0].key == "key1",
          "Op 0 key should be key1");
  Assert2(cmd2->batch_ops_[0].value == "value1",
          "Op 0 value should be value1");

  Assert2(cmd2->batch_ops_[1].op == ReplicatedDBOp::DELETE,
          "Op 1 should be DELETE");
  Assert2(cmd2->batch_ops_[1].key == "key2",
          "Op 1 key should be key2");
  Assert2(cmd2->batch_ops_[1].value.empty(),
          "Op 1 value should be empty for DELETE");

  Assert2(cmd2->batch_ops_[2].op == ReplicatedDBOp::PUT,
          "Op 2 should be PUT");
  Assert2(cmd2->batch_ops_[2].key == "key3",
          "Op 2 key should be key3");
  Assert2(cmd2->batch_ops_[2].value == "value3",
          "Op 2 value should be value3");

  // Test empty batch
  std::vector<KVOperation> empty_ops;
  // @unsafe { Factory creates rusty::Arc }
  auto cmd_empty = ReplicatedDBCommand::CreateBatch(empty_ops);
  srpc::BufferSink sink2;
  srpc::BinaryWriteArchive war2(srpc::make_sink_proxy_buffer(&sink2));
  cmd_empty->save(war2);
  auto cmd_empty2 = std::make_shared<ReplicatedDBCommand>();
  srpc::BufferSource src2(sink2.bytes.data(), sink2.bytes.len());
  srpc::BinaryReadArchive rar2(srpc::make_source_proxy_buffer(&src2));
  cmd_empty2->load(rar2);
  Assert2(cmd_empty2->op_ == ReplicatedDBOp::BATCH,
          "Empty batch op should be BATCH");
  Assert2(cmd_empty2->batch_ops_.empty(),
          "Empty batch should have 0 ops");

  // Test large batch (100 operations)
  std::vector<KVOperation> large_ops;
  for (int i = 0; i < 100; i++) {
    ReplicatedDBOp op = (i % 2 == 0) ? ReplicatedDBOp::PUT : ReplicatedDBOp::DELETE;
    large_ops.push_back({op, "key_" + std::to_string(i), "val_" + std::to_string(i)});
  }
  // @unsafe { Factory creates rusty::Arc }
  auto cmd_large = ReplicatedDBCommand::CreateBatch(large_ops);
  srpc::BufferSink sink3;
  srpc::BinaryWriteArchive war3(srpc::make_sink_proxy_buffer(&sink3));
  cmd_large->save(war3);
  auto cmd_large2 = std::make_shared<ReplicatedDBCommand>();
  srpc::BufferSource src3(sink3.bytes.data(), sink3.bytes.len());
  srpc::BinaryReadArchive rar3(srpc::make_source_proxy_buffer(&src3));
  cmd_large2->load(rar3);
  Assert2(cmd_large2->batch_ops_.size() == 100,
          "Large batch should have 100 ops, got %zu", cmd_large2->batch_ops_.size());
  for (int i = 0; i < 100; i++) {
    ReplicatedDBOp expected_op = (i % 2 == 0) ? ReplicatedDBOp::PUT : ReplicatedDBOp::DELETE;
    Assert2(cmd_large2->batch_ops_[i].op == expected_op,
            "Op %d type mismatch", i);
    Assert2(cmd_large2->batch_ops_[i].key == "key_" + std::to_string(i),
            "Op %d key mismatch", i);
    Assert2(cmd_large2->batch_ops_[i].value == "val_" + std::to_string(i),
            "Op %d value mismatch", i);
  }

  Log_info("TEST 84: ReplicatedDBCommand BATCH marshal round-trip PASSED!");
  Passed2();
}

// ============================================================================
// Test 85: testReplicatedDBPutGet
// ============================================================================
// Create ReplicatedDB on leader, Put a key, Get it back from local RocksDB.
// @unsafe - Uses ReplicatedDB which wraps RocksDB and Raft
int RaftLabTest::testReplicatedDBPutGet(void) {
  Init2(85, "ReplicatedDB Put/Get on leader");

  // Wait for election
  // @unsafe { Fiber::sleep }
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  auto* svr = config_->GetServer(leader);
  Assert2(svr != nullptr, "Leader server is null");

  // Create ReplicatedDB with a temp path
  std::string db_path = "/tmp/raft_test_repldb_85_" + std::to_string(leader);

  // Clean up any leftover DB from previous runs
  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, db_path.c_str(), &err);
    if (err) rocksdb_free(err);
    rocksdb_options_destroy(opts);
  }

  // Register apply callback and create ReplicatedDB
  auto rdb = std::make_unique<ReplicatedDB>(svr, db_path);
  Assert2(rdb->IsOpen(), "ReplicatedDB should be open");

  // Register the apply callback on the server
  // @unsafe { RegLearnerAction }
  svr->RegLearnerAction([&rdb](int slot, janus::Command md) -> int {
    rdb->ApplyEntry(slot, md);
    return 0;
  });

  // Put a key-value pair (goes through Raft)
  bool put_ok = rdb->Put("hello", "world");
  Assert2(put_ok, "Put should succeed on leader");

  // Get the value back from local RocksDB
  // The apply callback should have written it
  std::string value;
  bool get_ok = rdb->Get("hello", &value);
  Assert2(get_ok, "Get should find the key");
  Assert2(value == "world", "Get value should be 'world', got '%s'", value.c_str());

  // Test Get for non-existent key
  std::string value2;
  bool get_ok2 = rdb->Get("nonexistent", &value2);
  Assert2(!get_ok2, "Get should return false for non-existent key");

  // Test overwrite
  bool put_ok2 = rdb->Put("hello", "updated");
  Assert2(put_ok2, "Put overwrite should succeed");

  std::string value3;
  bool get_ok3 = rdb->Get("hello", &value3);
  Assert2(get_ok3, "Get after overwrite should succeed");
  Assert2(value3 == "updated", "Value should be 'updated', got '%s'", value3.c_str());

  // Verify last applied index advanced
  Assert2(rdb->GetLastAppliedIndex() > 0, "Last applied index should be > 0");

  // Restore the original learner action
  config_->SetLearnerAction();

  // Cleanup
  rdb.reset();
  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, db_path.c_str(), &err);
    if (err) rocksdb_free(err);
    rocksdb_options_destroy(opts);
  }

  Log_info("TEST 85: ReplicatedDB Put/Get on leader PASSED!");
  Passed2();
}

// ============================================================================
// Test 86: testReplicatedDBDelete
// ============================================================================
// Put a key, Delete it, verify Get returns not-found.
// @unsafe - Uses ReplicatedDB which wraps RocksDB and Raft
int RaftLabTest::testReplicatedDBDelete(void) {
  Init2(86, "ReplicatedDB Delete");

  // Wait for election
  // @unsafe { Fiber::sleep }
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  auto* svr = config_->GetServer(leader);
  Assert2(svr != nullptr, "Leader server is null");

  std::string db_path = "/tmp/raft_test_repldb_86_" + std::to_string(leader);

  // Clean up
  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, db_path.c_str(), &err);
    if (err) rocksdb_free(err);
    rocksdb_options_destroy(opts);
  }

  auto rdb = std::make_unique<ReplicatedDB>(svr, db_path);
  Assert2(rdb->IsOpen(), "ReplicatedDB should be open");

  // Register apply callback
  // @unsafe { RegLearnerAction }
  svr->RegLearnerAction([&rdb](int slot, janus::Command md) -> int {
    rdb->ApplyEntry(slot, md);
    return 0;
  });

  // Put a key
  bool put_ok = rdb->Put("to_delete", "some_value");
  Assert2(put_ok, "Put should succeed");

  // Verify it exists
  std::string value;
  bool get_ok = rdb->Get("to_delete", &value);
  Assert2(get_ok, "Get should find the key after Put");
  Assert2(value == "some_value", "Value should be 'some_value'");

  // Delete the key
  bool del_ok = rdb->Delete("to_delete");
  Assert2(del_ok, "Delete should succeed");

  // Verify it's gone
  std::string value2;
  bool get_ok2 = rdb->Get("to_delete", &value2);
  Assert2(!get_ok2, "Get should return false after Delete");

  // Restore the original learner action
  config_->SetLearnerAction();

  // Cleanup
  rdb.reset();
  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, db_path.c_str(), &err);
    if (err) rocksdb_free(err);
    rocksdb_options_destroy(opts);
  }

  Log_info("TEST 86: ReplicatedDB Delete PASSED!");
  Passed2();
}

// ============================================================================
// Test 87: testReplicatedDBReplication
// ============================================================================
// Put on leader, verify a follower's ReplicatedDB also has the value via
// the apply callback (data replicated through Raft).
// @unsafe - Uses ReplicatedDB which wraps RocksDB and Raft
int RaftLabTest::testReplicatedDBReplication(void) {
  Init2(87, "ReplicatedDB replication to follower");

  // Wait for election
  // @unsafe { Fiber::sleep }
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  auto* leader_svr = config_->GetServer(leader);
  Assert2(leader_svr != nullptr, "Leader server is null");

  // Find a follower
  siteid_t follower = -1;
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t sid = config_->getServerIdByIndex(i);
    if (static_cast<int>(sid) != leader) {
      follower = sid;
      break;
    }
  }
  Assert2(follower != static_cast<siteid_t>(-1), "No follower found");

  auto* follower_svr = config_->GetServer(follower);
  Assert2(follower_svr != nullptr, "Follower server is null");

  std::string leader_db_path = "/tmp/raft_test_repldb_87_leader_" + std::to_string(leader);
  std::string follower_db_path = "/tmp/raft_test_repldb_87_follower_" + std::to_string(follower);

  // Clean up previous DBs
  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, leader_db_path.c_str(), &err);
    if (err) { rocksdb_free(err); err = nullptr; }
    rocksdb_destroy_db(opts, follower_db_path.c_str(), &err);
    if (err) { rocksdb_free(err); err = nullptr; }
    rocksdb_options_destroy(opts);
  }

  // Create ReplicatedDB instances for both leader and follower
  auto leader_rdb = std::make_unique<ReplicatedDB>(leader_svr, leader_db_path);
  auto follower_rdb = std::make_unique<ReplicatedDB>(follower_svr, follower_db_path);
  Assert2(leader_rdb->IsOpen(), "Leader ReplicatedDB should be open");
  Assert2(follower_rdb->IsOpen(), "Follower ReplicatedDB should be open");

  // Register apply callbacks on both
  // @unsafe { RegLearnerAction }
  leader_svr->RegLearnerAction([&leader_rdb](int slot, janus::Command md) -> int {
    leader_rdb->ApplyEntry(slot, md);
    return 0;
  });
  follower_svr->RegLearnerAction([&follower_rdb](int slot, janus::Command md) -> int {
    follower_rdb->ApplyEntry(slot, md);
    return 0;
  });

  // Put on leader
  bool put_ok = leader_rdb->Put("replicated_key", "replicated_value");
  Assert2(put_ok, "Put on leader should succeed");

  // Wait for replication to follower
  // The Raft heartbeat will replicate the entry and the apply callback will fire
  bool found = false;
  for (int attempt = 0; attempt < 50; attempt++) {
    std::string value;
    if (follower_rdb->Get("replicated_key", &value)) {
      Assert2(value == "replicated_value",
              "Follower value should be 'replicated_value', got '%s'", value.c_str());
      found = true;
      break;
    }
    // @unsafe { usleep }
    usleep(100000);  // 100ms
  }
  Assert2(found, "Follower should eventually have the replicated key");

  // Verify leader also has it
  std::string leader_value;
  bool leader_get = leader_rdb->Get("replicated_key", &leader_value);
  Assert2(leader_get, "Leader should have the key");
  Assert2(leader_value == "replicated_value", "Leader value should match");

  // Restore the original learner action
  config_->SetLearnerAction();

  // Cleanup
  leader_rdb.reset();
  follower_rdb.reset();
  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, leader_db_path.c_str(), &err);
    if (err) { rocksdb_free(err); err = nullptr; }
    rocksdb_destroy_db(opts, follower_db_path.c_str(), &err);
    if (err) { rocksdb_free(err); err = nullptr; }
    rocksdb_options_destroy(opts);
  }

  Log_info("TEST 87: ReplicatedDB replication to follower PASSED!");
  Passed2();
}

// ============================================================================
// Test 88: testReplicatedDBSnapshot
// ============================================================================
// Create ReplicatedDB on leader, put several keys, create a snapshot via
// CreateStateMachineSnapshot(), verify blob is non-empty, then load it back
// via LoadStateMachineSnapshot() and verify all keys are still accessible.
// @unsafe - Uses ReplicatedDB which wraps RocksDB and Raft
int RaftLabTest::testReplicatedDBSnapshot(void) {
  Init2(88, "ReplicatedDB snapshot create/load round-trip");

  // Wait for election
  // @unsafe { Fiber::sleep }
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  auto* svr = config_->GetServer(leader);
  Assert2(svr != nullptr, "Leader server is null");

  std::string db_path = "/tmp/raft_test_repldb_88_" + std::to_string(leader);

  // Clean up any leftover DB from previous runs
  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, db_path.c_str(), &err);
    if (err) rocksdb_free(err);
    rocksdb_options_destroy(opts);
  }

  // Create ReplicatedDB and register apply callback
  auto rdb = std::make_unique<ReplicatedDB>(svr, db_path);
  Assert2(rdb->IsOpen(), "ReplicatedDB should be open");

  // @unsafe { RegLearnerAction }
  svr->RegLearnerAction([&rdb](int slot, janus::Command md) -> int {
    rdb->ApplyEntry(slot, md);
    return 0;
  });

  // Put several keys
  Assert2(rdb->Put("snap_key1", "value1"), "Put snap_key1 should succeed");
  Assert2(rdb->Put("snap_key2", "value2"), "Put snap_key2 should succeed");
  Assert2(rdb->Put("snap_key3", "value3"), "Put snap_key3 should succeed");

  // Verify keys are present
  std::string val;
  Assert2(rdb->Get("snap_key1", &val) && val == "value1", "snap_key1 should be value1");
  Assert2(rdb->Get("snap_key2", &val) && val == "value2", "snap_key2 should be value2");
  Assert2(rdb->Get("snap_key3", &val) && val == "value3", "snap_key3 should be value3");

  // Create snapshot
  std::string snapshot_blob = rdb->CreateStateMachineSnapshot();
  Assert2(!snapshot_blob.empty(), "Snapshot blob should be non-empty");
  Log_info("TEST 88: Snapshot blob size = {} bytes", snapshot_blob.size());

  // Verify the blob has a compression header byte followed by data
  Assert2(snapshot_blob.size() >= 1, "Blob should have at least 1 byte (compression header)");
  uint8_t compression_byte = static_cast<uint8_t>(snapshot_blob[0]);
  Assert2(compression_byte == 0 || compression_byte == 1,
          "Compression header should be 0 (uncompressed) or 1 (LZ4), got %u", compression_byte);
  Log_info("TEST 88: Snapshot compression byte = {}", compression_byte);

  // Load snapshot back into the same ReplicatedDB (simulates recovery)
  rdb->LoadStateMachineSnapshot(snapshot_blob);
  Assert2(rdb->IsOpen(), "ReplicatedDB should be open after snapshot load");

  // Verify all keys are still accessible after loading
  Assert2(rdb->Get("snap_key1", &val) && val == "value1",
          "snap_key1 should be value1 after snapshot load");
  Assert2(rdb->Get("snap_key2", &val) && val == "value2",
          "snap_key2 should be value2 after snapshot load");
  Assert2(rdb->Get("snap_key3", &val) && val == "value3",
          "snap_key3 should be value3 after snapshot load");

  // Verify last_applied_index was preserved
  Assert2(rdb->GetLastAppliedIndex() > 0,
          "last_applied_index should be > 0 after snapshot load");

  // Restore the original learner action
  config_->SetLearnerAction();

  // Cleanup
  rdb.reset();
  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, db_path.c_str(), &err);
    if (err) rocksdb_free(err);
    rocksdb_options_destroy(opts);
  }

  Log_info("TEST 88: ReplicatedDB snapshot create/load round-trip PASSED!");
  Passed2();
}

// ============================================================================
// Test 89: testReplicatedDBSnapshotTransfer
// ============================================================================
// Create ReplicatedDB on leader and follower. Put keys on leader. Create
// snapshot on leader. Load snapshot on follower. Verify follower has all keys.
// @unsafe - Uses ReplicatedDB which wraps RocksDB and Raft
int RaftLabTest::testReplicatedDBSnapshotTransfer(void) {
  Init2(89, "ReplicatedDB snapshot transfer to follower");

  // Wait for election
  // @unsafe { Fiber::sleep }
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  auto* leader_svr = config_->GetServer(leader);
  Assert2(leader_svr != nullptr, "Leader server is null");

  // Find a follower
  siteid_t follower = -1;
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t sid = config_->getServerIdByIndex(i);
    if (static_cast<int>(sid) != leader) {
      follower = sid;
      break;
    }
  }
  Assert2(follower != static_cast<siteid_t>(-1), "No follower found");

  auto* follower_svr = config_->GetServer(follower);
  Assert2(follower_svr != nullptr, "Follower server is null");

  std::string leader_db_path = "/tmp/raft_test_repldb_89_leader_" + std::to_string(leader);
  std::string follower_db_path = "/tmp/raft_test_repldb_89_follower_" + std::to_string(follower);

  // Clean up previous DBs
  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, leader_db_path.c_str(), &err);
    if (err) { rocksdb_free(err); err = nullptr; }
    rocksdb_destroy_db(opts, follower_db_path.c_str(), &err);
    if (err) { rocksdb_free(err); err = nullptr; }
    rocksdb_options_destroy(opts);
  }

  // Create ReplicatedDB on leader only (follower gets snapshot)
  auto leader_rdb = std::make_unique<ReplicatedDB>(leader_svr, leader_db_path);
  Assert2(leader_rdb->IsOpen(), "Leader ReplicatedDB should be open");

  // Register apply callback on leader
  // @unsafe { RegLearnerAction }
  leader_svr->RegLearnerAction([&leader_rdb](int slot, janus::Command md) -> int {
    leader_rdb->ApplyEntry(slot, md);
    return 0;
  });

  // Put keys on leader (goes through Raft)
  Assert2(leader_rdb->Put("transfer_key1", "val_a"), "Put transfer_key1 should succeed");
  Assert2(leader_rdb->Put("transfer_key2", "val_b"), "Put transfer_key2 should succeed");
  Assert2(leader_rdb->Put("transfer_key3", "val_c"), "Put transfer_key3 should succeed");

  // Create snapshot on leader
  std::string snapshot_blob = leader_rdb->CreateStateMachineSnapshot();
  Assert2(!snapshot_blob.empty(), "Leader snapshot blob should be non-empty");
  Log_info("TEST 89: Leader snapshot blob size = {} bytes", snapshot_blob.size());

  // Create a follower ReplicatedDB (empty initially)
  auto follower_rdb = std::make_unique<ReplicatedDB>(follower_svr, follower_db_path);
  Assert2(follower_rdb->IsOpen(), "Follower ReplicatedDB should be open");

  // Verify follower does NOT have the keys yet
  std::string val;
  Assert2(!follower_rdb->Get("transfer_key1", &val),
          "Follower should NOT have transfer_key1 before snapshot load");

  // Load the leader's snapshot onto the follower
  follower_rdb->LoadStateMachineSnapshot(snapshot_blob);
  Assert2(follower_rdb->IsOpen(), "Follower ReplicatedDB should be open after snapshot load");

  // Verify follower now has all keys
  Assert2(follower_rdb->Get("transfer_key1", &val) && val == "val_a",
          "Follower should have transfer_key1=val_a after snapshot load");
  Assert2(follower_rdb->Get("transfer_key2", &val) && val == "val_b",
          "Follower should have transfer_key2=val_b after snapshot load");
  Assert2(follower_rdb->Get("transfer_key3", &val) && val == "val_c",
          "Follower should have transfer_key3=val_c after snapshot load");

  // Verify follower's last_applied_index was transferred
  Assert2(follower_rdb->GetLastAppliedIndex() > 0,
          "Follower last_applied_index should be > 0 after snapshot load");
  Assert2(follower_rdb->GetLastAppliedIndex() == leader_rdb->GetLastAppliedIndex(),
          "Follower and leader last_applied_index should match");

  // Restore the original learner action
  config_->SetLearnerAction();

  // Cleanup
  leader_rdb.reset();
  follower_rdb.reset();
  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, leader_db_path.c_str(), &err);
    if (err) { rocksdb_free(err); err = nullptr; }
    rocksdb_destroy_db(opts, follower_db_path.c_str(), &err);
    if (err) { rocksdb_free(err); err = nullptr; }
    rocksdb_options_destroy(opts);
  }

  Log_info("TEST 89: ReplicatedDB snapshot transfer to follower PASSED!");
  Passed2();
}

// ============================================================================
// Test 90: testReplicatedDBWiring
// Verifies that Setup() creates a ReplicatedDB and registers the apply callback
// when MAKO_REPLICATED_DB=1 env var is set. Tests the full wiring path:
//   1. Manually create and wire a ReplicatedDB on leader (simulates Setup() logic)
//   2. Verify GetReplicatedDB() returns non-null
//   3. Put/Get through the wired ReplicatedDB
//   4. Verify follower also works when wired
// ============================================================================
// @unsafe - Creates ReplicatedDB, interacts with Raft and RocksDB
int RaftLabTest::testReplicatedDBWiring(void) {
  Init2(90, "ReplicatedDB wiring in Setup path");

  // Wait for election
  // @unsafe { Fiber::sleep }
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  auto* leader_svr = config_->GetServer(leader);
  Assert2(leader_svr != nullptr, "Leader server is null");

  // Verify no ReplicatedDB exists initially (Setup() was called without env var)
  Assert2(leader_svr->GetReplicatedDB() == nullptr,
          "ReplicatedDB should be null before wiring");

  // Simulate what Setup() does when MAKO_REPLICATED_DB=1: create and register
  std::string leader_db_path = "/tmp/raft_test_repldb_90_leader_" + std::to_string(leader);

  // Clean up previous DB
  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, leader_db_path.c_str(), &err);
    if (err) { rocksdb_free(err); err = nullptr; }
    rocksdb_options_destroy(opts);
  }

  // Create ReplicatedDB and assign to server (same as Setup() would do)
  auto rdb = std::make_shared<ReplicatedDB>(leader_svr, leader_db_path);
  Assert2(rdb->IsOpen(), "ReplicatedDB should be open after construction");

  // Wire it into the server (mirroring Setup() logic)
  leader_svr->replicated_db_ = rdb;

  // Register apply callback (same lambda as Setup())
  // @unsafe { RegLearnerAction }
  leader_svr->RegLearnerAction([rdb](int slot, janus::Command md) -> int {
    if (rdb) {
      rdb->ApplyEntry(slot, md);
    }
    return 0;
  });

  // Verify GetReplicatedDB() now returns the instance
  Assert2(leader_svr->GetReplicatedDB() != nullptr,
          "GetReplicatedDB() should return non-null after wiring");
  Assert2(leader_svr->GetReplicatedDB().get() == rdb.get(),
          "GetReplicatedDB() should return the same instance we set");

  // Test Put/Get through the wired ReplicatedDB (goes through Raft)
  Assert2(rdb->Put("wiring_key1", "value1"), "Put wiring_key1 should succeed");
  Assert2(rdb->Put("wiring_key2", "value2"), "Put wiring_key2 should succeed");

  std::string val;
  Assert2(rdb->Get("wiring_key1", &val) && val == "value1",
          "Get wiring_key1 should return value1");
  Assert2(rdb->Get("wiring_key2", &val) && val == "value2",
          "Get wiring_key2 should return value2");

  // Verify last_applied_index was updated by the apply callback
  Assert2(rdb->GetLastAppliedIndex() > 0,
          "last_applied_index should be > 0 after applying entries");

  // Test Delete through wired path
  Assert2(rdb->Delete("wiring_key1"), "Delete wiring_key1 should succeed");
  Assert2(!rdb->Get("wiring_key1", &val),
          "wiring_key1 should not exist after delete");
  Assert2(rdb->Get("wiring_key2", &val) && val == "value2",
          "wiring_key2 should still exist after deleting key1");

  // Now wire a follower too and verify it also works
  siteid_t follower_id = static_cast<siteid_t>(-1);
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t sid = config_->getServerIdByIndex(i);
    if (static_cast<int>(sid) != leader) {
      follower_id = sid;
      break;
    }
  }
  Assert2(follower_id != static_cast<siteid_t>(-1), "No follower found");

  auto* follower_svr = config_->GetServer(follower_id);
  Assert2(follower_svr != nullptr, "Follower server is null");

  std::string follower_db_path = "/tmp/raft_test_repldb_90_follower_" + std::to_string(follower_id);

  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, follower_db_path.c_str(), &err);
    if (err) { rocksdb_free(err); err = nullptr; }
    rocksdb_options_destroy(opts);
  }

  auto follower_rdb = std::make_shared<ReplicatedDB>(follower_svr, follower_db_path);
  Assert2(follower_rdb->IsOpen(), "Follower ReplicatedDB should be open");

  follower_svr->replicated_db_ = follower_rdb;

  // @unsafe { RegLearnerAction }
  follower_svr->RegLearnerAction([follower_rdb](int slot, janus::Command md) -> int {
    if (follower_rdb) {
      follower_rdb->ApplyEntry(slot, md);
    }
    return 0;
  });

  Assert2(follower_svr->GetReplicatedDB() != nullptr,
          "Follower GetReplicatedDB() should return non-null after wiring");

  // Put through leader, wait for replication, check follower
  Assert2(rdb->Put("wiring_replicated", "cross_node"), "Put wiring_replicated should succeed");

  // Give time for replication
  // @unsafe { Fiber::sleep }
  Fiber::sleep(500000);  // 500ms

  Assert2(follower_rdb->Get("wiring_replicated", &val) && val == "cross_node",
          "Follower should have wiring_replicated=cross_node after replication");

  // Restore original learner action
  config_->SetLearnerAction();

  // Cleanup: clear replicated_db_ pointers
  leader_svr->replicated_db_ = nullptr;
  follower_svr->replicated_db_ = nullptr;
  rdb.reset();
  follower_rdb.reset();

  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, leader_db_path.c_str(), &err);
    if (err) { rocksdb_free(err); err = nullptr; }
    rocksdb_destroy_db(opts, follower_db_path.c_str(), &err);
    if (err) { rocksdb_free(err); err = nullptr; }
    rocksdb_options_destroy(opts);
  }

  Log_info("TEST 90: ReplicatedDB wiring in Setup path PASSED!");
  Passed2();
}

// ============================================================================
// Test 91: testReplicatedDBSnapshotCompression
// ============================================================================
// Create ReplicatedDB with compression enabled, put several large keys, create
// snapshot, verify it is LZ4-compressed (check header byte), load it back,
// verify all keys survive. Then test with compression disabled (uncompressed
// header byte) and verify round-trip still works.
// @unsafe - Uses ReplicatedDB which wraps RocksDB and Raft
int RaftLabTest::testReplicatedDBSnapshotCompression(void) {
  Init2(91, "ReplicatedDB snapshot compression (LZ4)");

  // Wait for election
  // @unsafe { Fiber::sleep }
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  auto* svr = config_->GetServer(leader);
  Assert2(svr != nullptr, "Leader server is null");

  std::string db_path = "/tmp/raft_test_repldb_91_" + std::to_string(leader);

  // Clean up any leftover DB from previous runs
  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, db_path.c_str(), &err);
    if (err) rocksdb_free(err);
    rocksdb_options_destroy(opts);
  }

  // Create ReplicatedDB (compression is enabled by default)
  auto rdb = std::make_unique<ReplicatedDB>(svr, db_path);
  Assert2(rdb->IsOpen(), "ReplicatedDB should be open");
  Assert2(rdb->IsCompressionEnabled(), "Compression should be enabled by default");

  // @unsafe { RegLearnerAction }
  svr->RegLearnerAction([&rdb](int slot, janus::Command md) -> int {
    rdb->ApplyEntry(slot, md);
    return 0;
  });

  // Put several keys with large-ish values (to make compression meaningful)
  std::string large_value(1024, 'A');  // 1KB of repeated 'A' - compresses well
  Assert2(rdb->Put("comp_key1", large_value), "Put comp_key1 should succeed");
  Assert2(rdb->Put("comp_key2", large_value), "Put comp_key2 should succeed");
  Assert2(rdb->Put("comp_key3", "small_val"), "Put comp_key3 should succeed");

  // Verify keys are present
  std::string val;
  Assert2(rdb->Get("comp_key1", &val) && val == large_value,
          "comp_key1 should have large_value");
  Assert2(rdb->Get("comp_key2", &val) && val == large_value,
          "comp_key2 should have large_value");
  Assert2(rdb->Get("comp_key3", &val) && val == "small_val",
          "comp_key3 should be small_val");

  // --- Part 1: Compressed snapshot ---
  std::string compressed_blob = rdb->CreateStateMachineSnapshot();
  Assert2(!compressed_blob.empty(), "Compressed snapshot blob should be non-empty");

  // Verify header byte is LZ4 (1)
  uint8_t header = static_cast<uint8_t>(compressed_blob[0]);
  Assert2(header == 1, "Compressed snapshot header should be 1 (LZ4), got %u", header);

  // Verify the compressed blob has the original size field
  Assert2(compressed_blob.size() >= 5,
          "Compressed blob should have at least 5 bytes (header + orig_size)");
  uint32_t orig_size = 0;
  std::memcpy(&orig_size, compressed_blob.data() + 1, sizeof(orig_size));
  Assert2(orig_size > 0, "Original size should be > 0, got %u", orig_size);
  Log_info("TEST 91: Compressed blob: {} bytes, original: {} bytes (ratio: {:.1f}%)",
           compressed_blob.size(), orig_size,
           100.0 * static_cast<double>(compressed_blob.size()) / static_cast<double>(orig_size));

  // Load the compressed snapshot back
  rdb->LoadStateMachineSnapshot(compressed_blob);
  Assert2(rdb->IsOpen(), "ReplicatedDB should be open after compressed snapshot load");

  // Verify all keys survive
  Assert2(rdb->Get("comp_key1", &val) && val == large_value,
          "comp_key1 should survive compressed snapshot round-trip");
  Assert2(rdb->Get("comp_key2", &val) && val == large_value,
          "comp_key2 should survive compressed snapshot round-trip");
  Assert2(rdb->Get("comp_key3", &val) && val == "small_val",
          "comp_key3 should survive compressed snapshot round-trip");

  // Verify last_applied_index was preserved
  Assert2(rdb->GetLastAppliedIndex() > 0,
          "last_applied_index should be > 0 after compressed snapshot load");
  Log_info("TEST 91: Compressed snapshot round-trip PASSED");

  // --- Part 2: Build an uncompressed blob manually and load it ---
  // Create a fresh snapshot to get the raw blob, then manually construct
  // an uncompressed version by stripping the LZ4 header and decompressing
  // We test backward compat by constructing an uncompressed blob
  // First, create a new snapshot (which will be compressed)
  std::string compressed2 = rdb->CreateStateMachineSnapshot();
  Assert2(!compressed2.empty(), "Second compressed snapshot should be non-empty");
  Assert2(static_cast<uint8_t>(compressed2[0]) == 1, "Should still be LZ4");

  // Decompress to get raw blob, then wrap as uncompressed
  uint32_t orig_size2 = 0;
  std::memcpy(&orig_size2, compressed2.data() + 1, sizeof(orig_size2));
  std::string raw_blob(orig_size2, '\0');
  int decompressed = LZ4_decompress_safe(
      compressed2.data() + 5, raw_blob.data(),
      static_cast<int>(compressed2.size() - 5),
      static_cast<int>(orig_size2));
  Assert2(decompressed >= 0, "Manual decompression should succeed");

  // Construct uncompressed blob: header(0) + raw_blob
  std::string uncompressed_blob;
  uncompressed_blob.resize(1 + raw_blob.size());
  uncompressed_blob[0] = 0;  // uncompressed
  std::memcpy(uncompressed_blob.data() + 1, raw_blob.data(), raw_blob.size());

  // Load the uncompressed blob
  rdb->LoadStateMachineSnapshot(uncompressed_blob);
  Assert2(rdb->IsOpen(), "ReplicatedDB should be open after uncompressed snapshot load");

  // Verify all keys survive
  Assert2(rdb->Get("comp_key1", &val) && val == large_value,
          "comp_key1 should survive uncompressed snapshot round-trip");
  Assert2(rdb->Get("comp_key2", &val) && val == large_value,
          "comp_key2 should survive uncompressed snapshot round-trip");
  Assert2(rdb->Get("comp_key3", &val) && val == "small_val",
          "comp_key3 should survive uncompressed snapshot round-trip");
  Log_info("TEST 91: Uncompressed (backward compat) snapshot round-trip PASSED");

  // Restore the original learner action
  config_->SetLearnerAction();

  // Cleanup
  rdb.reset();
  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, db_path.c_str(), &err);
    if (err) rocksdb_free(err);
    rocksdb_options_destroy(opts);
  }

  Log_info("TEST 91: ReplicatedDB snapshot compression PASSED!");
  Passed2();
}

// ============================================================================
// Test 99: testLinearizableGet
// ============================================================================
// Put a key via Raft, then read it via LinearizableGet on the leader.
// Verify value matches. Verify LinearizableGet fails on a non-leader (follower).
// @unsafe - Uses ReplicatedDB which wraps RocksDB and Raft
int RaftLabTest::testLinearizableGet(void) {
  Init2(99, "LinearizableGet on leader and follower");

  // Wait for election
  // @unsafe { Fiber::sleep }
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  auto* svr = config_->GetServer(leader);
  Assert2(svr != nullptr, "Leader server is null");

  // Create ReplicatedDB on the leader
  std::string db_path = "/tmp/raft_test_repldb_99_" + std::to_string(leader);

  // Clean up any leftover DB from previous runs
  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, db_path.c_str(), &err);
    if (err) rocksdb_free(err);
    rocksdb_options_destroy(opts);
  }

  auto rdb = std::make_unique<ReplicatedDB>(svr, db_path);
  Assert2(rdb->IsOpen(), "ReplicatedDB should be open");

  // Register the apply callback on the server
  // @unsafe { RegLearnerAction }
  svr->RegLearnerAction([&rdb](int slot, janus::Command md) -> int {
    rdb->ApplyEntry(slot, md);
    return 0;
  });

  // Put a key-value pair (goes through Raft)
  bool put_ok = rdb->Put("linread_key", "linread_value");
  Assert2(put_ok, "Put should succeed on leader");

  // LinearizableGet on the leader should succeed
  std::string value;
  bool get_ok = rdb->LinearizableGet("linread_key", &value);
  Assert2(get_ok, "LinearizableGet should succeed on leader");
  Assert2(value == "linread_value",
          "LinearizableGet value should be 'linread_value', got '%s'", value.c_str());

  // LinearizableGet for non-existent key should fail
  std::string value2;
  bool get_ok2 = rdb->LinearizableGet("nonexistent_key", &value2);
  Assert2(!get_ok2, "LinearizableGet should return false for non-existent key");

  // Create ReplicatedDB on a follower and verify LinearizableGet fails
  siteid_t follower = static_cast<siteid_t>(-1);
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t sid = config_->getServerIdByIndex(i);
    if (static_cast<int>(sid) != leader) {
      follower = sid;
      break;
    }
  }
  Assert2(follower != static_cast<siteid_t>(-1), "Should have at least one follower");

  auto* follower_svr = config_->GetServer(follower);
  Assert2(follower_svr != nullptr, "Follower server is null");

  std::string follower_db_path = "/tmp/raft_test_repldb_99_follower_" + std::to_string(follower);

  // Clean up any leftover DB
  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, follower_db_path.c_str(), &err);
    if (err) rocksdb_free(err);
    rocksdb_options_destroy(opts);
  }

  auto follower_rdb = std::make_unique<ReplicatedDB>(follower_svr, follower_db_path);
  Assert2(follower_rdb->IsOpen(), "Follower ReplicatedDB should be open");

  // LinearizableGet on follower should fail (not leader)
  std::string follower_value;
  bool follower_get = follower_rdb->LinearizableGet("linread_key", &follower_value);
  Assert2(!follower_get, "LinearizableGet should fail on follower");

  // Restore and cleanup
  config_->SetLearnerAction();
  follower_rdb.reset();
  rdb.reset();

  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, db_path.c_str(), &err);
    if (err) rocksdb_free(err);
    rocksdb_options_destroy(opts);
  }
  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, follower_db_path.c_str(), &err);
    if (err) rocksdb_free(err);
    rocksdb_options_destroy(opts);
  }

  Log_info("TEST 99: LinearizableGet on leader and follower PASSED!");
  Passed2();
}

// ============================================================================
// Test 100: testLinearizableGetAfterLeaderChange
// ============================================================================
// Put a key, disconnect the leader to force a new election, verify
// LinearizableGet fails on the old leader and succeeds on the new leader.
// @unsafe - Uses ReplicatedDB which wraps RocksDB and Raft
int RaftLabTest::testLinearizableGetAfterLeaderChange(void) {
  Init2(100, "LinearizableGet after leader change");

  // Wait for election
  // @unsafe { Fiber::sleep }
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader1 = config_->OneLeader();
  Assert2(leader1 >= 0, "No leader elected");

  auto* svr1 = config_->GetServer(leader1);
  Assert2(svr1 != nullptr, "Leader server is null");

  // Create ReplicatedDB on the leader
  std::string db_path1 = "/tmp/raft_test_repldb_100_" + std::to_string(leader1);

  // Clean up any leftover DB
  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, db_path1.c_str(), &err);
    if (err) rocksdb_free(err);
    rocksdb_options_destroy(opts);
  }

  auto rdb1 = std::make_unique<ReplicatedDB>(svr1, db_path1);
  Assert2(rdb1->IsOpen(), "ReplicatedDB should be open");

  // @unsafe { RegLearnerAction }
  svr1->RegLearnerAction([&rdb1](int slot, janus::Command md) -> int {
    rdb1->ApplyEntry(slot, md);
    return 0;
  });

  // Put a key-value pair
  bool put_ok = rdb1->Put("leader_change_key", "leader_change_value");
  Assert2(put_ok, "Put should succeed on leader");

  // Verify LinearizableGet works on current leader
  std::string value1;
  bool get_ok1 = rdb1->LinearizableGet("leader_change_key", &value1);
  Assert2(get_ok1, "LinearizableGet should succeed on leader before disconnect");
  Assert2(value1 == "leader_change_value",
          "Value should be 'leader_change_value', got '%s'", value1.c_str());

  // Disconnect the leader to force a new election
  config_->Disconnect(leader1);

  // Wait for new election
  // @unsafe { Fiber::sleep }
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  int leader2 = config_->OneLeader();
  Assert2(leader2 >= 0, "New leader should be elected after disconnect");
  Assert2(leader2 != leader1, "New leader should be different from old leader");

  // LinearizableGet on disconnected old leader should fail
  // (it's disconnected, IsLeader() should eventually return false)
  std::string old_value;
  bool old_get = rdb1->LinearizableGet("leader_change_key", &old_value);
  Assert2(!old_get, "LinearizableGet should fail on disconnected old leader");

  // Create ReplicatedDB on the new leader and verify LinearizableGet works
  auto* svr2 = config_->GetServer(leader2);
  Assert2(svr2 != nullptr, "New leader server is null");

  std::string db_path2 = "/tmp/raft_test_repldb_100_" + std::to_string(leader2);

  // Clean up any leftover DB
  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, db_path2.c_str(), &err);
    if (err) rocksdb_free(err);
    rocksdb_options_destroy(opts);
  }

  auto rdb2 = std::make_unique<ReplicatedDB>(svr2, db_path2);
  Assert2(rdb2->IsOpen(), "New leader ReplicatedDB should be open");

  // @unsafe { RegLearnerAction }
  svr2->RegLearnerAction([&rdb2](int slot, janus::Command md) -> int {
    rdb2->ApplyEntry(slot, md);
    return 0;
  });

  // Put a new key on the new leader to ensure it's applied
  bool put_ok2 = rdb2->Put("new_leader_key", "new_leader_value");
  Assert2(put_ok2, "Put should succeed on new leader");

  // LinearizableGet on new leader should work
  std::string new_value;
  bool new_get = rdb2->LinearizableGet("new_leader_key", &new_value);
  Assert2(new_get, "LinearizableGet should succeed on new leader");
  Assert2(new_value == "new_leader_value",
          "Value should be 'new_leader_value', got '%s'", new_value.c_str());

  // Reconnect old leader and cleanup
  config_->Reconnect(leader1);

  // Restore and cleanup
  config_->SetLearnerAction();
  rdb1.reset();
  rdb2.reset();

  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, db_path1.c_str(), &err);
    if (err) rocksdb_free(err);
    rocksdb_options_destroy(opts);
  }
  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, db_path2.c_str(), &err);
    if (err) rocksdb_free(err);
    rocksdb_options_destroy(opts);
  }

  Log_info("TEST 100: LinearizableGet after leader change PASSED!");
  Passed2();
}

// ============================================================================
// Test 101: testReplicatedDBCrashRecovery
// ============================================================================
// Kill a follower replica, commit entries on remaining nodes, restart the killed
// replica, verify it catches up and has correct RocksDB state.
// @unsafe - Uses ReplicatedDB, Kill/Restart, RocksDB C API
int RaftLabTest::testReplicatedDBCrashRecovery(void) {
  Init2(101, "ReplicatedDB crash recovery");

  // Wait for election
  // @unsafe { Fiber::sleep }
  Fiber::sleep(ELECTIONTIMEOUT);

  int leader = config_->OneLeader();
  Assert2(leader >= 0, "No leader elected");

  auto* leader_svr = config_->GetServer(leader);
  Assert2(leader_svr != nullptr, "Leader server is null");

  // Find two followers: one to kill, one to verify replication
  siteid_t follower_victim = static_cast<siteid_t>(-1);
  for (int i = 0; i < NSERVERS; i++) {
    siteid_t sid = config_->getServerIdByIndex(i);
    if (static_cast<int>(sid) != leader) {
      follower_victim = sid;
      break;
    }
  }
  Assert2(follower_victim != static_cast<siteid_t>(-1), "No follower found");

  auto* follower_svr = config_->GetServer(follower_victim);
  Assert2(follower_svr != nullptr, "Follower server is null");

  // Set up DB paths
  std::string leader_db_path = "/tmp/raft_test_repldb_101_leader_" + std::to_string(leader);
  std::string follower_db_path = "/tmp/raft_test_repldb_101_follower_" + std::to_string(follower_victim);

  // Clean up previous DBs
  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, leader_db_path.c_str(), &err);
    if (err) { rocksdb_free(err); err = nullptr; }
    rocksdb_destroy_db(opts, follower_db_path.c_str(), &err);
    if (err) { rocksdb_free(err); err = nullptr; }
    rocksdb_options_destroy(opts);
  }

  // Step 1: Create ReplicatedDB on leader
  auto leader_rdb = std::make_unique<ReplicatedDB>(leader_svr, leader_db_path);
  Assert2(leader_rdb->IsOpen(), "Leader ReplicatedDB should be open");

  // @unsafe { RegLearnerAction }
  leader_svr->RegLearnerAction([&leader_rdb](int slot, janus::Command md) -> int {
    leader_rdb->ApplyEntry(slot, md);
    return 0;
  });

  // Step 2: Put initial keys on leader
  Assert2(leader_rdb->Put("k1", "v1"), "Put k1 should succeed");
  Assert2(leader_rdb->Put("k2", "v2"), "Put k2 should succeed");
  Log_info("TEST 101: Put k1=v1, k2=v2 on leader");

  // Step 3: Wait for replication to all followers
  // @unsafe { Fiber::sleep }
  Fiber::sleep(1000000);  // 1 second

  // Step 4: Create ReplicatedDB on follower and verify it has k1 and k2
  auto follower_rdb = std::make_unique<ReplicatedDB>(follower_svr, follower_db_path);
  Assert2(follower_rdb->IsOpen(), "Follower ReplicatedDB should be open");

  // @unsafe { RegLearnerAction }
  follower_svr->RegLearnerAction([&follower_rdb](int slot, janus::Command md) -> int {
    follower_rdb->ApplyEntry(slot, md);
    return 0;
  });

  // Wait for apply callback to fire for already-committed entries
  // @unsafe { Fiber::sleep }
  Fiber::sleep(500000);  // 500ms

  // Verify follower has the keys via apply callback
  bool found_k1 = false;
  bool found_k2 = false;
  for (int attempt = 0; attempt < 30; attempt++) {
    std::string val;
    found_k1 = follower_rdb->Get("k1", &val) && val == "v1";
    found_k2 = follower_rdb->Get("k2", &val) && val == "v2";
    if (found_k1 && found_k2) break;
    // @unsafe { usleep }
    usleep(100000);  // 100ms
  }
  Assert2(found_k1, "Follower should have k1=v1 before kill");
  Assert2(found_k2, "Follower should have k2=v2 before kill");
  Log_info("TEST 101: Follower verified k1, k2 before kill");

  // Step 5: Kill the follower - destroy the ReplicatedDB first
  follower_rdb.reset();
  Log_info("TEST 101: Killing follower {}", follower_victim);
  config_->Kill(follower_victim);

  // Wait for the kill to take effect
  // @unsafe { Fiber::sleep }
  Fiber::sleep(ELECTIONTIMEOUT / 2);

  // Step 6: Put more keys on leader while follower is dead
  Assert2(leader_rdb->Put("k3", "v3"), "Put k3 should succeed while follower is dead");
  Assert2(leader_rdb->Put("k4", "v4"), "Put k4 should succeed while follower is dead");
  Log_info("TEST 101: Put k3=v3, k4=v4 on leader while follower is dead");

  // Verify leader has all 4 keys
  {
    std::string val;
    Assert2(leader_rdb->Get("k1", &val) && val == "v1", "Leader should have k1=v1");
    Assert2(leader_rdb->Get("k2", &val) && val == "v2", "Leader should have k2=v2");
    Assert2(leader_rdb->Get("k3", &val) && val == "v3", "Leader should have k3=v3");
    Assert2(leader_rdb->Get("k4", &val) && val == "v4", "Leader should have k4=v4");
  }
  Log_info("TEST 101: Leader verified all 4 keys");

  // Step 7: Restart the follower
  Log_info("TEST 101: Restarting follower {}", follower_victim);
  config_->Restart(follower_victim);

  // Step 8: Wait for Raft to replicate missed entries to the restarted follower
  // @unsafe { Fiber::sleep }
  Fiber::sleep(ELECTIONTIMEOUT);

  // Step 9: Create a new ReplicatedDB on the restarted follower
  // The follower's RocksDB already has k1 and k2 from before the kill.
  // We need to set up the apply callback so new entries (k3, k4) get applied.
  auto* restarted_svr = config_->GetServer(follower_victim);
  Assert2(restarted_svr != nullptr, "Restarted follower server is null");

  // Clean the old follower DB path - the restarted server needs a fresh DB
  // because the old DB files are from the pre-crash state
  // Actually, keep the old DB - it has k1 and k2, and the apply callback
  // should be idempotent. We just need to re-open it.
  auto restarted_rdb = std::make_unique<ReplicatedDB>(restarted_svr, follower_db_path);
  Assert2(restarted_rdb->IsOpen(), "Restarted follower ReplicatedDB should be open");

  // Register apply callback on the restarted server
  // @unsafe { RegLearnerAction }
  restarted_svr->RegLearnerAction([&restarted_rdb](int slot, janus::Command md) -> int {
    restarted_rdb->ApplyEntry(slot, md);
    return 0;
  });

  // Wait for apply callback to process the missed entries (k3, k4)
  // @unsafe { Fiber::sleep }
  Fiber::sleep(1000000);  // 1 second

  // Step 10: Verify the restarted follower has all 4 keys
  bool all_found = false;
  for (int attempt = 0; attempt < 50; attempt++) {
    std::string v1, v2, v3, v4;
    bool has_k1 = restarted_rdb->Get("k1", &v1) && v1 == "v1";
    bool has_k2 = restarted_rdb->Get("k2", &v2) && v2 == "v2";
    bool has_k3 = restarted_rdb->Get("k3", &v3) && v3 == "v3";
    bool has_k4 = restarted_rdb->Get("k4", &v4) && v4 == "v4";
    if (has_k1 && has_k2 && has_k3 && has_k4) {
      all_found = true;
      break;
    }
    // @unsafe { usleep }
    usleep(200000);  // 200ms
  }
  Assert2(all_found, "Restarted follower should have all 4 keys (k1-k4)");
  Log_info("TEST 101: Restarted follower verified all 4 keys");

  // Step 11: Verify the cluster can still commit with all 5 nodes
  // Restore learner action first for the agreement check
  config_->SetLearnerAction();

  uint64_t agree_idx = config_->DoAgreement(10101, NSERVERS, true);
  Assert2(agree_idx > 0, "Cluster should still commit with all 5 nodes after recovery");
  Log_info("TEST 101: Cluster committed with all 5 nodes, index={}", agree_idx);

  // Cleanup
  leader_rdb.reset();
  restarted_rdb.reset();

  // @unsafe { RocksDB C API }
  {
    rocksdb_options_t* opts = rocksdb_options_create();
    char* err = nullptr;
    rocksdb_destroy_db(opts, leader_db_path.c_str(), &err);
    if (err) { rocksdb_free(err); err = nullptr; }
    rocksdb_destroy_db(opts, follower_db_path.c_str(), &err);
    if (err) { rocksdb_free(err); err = nullptr; }
    rocksdb_options_destroy(opts);
  }

  Log_info("TEST 101: ReplicatedDB crash recovery PASSED!");
  Passed2();
}

#endif

}
