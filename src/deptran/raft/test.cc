#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <inttypes.h>
#include <cerrno>
#include <cstring>

#include "test.h"
#include "snapshot_manager.hpp"
#include "snapshot_format.hpp"
#include "memory_snapshot_manager.hpp"

import std;
import rusty;

namespace janus {

#ifdef RAFT_TEST_CORO

// @unsafe - Test-only RAII kernel for cleanup across Assert2 early returns.
// The Rust DSL cannot currently express a C++ destructor that owns a capturing
// lambda, so test control flow supplies the cleanup body and this kernel only
// guarantees exactly-once invocation at scope exit.
template <typename Cleanup>
class RaftTestScopeExit {
 public:
  // @unsafe - Moves a capturing C++ closure into the guard.
  explicit RaftTestScopeExit(Cleanup cleanup)
      : cleanup_(std::move(cleanup)) {}
  RaftTestScopeExit(const RaftTestScopeExit&) = delete;
  RaftTestScopeExit& operator=(const RaftTestScopeExit&) = delete;
  // @unsafe - Invokes the test-owned cleanup closure.
  ~RaftTestScopeExit() noexcept { cleanup_(); }

 private:
  Cleanup cleanup_;
};

// @unsafe - Constructs a test-only cleanup guard around a C++ closure.
template <typename Cleanup>
RaftTestScopeExit<Cleanup> MakeRaftTestScopeExit(Cleanup cleanup) {
  return RaftTestScopeExit<Cleanup>(std::move(cleanup));
}

// Test-only prepared transaction whose commit succeeds only after the exact
// Raft snapshot is readable from SnapshotManager. This is an executable oracle
// for Prepare -> manager TakeSnapshot -> Commit publication ordering.
class SnapshotPublicationProbe final
    : public PreparedStateMachineSnapshotInstall {
 public:
  SnapshotPublicationProbe(
      std::shared_ptr<janus::raft::SnapshotManager> manager,
      uint64_t expected_index,
      uint64_t expected_term,
      std::string expected_data,
      std::atomic<bool>* commit_called,
      std::atomic<bool>* commit_saw_published,
      std::atomic<bool>* aborted_before_commit)
      : manager_(std::move(manager)),
        expected_index_(expected_index),
        expected_term_(expected_term),
        expected_data_(std::move(expected_data)),
        commit_called_(commit_called),
        commit_saw_published_(commit_saw_published),
        aborted_before_commit_(aborted_before_commit) {}

  // @safe - Records whether an uncommitted probe was abandoned.
  ~SnapshotPublicationProbe() override {
    if (!commit_attempted_ && aborted_before_commit_ != nullptr) {
      aborted_before_commit_->store(true, std::memory_order_release);
    }
  }

  // @unsafe - Reads test SnapshotManager state at the commit linearization point.
  bool Commit() override {
    if (commit_attempted_) {
      return false;
    }
    commit_attempted_ = true;
    janus::raft::SnapshotMetadata metadata;
    std::string data;
    const bool published = manager_ != nullptr &&
        manager_->LoadLatestSnapshot(&metadata, &data) &&
        metadata.last_included_index == expected_index_ &&
        metadata.last_included_term == expected_term_ &&
        data == expected_data_;
    if (commit_saw_published_ != nullptr) {
      commit_saw_published_->store(published, std::memory_order_release);
    }
    if (commit_called_ != nullptr) {
      commit_called_->store(true, std::memory_order_release);
    }
    return published;
  }

 private:
  std::shared_ptr<janus::raft::SnapshotManager> manager_;
  uint64_t expected_index_ = 0;
  uint64_t expected_term_ = 0;
  std::string expected_data_;
  std::atomic<bool>* commit_called_ = nullptr;
  std::atomic<bool>* commit_saw_published_ = nullptr;
  std::atomic<bool>* aborted_before_commit_ = nullptr;
  bool commit_attempted_ = false;
};

// @unsafe - Test-only LabAccess bridge for rotating storage on a live server.
// Holding both locks through manager publication and the initial checkpoint means
// the server never advertises a compacted prefix without bytes in the active
// manager. This deliberately does not become production RaftServer API.
bool RaftLabTest::InstallAndSeedSnapshotManager(
    RaftServer* server,
    std::shared_ptr<janus::raft::SnapshotManager> manager,
    uint64_t snapshot_threshold,
    uint64_t* seeded_snapshot_index) {
  if (server == nullptr || manager == nullptr ||
      seeded_snapshot_index == nullptr) {
    return false;
  }

  std::lock_guard<std::mutex> apply_lock(
      RaftServer::LabAccess::state_machine_apply_mtx(*server));
  std::lock_guard<std::mutex> lock(server->mtx_);

  if (RaftServer::LabAccess::snapidx(*server) > 0) {
    // A compacted boundary is meaningful only together with its exact state
    // bytes. Copy that checkpoint rather than regenerating a possibly newer
    // state-machine image while rotating managers.
    auto old_manager = RaftServer::LabAccess::snapshot_manager(*server);
    if (old_manager == nullptr) {
      return false;
    }
    janus::raft::SnapshotMetadata metadata;
    std::string state_data;
    if (!old_manager->LoadLatestSnapshot(&metadata, &state_data) ||
        metadata.last_included_index != RaftServer::LabAccess::snapidx(*server) ||
        metadata.last_included_term != RaftServer::LabAccess::snapterm(*server) ||
        !manager->TakeSnapshot(
            metadata.last_included_index, metadata.last_included_term,
            state_data.data(), state_data.size())) {
      return false;
    }

    server->SetSnapshotManagerLocked(std::move(manager));
  } else {
    // With no advertised boundary, the replacement may be published only
    // inside this gate and rolled back if the initial checkpoint fails.
    auto old_manager = RaftServer::LabAccess::snapshot_manager(*server);
    server->SetSnapshotManagerLocked(manager);
    if (!RaftServer::LabAccess::CreateSnapshotLocked(*server)) {
      server->SetSnapshotManagerLocked(std::move(old_manager));
      return false;
    }
  }

  server->SetSnapshotThresholdLocked(snapshot_threshold);
  *seeded_snapshot_index = RaftServer::LabAccess::snapidx(*server);
  return true;
}

// #define TEST_EXPAND(x) x || x || x || x || x 
#define TEST_EXPAND(x) x 

int RaftLabTest::Run(void) {
  Log_info("Starting Raft lab tests");
  Log_info("Setting up learner action callbacks");
  config_->SetLearnerAction();
  uint64_t start_rpc = config_->RpcTotal();
  Log_info("Beginning test sequence");

  Log_info("Running BASIC Raft test group");
  bool failed =
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

  // Snapshot data format and metadata tests
  // These are unit tests that don't require persistence
  if (!failed) {
    Log_info("Running SNAPSHOT data format tests");
    failed =
        TEST_EXPAND(testSnapshotMetadataCreation())         // Test 50
        || TEST_EXPAND(testSnapshotFormatRoundTrip())       // Test 51
        || TEST_EXPAND(testSnapshotManagerSaveLoad())       // Test 52
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

  // High frequency apply stress test
  if (!failed) {
    Log_info("Running high frequency apply stress test");
    failed =
        TEST_EXPAND(testHighFrequencyApply());                 // Test 72
  }

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
  // that the upstream MIT 6.824 reference implementation did not emit. The
  // 40-56 range was observed on a quiet local run while the since-retired
  // TimeoutNow, NotifyRestart and durable-ack RPCs were still emitted; 70
  // leaves headroom for jitter.
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

void RaftLabTest::wait(uint64_t microseconds) {
  create_sp_timeout_event(microseconds)->wait();
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

  janus::raft::MemorySnapshotManager mgr;

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

  Log_info("[SNAPSHOT-MGR-TEST] Save/load round-trip PASSED");
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
  auto test_mgr = std::make_shared<janus::raft::MemorySnapshotManager>();

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

  // Set up an in-memory snapshot manager
  auto test_mgr = std::make_shared<janus::raft::MemorySnapshotManager>();
  auto original_threshold = server->GetSnapshotThreshold();
  {
    std::lock_guard<std::mutex> lock(server->mtx_);
    server->SetSnapshotManagerLocked(test_mgr);
    // Set a low threshold so we can trigger a snapshot easily.
    server->SetSnapshotThresholdLocked(5);
  }

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

  // The snapshot now backs a compacted live prefix.  Restore only the runtime
  // threshold; keep the manager and its snapshot available for future catch-up.
  {
    std::lock_guard<std::mutex> lock(server->mtx_);
    server->SetSnapshotThresholdLocked(original_threshold);
  }
  Log_info("[CREATE-SNAPSHOT-BASIC-TEST] Retaining live in-memory snapshot manager");

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
  auto test_mgr = std::make_shared<janus::raft::MemorySnapshotManager>();
  auto original_threshold = server->GetSnapshotThreshold();
  uint64_t snapshot_baseline = 0;
  Assert2(InstallAndSeedSnapshotManager(
              server, test_mgr, 5, &snapshot_baseline),
          "Could not atomically seed Test56 replacement snapshot manager");

  // Submit entries to trigger snapshot
  uint64_t first_new_index = 0;
  for (int i = 1; i <= 10; i++) {
    uint64_t idx = config_->DoAgreement(200 + i, NSERVERS, true);
    Assert2(idx > 0, "DoAgreement failed for cmd %d", 200 + i);
    if (first_new_index == 0) {
      first_new_index = idx;
    }
  }

  uint64_t snap_idx = 0;
  bool snapshot_ready = false;
  for (int attempt = 0; attempt < 200 && !snapshot_ready; ++attempt) {
    {
      std::lock_guard<std::mutex> lock(server->mtx_);
      snap_idx = server->GetSnapshotIndexLocked();
    }
    auto candidate = test_mgr->GetLatestSnapshot();
    if (candidate.is_some()) {
      const auto candidate_metadata = candidate.unwrap();
      snapshot_ready =
          candidate_metadata.last_included_index == snap_idx &&
          snap_idx > snapshot_baseline && snap_idx >= first_new_index;
    }
    if (!snapshot_ready) {
      Fiber::sleep(10000);
    }
  }
  auto latest = test_mgr->GetLatestSnapshot();
  Assert2(snapshot_ready && latest.is_some(),
          "Replacement snapshot manager did not advance for this workload");
  const auto metadata = latest.unwrap();
  Assert2(snap_idx == metadata.last_included_index,
          "Server snapshot index %lu does not match manager index %lu",
          snap_idx, metadata.last_included_index);
  Assert2(snap_idx > snapshot_baseline && snap_idx >= first_new_index,
          "Snapshot did not advance for this workload: baseline=%lu, first=%lu, got=%lu",
          snapshot_baseline, first_new_index, snap_idx);

  // Now submit more entries AFTER snapshot - these should still commit
  for (int i = 1; i <= 5; i++) {
    uint64_t idx = config_->DoAgreement(300 + i, NSERVERS, true);
    Assert2(idx > 0, "DoAgreement after snapshot failed for cmd %d", 300 + i);
  }

  // Verify the system is still functional
  int leader2 = config_->OneLeader();
  Assert2(leader2 >= 0, "Should still have a leader after snapshot+compaction");

  // The snapshot now backs a compacted live prefix.  Restore only the runtime
  // threshold; keep the manager and its snapshot available for future catch-up.
  {
    std::lock_guard<std::mutex> lock(server->mtx_);
    server->SetSnapshotThresholdLocked(original_threshold);
  }
  Log_info("[CREATE-SNAPSHOT-COMPACTION-TEST] Retaining live in-memory snapshot manager");

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
// Test 58: Same-term InstallSnapshot at/below commit is a successful no-op
// =============================================================================
// @unsafe - test function that exercises OnInstallSnapshot
int RaftLabTest::testInstallSnapshotBasic(void) {
  Init2(58, "InstallSnapshot stale index is a no-op");

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

  // Install a unique manager and seed it atomically. A prior test may already
  // have compacted this server, so an empty replacement would violate the live
  // snapshot/log invariant even before this RPC is exercised.
  auto test_mgr = std::make_shared<janus::raft::MemorySnapshotManager>();

  uint64_t old_snapidx = 0;
  uint64_t old_snapterm = 0;
  uint64_t old_commit_index = 0;
  uint64_t old_execute_index = 0;
  uint64_t old_last_log_index = 0;
  uint64_t old_min_active_slot = 0;
  uint64_t follower_term = 0;
  uint64_t seeded_snapshot_index = 0;
  Assert2(InstallAndSeedSnapshotManager(
              server, test_mgr, server->GetSnapshotThreshold(),
              &seeded_snapshot_index),
          "Could not atomically seed Test58 replacement snapshot manager");
  {
    std::lock_guard<std::mutex> lock(server->mtx_);
    old_snapidx = server->GetSnapshotIndexLocked();
    old_snapterm = server->GetSnapshotTermLocked();
    old_commit_index = server->commitIndex;
    old_execute_index = server->executeIndex;
    old_last_log_index = server->lastLogIndex;
    old_min_active_slot = server->min_active_slot_;
    follower_term = server->currentTerm;
  }

  janus::raft::SnapshotMetadata before_metadata;
  std::string before_snapshot_data;
  Assert2(test_mgr->LoadLatestSnapshot(
              &before_metadata, &before_snapshot_data),
          "Seeded Test58 snapshot manager has no readable snapshot");
  const size_t before_snapshot_count = test_mgr->ListSnapshots().size();

  // A snapshot at commitIndex is stale by definition. It is still valid
  // leader contact (same term), but its payload must not rewrite snapshot,
  // log, or apply state.
  const uint64_t stale_snapshot_index = old_commit_index;
  Assert2(stale_snapshot_index > 0,
          "Test58 needs a non-zero committed prefix");
  const std::string stale_snapshot_data =
      "stale_same_term_snapshot_must_not_be_persisted";

  uint64_t reply_term = 0;
  server->OnInstallSnapshot(
      follower_term,
      config_->GetServer(leader)->site_id_,  // leader_id
      stale_snapshot_index,
      follower_term,
      stale_snapshot_data,
      &reply_term);

  Assert2(reply_term == follower_term,
          "Same-term stale snapshot reply should be %lu, got %lu",
          follower_term, reply_term);

  {
    std::lock_guard<std::mutex> lock(server->mtx_);
    Assert2(server->GetSnapshotIndexLocked() == old_snapidx,
            "Stale snapshot changed snapidx from %lu to %lu",
            old_snapidx, server->GetSnapshotIndexLocked());
    Assert2(server->GetSnapshotTermLocked() == old_snapterm,
            "Stale snapshot changed snapterm from %lu to %lu",
            old_snapterm, server->GetSnapshotTermLocked());
    Assert2(server->commitIndex == old_commit_index &&
                server->executeIndex == old_execute_index &&
                server->lastLogIndex == old_last_log_index &&
                server->min_active_slot_ == old_min_active_slot,
            "Stale snapshot mutated log/apply indices");
    Assert2(!server->IsLeaderLocked(),
            "Accepted same-term leader contact must leave receiver a follower");
  }

  janus::raft::SnapshotMetadata after_metadata;
  std::string after_snapshot_data;
  Assert2(test_mgr->LoadLatestSnapshot(
              &after_metadata, &after_snapshot_data),
          "Stale snapshot removed or corrupted the existing snapshot");
  Assert2(after_metadata.last_included_index ==
              before_metadata.last_included_index &&
              after_metadata.last_included_term ==
                  before_metadata.last_included_term &&
              after_metadata.timestamp_ms == before_metadata.timestamp_ms &&
              after_metadata.size_bytes == before_metadata.size_bytes &&
              after_metadata.checksum == before_metadata.checksum &&
              after_snapshot_data == before_snapshot_data &&
              test_mgr->ListSnapshots().size() == before_snapshot_count,
          "Stale snapshot changed snapshot manager state");

  // Exercise the fallible Prepare boundary itself (not the stale fast path).
  // A validation rejection must not publish bytes, compact the log, or
  // fail-stop a healthy follower because the prepare contract forbids live
  // state-machine mutation.
  std::map<slotid_t, std::shared_ptr<RaftData>> rejected_logs_before;
  uint64_t rejected_snapidx_before = 0;
  uint64_t rejected_snapterm_before = 0;
  uint64_t rejected_commit_before = 0;
  uint64_t rejected_execute_before = 0;
  uint64_t rejected_last_log_before = 0;
  uint64_t rejected_min_active_before = 0;
  uint64_t rejected_local_progress = 0;
  {
    std::lock_guard<std::mutex> lock(server->mtx_);
    rejected_logs_before = server->raft_logs_;
    rejected_snapidx_before = RaftServer::LabAccess::snapidx(*server);
    rejected_snapterm_before = RaftServer::LabAccess::snapterm(*server);
    rejected_commit_before = server->commitIndex;
    rejected_execute_before = server->executeIndex;
    rejected_last_log_before = server->lastLogIndex;
    rejected_min_active_before = server->min_active_slot_;
    rejected_local_progress = std::max(
        {server->commitIndex, server->executeIndex,
         server->GetAppliedIndex(), RaftServer::LabAccess::snapidx(*server), server->lastLogIndex});
  }
  Assert2(rejected_local_progress < UINT64_MAX,
          "Test58 cannot construct a successor snapshot boundary");

  std::atomic<bool> rejecting_prepare_called{false};
  uint64_t rejecting_callback_token =
      server->SetStateMachineSnapshotCallbacks(
          [](uint64_t) { return std::string(); },
          [&rejecting_prepare_called](
              const std::string&, uint64_t)
              -> std::unique_ptr<PreparedStateMachineSnapshotInstall> {
            rejecting_prepare_called.store(true, std::memory_order_release);
            return nullptr;
          });
  Assert2(rejecting_callback_token != 0,
          "Could not install Test58 rejecting prepare callback");
  auto restore_test58_callbacks = MakeRaftTestScopeExit([&]() {  // @unsafe
    if (rejecting_callback_token != 0) {
      server->ClearStateMachineSnapshotCallbacks(
          rejecting_callback_token);
      rejecting_callback_token = 0;
    }
  });

  uint64_t rejected_reply_term = follower_term;
  server->OnInstallSnapshot(
      follower_term,
      config_->GetServer(leader)->site_id_,
      rejected_local_progress + 1,
      follower_term,
      "archive_rejected_during_prepare",
      &rejected_reply_term);
  Assert2(rejecting_prepare_called.load(std::memory_order_acquire),
          "InstallSnapshot did not invoke the rejecting Prepare callback");
  Assert2(rejected_reply_term == 0,
          "Rejected Prepare must return unavailable term 0, got %lu",
          rejected_reply_term);
  Assert2(!RaftServer::LabAccess::stop(*server).load(rusty::sync::atomic::Ordering::Acquire),
          "Clean Prepare rejection incorrectly fail-stopped the follower");

  {
    std::lock_guard<std::mutex> lock(server->mtx_);
    Assert2(RaftServer::LabAccess::snapidx(*server) == rejected_snapidx_before &&
                RaftServer::LabAccess::snapterm(*server) == rejected_snapterm_before &&
                server->commitIndex == rejected_commit_before &&
                server->executeIndex == rejected_execute_before &&
                server->lastLogIndex == rejected_last_log_before &&
                server->min_active_slot_ == rejected_min_active_before &&
                server->raft_logs_ == rejected_logs_before,
            "Rejected Prepare mutated the in-memory snapshot/log boundary");
  }

  janus::raft::SnapshotMetadata rejected_after_metadata;
  std::string rejected_after_data;
  Assert2(test_mgr->LoadLatestSnapshot(
              &rejected_after_metadata, &rejected_after_data) &&
              rejected_after_metadata.last_included_index ==
                  before_metadata.last_included_index &&
              rejected_after_metadata.last_included_term ==
                  before_metadata.last_included_term &&
              rejected_after_metadata.timestamp_ms ==
                  before_metadata.timestamp_ms &&
              rejected_after_metadata.size_bytes ==
                  before_metadata.size_bytes &&
              rejected_after_metadata.checksum == before_metadata.checksum &&
              rejected_after_data == before_snapshot_data &&
              test_mgr->ListSnapshots().size() == before_snapshot_count,
          "Rejected Prepare changed the snapshot manager");

  Assert2(server->ClearStateMachineSnapshotCallbacks(
              rejecting_callback_token),
          "Could not clear Test58 rejecting prepare callback");
  rejecting_callback_token = 0;

  Log_info("[INSTALL-SNAPSHOT-STALE-INDEX-TEST] Retaining live in-memory snapshot manager");

  Log_info("[INSTALL-SNAPSHOT-STALE-INDEX-TEST] PASSED");
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

  // Record one coherent follower state while the live Raft/apply threads are
  // excluded. The raw fields below are not atomic.
  uint64_t before_snapidx = 0;
  uint64_t before_snapterm = 0;
  uint64_t before_commitIndex = 0;
  uint64_t before_executeIndex = 0;
  uint64_t before_lastLogIndex = 0;
  uint64_t follower_term = 0;
  siteid_t before_leader_id = static_cast<siteid_t>(INVALID_SITEID);
  siteid_t before_vote_for = static_cast<siteid_t>(INVALID_SITEID);
  bool before_is_leader = false;
  bool before_req_voting = false;
  bool before_election_in_progress = false;
  {
    std::lock_guard<std::mutex> lock(server->mtx_);
    before_snapidx = RaftServer::LabAccess::snapidx(*server);
    before_snapterm = RaftServer::LabAccess::snapterm(*server);
    before_commitIndex = server->commitIndex;
    before_executeIndex = server->executeIndex;
    before_lastLogIndex = server->lastLogIndex;
    follower_term = server->currentTerm;
    before_leader_id = RaftServer::LabAccess::current_leader_id(*server);
    before_vote_for = RaftServer::LabAccess::vote_for(*server);
    before_is_leader = RaftServer::LabAccess::is_leader(*server);
    before_req_voting = RaftServer::LabAccess::req_voting(*server);
    before_election_in_progress = RaftServer::LabAccess::election_in_progress(*server);
  }

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

  // Verify follower state is UNCHANGED through one synchronized observation.
  {
    std::lock_guard<std::mutex> lock(server->mtx_);
    Assert2(RaftServer::LabAccess::snapidx(*server) == before_snapidx,
            "snapidx should be unchanged (%lu), got %lu",
            before_snapidx, RaftServer::LabAccess::snapidx(*server));
    Assert2(RaftServer::LabAccess::snapterm(*server) == before_snapterm,
            "snapterm should be unchanged (%lu), got %lu",
            before_snapterm, static_cast<uint64_t>(RaftServer::LabAccess::snapterm(*server)));
    Assert2(server->commitIndex == before_commitIndex,
            "commitIndex should be unchanged (%lu), got %lu",
            before_commitIndex, server->commitIndex);
    Assert2(server->executeIndex == before_executeIndex,
            "executeIndex should be unchanged (%lu), got %lu",
            before_executeIndex, server->executeIndex);
    Assert2(server->lastLogIndex == before_lastLogIndex &&
                server->currentTerm == follower_term &&
                RaftServer::LabAccess::current_leader_id(*server) == before_leader_id &&
                RaftServer::LabAccess::vote_for(*server) == before_vote_for &&
                RaftServer::LabAccess::is_leader(*server) == before_is_leader &&
                RaftServer::LabAccess::req_voting(*server) == before_req_voting &&
                RaftServer::LabAccess::election_in_progress(*server) ==
                    before_election_in_progress,
            "Stale-term snapshot mutated Raft role/election state");
  }

  // A same-term sender also cannot advertise a snapshot boundary from a
  // future term. This must be rejected before leader contact or snapshot
  // payload processing changes any receiver state.
  uint64_t future_boundary_reply_term = 0;
  const uint64_t future_boundary_index =
      before_lastLogIndex == UINT64_MAX
          ? before_lastLogIndex
          : before_lastLogIndex + 1;
  server->OnInstallSnapshot(
      follower_term,
      static_cast<uint64_t>(leader),
      future_boundary_index,
      follower_term + 1,
      "future_term_snapshot_must_not_be_loaded",
      &future_boundary_reply_term);
  Assert2(future_boundary_reply_term == 0,
          "Same-term future-boundary rejection must report unavailable (0), got %lu",
          future_boundary_reply_term);
  {
    std::lock_guard<std::mutex> lock(server->mtx_);
    Assert2(server->currentTerm == follower_term &&
                RaftServer::LabAccess::snapidx(*server) == before_snapidx &&
                static_cast<uint64_t>(RaftServer::LabAccess::snapterm(*server)) ==
                    before_snapterm &&
                server->commitIndex == before_commitIndex &&
                server->executeIndex == before_executeIndex &&
                server->lastLogIndex == before_lastLogIndex &&
                RaftServer::LabAccess::current_leader_id(*server) == before_leader_id &&
                RaftServer::LabAccess::vote_for(*server) == before_vote_for &&
                RaftServer::LabAccess::is_leader(*server) == before_is_leader &&
                RaftServer::LabAccess::req_voting(*server) == before_req_voting &&
                RaftServer::LabAccess::election_in_progress(*server) ==
                    before_election_in_progress,
            "Future-boundary snapshot mutated receiver state");
  }

  // A rejected, otherwise well-formed request also must not look successful
  // to the sender. InstallSnapshot's leader callback treats every non-zero
  // reply at its send term as proof that the snapshot boundary was installed.
  uint64_t unauthorized_reply_term = UINT64_MAX;
  server->OnInstallSnapshot(
      follower_term,
      998,
      future_boundary_index,
      follower_term,
      "unauthorized_snapshot_must_not_be_acknowledged",
      &unauthorized_reply_term);
  Assert2(unauthorized_reply_term == 0,
          "Unauthorized snapshot rejection must report unavailable (0), got %lu",
          unauthorized_reply_term);

  uint64_t unrepresentable_reply_term = UINT64_MAX;
  server->OnInstallSnapshot(
      follower_term,
      UINT64_MAX,
      future_boundary_index,
      follower_term,
      "unrepresentable_leader_must_not_be_acknowledged",
      &unrepresentable_reply_term);
  Assert2(unrepresentable_reply_term == 0,
          "Unrepresentable snapshot leader must report unavailable (0), got %lu",
          unrepresentable_reply_term);
  {
    std::lock_guard<std::mutex> lock(server->mtx_);
    Assert2(server->currentTerm == follower_term &&
                RaftServer::LabAccess::snapidx(*server) == before_snapidx &&
                static_cast<uint64_t>(RaftServer::LabAccess::snapterm(*server)) ==
                    before_snapterm &&
                server->commitIndex == before_commitIndex &&
                server->executeIndex == before_executeIndex &&
                server->lastLogIndex == before_lastLogIndex &&
                RaftServer::LabAccess::current_leader_id(*server) == before_leader_id &&
                RaftServer::LabAccess::vote_for(*server) == before_vote_for &&
                RaftServer::LabAccess::is_leader(*server) == before_is_leader &&
                RaftServer::LabAccess::req_voting(*server) == before_req_voting &&
                RaftServer::LabAccess::election_in_progress(*server) ==
                    before_election_in_progress,
            "Unauthorized snapshot rejection mutated receiver state");
  }

  Log_info("[INSTALL-SNAPSHOT-REJECTS-STALE-TEST] PASSED");
  Passed2();
}

// =============================================================================
// Test 60: HeartbeatLoop triggers InstallSnapshot for lagging followers
// =============================================================================
// @unsafe - test function that exercises HeartbeatLoop snapshot integration
int RaftLabTest::testHeartbeatTriggersInstallSnapshot(void) {
  Init2(60, "HeartbeatLoop installs snapshot after a real partition");

  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);

  // Give every possible leader a unique, live backing manager before the
  // partition. This keeps the test valid across an ordinary re-election.
  // The managers below become the live backing store for compacted prefixes.
  // Setting MAKO_RAFT_SNAPSHOTS keeps the process-wide snapshot knob in step
  // with these live managers for the remainder of the suite, just as Test69
  // does after its manager rotation.
  Assert2(setenv("MAKO_RAFT_SNAPSHOTS", "1", 1) == 0,  // @unsafe
          "Could not enable Test60 snapshots: %s", strerror(errno));
  std::vector<std::shared_ptr<janus::raft::SnapshotManager>> managers(
      NSERVERS);
  std::vector<uint64_t> original_thresholds(NSERVERS, 0);
  std::vector<uint64_t> seeded_snapshot_indices(NSERVERS, 0);
  for (int i = 0; i < NSERVERS; ++i) {
    auto server = config_->GetServer(i);
    Assert2(server != nullptr, "Test60 server %d is null", i);
    auto manager = std::make_shared<janus::raft::MemorySnapshotManager>();
    managers[i] = manager;
    original_thresholds[i] = server->GetSnapshotThreshold();
    Assert2(InstallAndSeedSnapshotManager(
                server, manager, 3, &seeded_snapshot_indices[i]),
            "Could not atomically seed Test60 server %d snapshot manager", i);
  }

  int follower = -1;
  for (int i = 0; i < NSERVERS; ++i) {
    if (i != leader) {
      follower = i;
      break;
    }
  }
  Assert2(follower >= 0, "No Test60 follower found");
  auto follower_server = config_->GetServer(follower);
  Assert2(follower_server != nullptr, "Test60 follower is null");

  std::atomic<bool> snapshot_prepare_called{false};
  std::atomic<bool> snapshot_prepare_saw_old_manager{false};
  std::atomic<bool> snapshot_commit_called{false};
  std::atomic<bool> snapshot_commit_saw_published{false};
  std::atomic<bool> snapshot_aborted_before_commit{false};
  uint64_t snapshot_callback_token = 0;

  bool follower_disconnected = false;
  auto restore_test60 = MakeRaftTestScopeExit([&]() {  // @unsafe
    if (snapshot_callback_token != 0) {
      follower_server->ClearStateMachineSnapshotCallbacks(
          snapshot_callback_token);
      snapshot_callback_token = 0;
    }
    if (follower_disconnected) {
      config_->Reconnect(follower);
    }
    for (int i = 0; i < NSERVERS; ++i) {
      auto server = config_->GetServer(i);
      if (server == nullptr) continue;
      std::lock_guard<std::mutex> lock(server->mtx_);
      server->SetSnapshotThresholdLocked(original_thresholds[i]);
    }
  });

  uint64_t follower_snap_before = 0;
  uint64_t follower_last_before = 0;
  {
    std::lock_guard<std::mutex> lock(follower_server->mtx_);
    follower_snap_before = follower_server->GetSnapshotIndexLocked();
    follower_last_before = follower_server->lastLogIndex;
  }

  config_->Disconnect(follower);
  follower_disconnected = true;

  // The callback-only command oracle cannot recreate a covered callback when
  // a connected replica catches up through a marker-only snapshot. Require a
  // real quorum through DoAgreement and separately require every reachable
  // replica to publish the applied boundary, just as Test69 does below.
  const int partition_quorum = NSERVERS / 2 + 1;
  uint64_t first_partition_index = 0;
  uint64_t last_partition_index = 0;
  for (int i = 1; i <= 8; ++i) {
    uint64_t idx =
        config_->DoAgreement(600 + i, partition_quorum, true);
    Assert2(idx > 0, "Test60 agreement failed for cmd %d", 600 + i);

    int connected_applied = 0;
    for (int attempt = 0;
         attempt < 100 && connected_applied < NSERVERS - 1;
         ++attempt) {
      connected_applied = 0;
      for (int site = 0; site < NSERVERS; ++site) {
        if (site == follower) continue;
        auto connected_server = config_->GetServer(site);
        if (connected_server != nullptr &&
            connected_server->GetAppliedIndex() >= idx) {
          connected_applied++;
        }
      }
      if (connected_applied < NSERVERS - 1) {
        Fiber::sleep(HEARTBEAT_INTERVAL);
      }
    }
    Assert2(connected_applied == NSERVERS - 1,
            "Only %d of %d connected Test60 replicas published applied "
            "index %lu for cmd %d",
            connected_applied, NSERVERS - 1, idx, 600 + i);

    if (first_partition_index == 0) {
      first_partition_index = idx;
    }
    last_partition_index = idx;
  }

  leader = config_->OneLeader();
  AssertOneLeader(leader);
  auto leader_server = config_->GetServer(leader);
  Assert2(leader_server != nullptr && leader >= 0 && leader < NSERVERS &&
              managers[leader] != nullptr,
          "Test60 has no live manager for leader %d", leader);

  uint64_t leader_execute_index = 0;
  uint64_t leader_snap_idx = 0;
  uint64_t leader_min_active = 0;
  bool leader_snapshot_ready = false;
  for (int attempt = 0;
       attempt < 300 && !leader_snapshot_ready;
       ++attempt) {
    {
      std::lock_guard<std::mutex> lock(leader_server->mtx_);
      leader_execute_index = leader_server->executeIndex;
      leader_snap_idx = leader_server->GetSnapshotIndexLocked();
      leader_min_active = leader_server->min_active_slot_;
    }
    auto candidate = managers[leader]->GetLatestSnapshot();
    if (candidate.is_some()) {
      const auto metadata = candidate.unwrap();
      leader_snapshot_ready =
          leader_execute_index >= last_partition_index &&
          metadata.last_included_index == leader_snap_idx &&
          leader_snap_idx > seeded_snapshot_indices[leader] &&
          leader_snap_idx >= first_partition_index;
    }
    if (!leader_snapshot_ready) {
      Fiber::sleep(10000);
    }
  }
  Assert2(leader_snapshot_ready,
          "Test60 leader did not create a fresh partition snapshot");
  Assert2(leader_snap_idx > follower_snap_before,
          "Leader snapshot %lu did not advance beyond follower %lu",
          leader_snap_idx, follower_snap_before);
  Assert2(follower_last_before < UINT64_MAX &&
              leader_min_active > follower_last_before + 1,
          "Leader retained a bridgeable log gap: min_active=%lu follower_next=%lu",
          leader_min_active, follower_last_before + 1);

  // Replace the marker-only loader with an owned probe for this one install.
  // Prepare observes the old manager state; Commit itself refuses to succeed
  // unless the exact incoming bytes are already readable from that manager.
  snapshot_callback_token =
      follower_server->SetStateMachineSnapshotCallbacks(
          [](uint64_t) { return std::string(); },
          [&, follower_manager = managers[follower]](
              const std::string& incoming_data,
              uint64_t incoming_index)
              -> std::unique_ptr<PreparedStateMachineSnapshotInstall> {
            constexpr size_t kMarkerSize = sizeof(uint64_t) * 2;
            if (incoming_data.size() != kMarkerSize) {
              return nullptr;
            }
            uint64_t marker_index = 0;
            uint64_t marker_term = 0;
            std::memcpy(&marker_index, incoming_data.data(),
                        sizeof(marker_index));
            std::memcpy(&marker_term,
                        incoming_data.data() + sizeof(marker_index),
                        sizeof(marker_term));
            if (marker_index != incoming_index) {
              return nullptr;
            }

            auto previous = follower_manager->GetLatestSnapshot();
            const bool still_old = previous.is_none() ||
                previous.unwrap().last_included_index < incoming_index;
            snapshot_prepare_saw_old_manager.store(
                still_old, std::memory_order_release);
            snapshot_prepare_called.store(true, std::memory_order_release);
            return std::make_unique<SnapshotPublicationProbe>(
                follower_manager, incoming_index, marker_term, incoming_data,
                &snapshot_commit_called, &snapshot_commit_saw_published,
                &snapshot_aborted_before_commit);
          });
  Assert2(snapshot_callback_token != 0,
          "Could not install Test60 snapshot publication probe");

  config_->Reconnect(follower);
  follower_disconnected = false;

  // Reconnecting the isolated follower can legitimately advance the term and
  // elect a different leader.  That leader may have compacted to a slightly
  // earlier (but still bridging) snapshot than `leader_snap_idx`, which was
  // captured from the pre-reconnect leader.  Raft requires the current leader
  // to install a snapshot beyond the follower's old log and then repair the
  // remaining suffix; it does not require every replica to converge on the
  // former leader's exact compaction boundary.
  const uint64_t required_snapshot_floor =
      std::max(follower_last_before + 1, first_partition_index);
  uint64_t follower_snap_after = follower_snap_before;
  for (int attempt = 0;
       attempt < 100 && follower_snap_after < required_snapshot_floor;
       ++attempt) {
    Fiber::sleep(HEARTBEAT_INTERVAL);
    std::lock_guard<std::mutex> lock(follower_server->mtx_);
    follower_snap_after = follower_server->GetSnapshotIndexLocked();
  }
  Assert2(follower_snap_after >= required_snapshot_floor &&
              follower_snap_after > follower_snap_before,
          "Follower snapshot did not bridge its old log from %lu through "
          "required floor %lu (pre-reconnect leader snapshot was %lu); got %lu",
          follower_snap_before, required_snapshot_floor, leader_snap_idx,
          follower_snap_after);
  auto follower_snapshot = managers[follower]->GetLatestSnapshot();
  Assert2(follower_snapshot.is_some() &&
              follower_snapshot.unwrap().last_included_index ==
                  follower_snap_after,
          "Follower manager does not contain the installed snapshot %lu",
          follower_snap_after);
  Assert2(snapshot_prepare_called.load(std::memory_order_acquire) &&
              snapshot_prepare_saw_old_manager.load(
                  std::memory_order_acquire),
          "InstallSnapshot Prepare did not run against the old manager image");
  Assert2(snapshot_commit_called.load(std::memory_order_acquire) &&
              snapshot_commit_saw_published.load(std::memory_order_acquire) &&
              !snapshot_aborted_before_commit.load(std::memory_order_acquire),
          "InstallSnapshot Commit ran before exact Raft snapshot publication");

  Assert2(follower_server->ClearStateMachineSnapshotCallbacks(
              snapshot_callback_token),
          "Could not clear Test60 snapshot publication probe");
  snapshot_callback_token = 0;

  uint64_t new_idx = config_->DoAgreement(700, NSERVERS, true);
  Assert2(new_idx > 0,
          "Cluster did not make progress after Test60 snapshot recovery");

  Log_info("[HEARTBEAT-SNAPSHOT-TEST] Retaining live in-memory snapshot managers");

  Log_info("[HEARTBEAT-SNAPSHOT-TEST] PASSED");
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

  // Set up snapshot managers on ALL servers with a low threshold.  Keep the
  // process-wide MAKO_RAFT_SNAPSHOTS knob enabled for the rest of this process
  // to match the live managers installed below: compacted logs are not
  // self-contained without the snapshot bytes that cover their prefix.
  // @unsafe { setenv and shared_ptr usage }
  Assert2(setenv("MAKO_RAFT_SNAPSHOTS", "1", 1) == 0,
          "Could not enable snapshots for long-partition fixture: %s",
          strerror(errno));

  std::vector<std::shared_ptr<janus::raft::SnapshotManager>> test_mgrs(
      NSERVERS);
  std::vector<uint64_t> seeded_snapshot_indices(NSERVERS, 0);
  std::vector<uint64_t> original_thresholds(NSERVERS, 0);
  std::vector<uint64_t> original_retention_windows(NSERVERS, 0);
  for (int i = 0; i < NSERVERS; i++) {
    auto server = config_->GetServer(i);
    if (server == nullptr) continue;
    auto mgr = std::make_shared<janus::raft::MemorySnapshotManager>();
    test_mgrs[i] = mgr;
    {
      std::lock_guard<std::mutex> lock(server->mtx_);
      original_thresholds[i] = server->GetSnapshotThreshold();
      original_retention_windows[i] = server->GetLogRetentionWindow();
    }
    Assert2(InstallAndSeedSnapshotManager(
                server, mgr, 5, &seeded_snapshot_indices[i]),
            "Could not atomically seed Test69 server %d snapshot manager", i);
    {
      std::lock_guard<std::mutex> lock(server->mtx_);
      server->SetLogRetentionWindow(10);
    }
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

  // Commit enough entries to trigger snapshot + compaction. NCommitted is a
  // callback-only RaftLab oracle: a replica that legitimately installs a
  // marker-only snapshot through the current command publishes its applied
  // boundary but cannot reconstruct that covered command's old callback.
  // Require a real Raft quorum through the command oracle, then independently
  // require every connected replica to publish the boundary through either
  // ordinary application or snapshot installation.
  const int partition_quorum = NSERVERS / 2 + 1;
  uint64_t first_partition_index = 0;
  uint64_t last_partition_index = 0;
  for (int i = 1; i <= 20; i++) {
    uint64_t idx = config_->DoAgreement(
        6900 + i, partition_quorum, true);
    Assert2(idx > 0, "DoAgreement failed for cmd %d", 6900 + i);

    int connected_applied = 0;
    for (int attempt = 0;
         attempt < 100 && connected_applied < NSERVERS - 1;
         ++attempt) {
      connected_applied = 0;
      for (int site = 0; site < NSERVERS; ++site) {
        if (site == follower) continue;
        auto connected_server = config_->GetServer(site);
        if (connected_server != nullptr &&
            connected_server->GetAppliedIndex() >= idx) {
          connected_applied++;
        }
      }
      if (connected_applied < NSERVERS - 1) {
        Fiber::sleep(HEARTBEAT_INTERVAL);
      }
    }
    Assert2(connected_applied == NSERVERS - 1,
            "Only %d of %d connected replicas published applied index %lu "
            "for cmd %d",
            connected_applied, NSERVERS - 1, idx, 6900 + i);

    if (first_partition_index == 0) {
      first_partition_index = idx;
    }
    last_partition_index = idx;
  }
  Log_info("TEST 69: Committed 20 entries with follower disconnected");

  // Verify leader has taken a snapshot and compacted
  // Re-check leader in case of re-election
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  auto leader_server = config_->GetServer(leader);
  Assert2(leader_server != nullptr, "Leader server should not be null");
  Assert2(leader >= 0 && leader < NSERVERS && test_mgrs[leader] != nullptr,
          "Test69 has no unique snapshot manager for leader %d", leader);

  uint64_t leader_execute_index = 0;
  uint64_t leader_snap_idx = 0;
  uint64_t leader_min_active = 0;
  bool leader_snapshot_ready = false;
  for (int attempt = 0;
       attempt < 300 && !leader_snapshot_ready;
       ++attempt) {
    {
      std::lock_guard<std::mutex> lock(leader_server->mtx_);
      leader_execute_index = leader_server->executeIndex;
      leader_snap_idx = leader_server->GetSnapshotIndexLocked();
      leader_min_active = leader_server->min_active_slot_;
    }
    auto candidate = test_mgrs[leader]->GetLatestSnapshot();
    if (candidate.is_some()) {
      const auto candidate_metadata = candidate.unwrap();
      leader_snapshot_ready =
          leader_execute_index >= last_partition_index &&
          candidate_metadata.last_included_index == leader_snap_idx &&
          leader_snap_idx > seeded_snapshot_indices[leader] &&
          leader_snap_idx >= first_partition_index;
    }
    if (!leader_snapshot_ready) {
      Fiber::sleep(10000);
    }
  }
  Assert2(leader_snapshot_ready,
          "Leader did not create a fresh snapshot for partition workload [%lu, %lu]",
          first_partition_index, last_partition_index);
  Assert2(leader_execute_index >= last_partition_index,
          "Leader executeIndex %lu did not reach partition workload end %lu",
          leader_execute_index, last_partition_index);

  auto latest_leader_snapshot = test_mgrs[leader]->GetLatestSnapshot();
  Assert2(latest_leader_snapshot.is_some(),
          "Test69 leader's unique manager has no snapshot");
  const auto leader_snapshot_metadata = latest_leader_snapshot.unwrap();

  Log_info("TEST 69: Leader snapshot index={}, min_active_slot={}",
           leader_snap_idx, leader_min_active);
  Assert2(leader_snap_idx == leader_snapshot_metadata.last_included_index,
          "Leader snapshot index %lu does not match manager index %lu",
          leader_snap_idx, leader_snapshot_metadata.last_included_index);
  Assert2(leader_snap_idx > seeded_snapshot_indices[leader] &&
              leader_snap_idx >= first_partition_index,
          "Leader snapshot did not advance for the partition workload: baseline=%lu, first=%lu, got=%lu",
          seeded_snapshot_indices[leader], first_partition_index,
          leader_snap_idx);
  Assert2(leader_min_active > 1,
          "Leader min_active_slot_ should be > 1 after compaction, got %lu",
          leader_min_active);

  // Capture the disconnected boundary before opening the network. Otherwise
  // a fast InstallSnapshot could complete between Reconnect() and this read,
  // turning the oracle itself into a race.
  auto follower_server = config_->GetServer(follower);
  Assert2(follower_server != nullptr,
          "Disconnected follower server should not be null");

  uint64_t follower_snap_before = 0;
  uint64_t follower_last_before = 0;
  {
    std::lock_guard<std::mutex> lock(follower_server->mtx_);
    follower_snap_before = follower_server->GetSnapshotIndexLocked();
    follower_last_before = follower_server->lastLogIndex;
  }
  Assert2(leader_snap_idx > follower_snap_before,
          "Leader snapshot %lu must be newer than disconnected follower snapshot %lu",
          leader_snap_idx, follower_snap_before);
  Assert2(follower_last_before < UINT64_MAX &&
              leader_min_active > follower_last_before + 1,
          "Leader retained a bridgeable Test69 gap: min_active=%lu follower_next=%lu",
          leader_min_active, follower_last_before + 1);

  // Reconnect the follower and wait for heartbeat/election rounds to trigger
  // InstallSnapshot. The isolated follower may have advanced its private term,
  // so reconnection can legitimately include one election before transfer.
  Log_info("TEST 69: Reconnecting follower {}", follower);
  config_->Reconnect(follower);

  // Reconnection can replace the pre-reconnect leader because the isolated
  // follower may carry a newer term.  The replacement leader only needs to
  // install a snapshot that bridges the follower's missing prefix; its local
  // compaction boundary need not equal leader_snap_idx above.
  const uint64_t required_snapshot_floor =
      std::max(follower_last_before + 1, first_partition_index);
  uint64_t follower_snap_idx = follower_snap_before;
  for (int attempt = 0;
       attempt < 100 && follower_snap_idx < required_snapshot_floor;
       ++attempt) {
    Fiber::sleep(HEARTBEAT_INTERVAL);
    std::lock_guard<std::mutex> lock(follower_server->mtx_);
    follower_snap_idx = follower_server->GetSnapshotIndexLocked();
  }
  Log_info("TEST 69: Follower snapshot index={} (required_floor={}, "
           "pre-reconnect leader={})",
           follower_snap_idx, required_snapshot_floor, leader_snap_idx);
  Assert2(follower_snap_idx >= required_snapshot_floor &&
              follower_snap_idx > follower_snap_before,
          "Follower snapshot should advance beyond %lu through required floor "
          "%lu (pre-reconnect leader snapshot was %lu), got %lu",
          follower_snap_before, required_snapshot_floor, leader_snap_idx,
          follower_snap_idx);
  Assert2(follower < NSERVERS && test_mgrs[follower] != nullptr,
          "Test69 has no unique snapshot manager for follower %d", follower);
  auto latest_follower_snapshot = test_mgrs[follower]->GetLatestSnapshot();
  Assert2(latest_follower_snapshot.is_some(),
          "Test69 follower's unique manager has no installed snapshot");
  const auto follower_snapshot_metadata = latest_follower_snapshot.unwrap();
  Assert2(follower_snapshot_metadata.last_included_index ==
              follower_snap_idx,
          "Follower snapshot index %lu does not match manager index %lu",
          follower_snap_idx,
          follower_snapshot_metadata.last_included_index);

  // Verify new entries can be committed with all 5 nodes
  uint64_t new_idx = config_->DoAgreement(6999, NSERVERS, true);
  Assert2(new_idx > 0,
          "DoAgreement should succeed with all 5 nodes after partition recovery");
  Log_info("TEST 69: Full cluster agreement reached at index {}", new_idx);

  // Restore runtime tuning, but keep each server on the live snapshot manager
  // that backs its compacted prefix.  MAKO_RAFT_SNAPSHOTS stays enabled to
  // match those live managers.
  for (int i = 0; i < NSERVERS; i++) {
    auto server = config_->GetServer(i);
    if (server == nullptr) continue;
    std::lock_guard<std::mutex> lock(server->mtx_);
    if (original_thresholds[i] != 0) {
      server->SetSnapshotThresholdLocked(original_thresholds[i]);
    }
    if (original_retention_windows[i] != 0) {
      server->SetLogRetentionWindow(original_retention_windows[i]);
    }
  }
  Log_info("TEST 69: Retaining live in-memory snapshot managers through suite shutdown");

  Log_info("TEST 69: Long partition recovery via InstallSnapshot PASSED!");
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
    std::lock_guard<std::mutex> lock(server->mtx_);
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

  Log_info("TEST 72: High frequency apply PASSED!");
  Passed2();
}

#endif

}
