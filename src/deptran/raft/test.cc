#include "test.h"
#include <set>
#include <vector>
#include <cstdlib>
#include <ctime>

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
  if (TEST_EXPAND(testPersistence())                      // Test 13
      || TEST_EXPAND(testLeaderFollowerPersistence())      // Test 14
      || TEST_EXPAND(testComprehensiveCrashRecovery())     // Test 15
      || TEST_EXPAND(testPartitionPlusRestart())           // Test 16
      || TEST_EXPAND(testSequentialPartitionsPlusRestart()) // Test 17
      || TEST_EXPAND(testMultipleRestartsPlusPartition()) // Test 18
      || TEST_EXPAND(testFigure8CrashRecovery())           // Test 19
      // Speculative Raft tests (Phase 7)
      || TEST_EXPAND(testSpeculativeLeaderElection())      // Test 20
      || TEST_EXPAND(testSpecCommitIndexAdvances())        // Test 21
      || TEST_EXPAND(testSpeculativeInvariantsHold())      // Test 22
      // testInitialElection()
      // || TEST_EXPAND(testReElection())
      // || TEST_EXPAND(testBasicAgree())
      // || TEST_EXPAND(testFailAgree())
      // || TEST_EXPAND(testFailNoAgree())
      // || TEST_EXPAND(testRejoin())
      // || TEST_EXPAND(testConcurrentStarts())
      // || TEST_EXPAND(testBackup())
      // || TEST_EXPAND(testCount())
      // || TEST_EXPAND(testUnreliableAgree())
      // || TEST_EXPAND(testFigure8())
    ) {
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
        /* Log_info("DoAgreeAndAssertIndex: Starting agreement for command %d with %d servers, expected index %ld", cmd, n, index); */ \
        auto r = config_->DoAgreement(cmd, n, false); \
        auto ind = index; \
        /* Log_info("DoAgreeAndAssertIndex: DoAgreement returned %ld for command %d", r, cmd); */ \
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
  Log_info("TEST 12: Leader elected: %d", leader);

  // Commit some entries
  Log_info("TEST 12: Committing initial entries");
  DoAgreeAndAssertIndex(101, NSERVERS, index_++);
  DoAgreeAndAssertIndex(102, NSERVERS, index_++);
  DoAgreeAndAssertIndex(103, NSERVERS, index_++);
  Log_info("TEST 12: Committed 3 entries");

  // Pick a follower to kill and restart
  siteid_t victim = config_->getNextServerId(leader, 1);
  Log_info("TEST 12: Killing follower %d", victim);

  // Get state before killing
  auto victim_server = config_->GetServer(victim);
  uint64_t term_before = victim_server->currentTerm;
  uint64_t last_log_before = victim_server->lastLogIndex;
  Log_info("TEST 12: Before kill - term=%lu, lastLogIndex=%lu", term_before, last_log_before);

  // Kill the server
  config_->Kill(victim);
  Log_info("TEST 12: Server %d killed", victim);

  // Sleep to ensure it's really gone
  Fiber::sleep(ELECTIONTIMEOUT / 2);

  // Commit more entries with remaining servers
  Log_info("TEST 12: Committing entries with %d servers", NSERVERS - 1);
  DoAgreeAndAssertIndex(104, NSERVERS - 1, index_++);
  DoAgreeAndAssertIndex(105, NSERVERS - 1, index_++);
  Log_info("TEST 12: Committed 2 more entries");

  // Restart the killed server
  Log_info("TEST 12: Restarting server %d", victim);
  config_->Restart(victim);
  Log_info("TEST 12: Server %d restarted", victim);

  // Give it time to catch up
  Fiber::sleep(ELECTIONTIMEOUT);

  Log_info("TEST 12: After Sleep for ELECTIONTIMEOUT");

  // Verify the restarted server recovered its state
  victim_server = config_->GetServer(victim);
  uint64_t term_after = victim_server->currentTerm;
  uint64_t last_log_after = victim_server->lastLogIndex;
  Log_info("TEST 12: After restart - term=%lu, lastLogIndex=%lu", term_after, last_log_after);

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
  Log_info("TEST 12: Committing with all %d servers", NSERVERS);
  DoAgreeAndAssertWaitSuccess(106, NSERVERS);
  Log_info("TEST 12: Final commit successful");

  // Now test killing and restarting the leader
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 12: Testing leader kill - current leader is %d", leader);

  // Get leader state before killing
  auto leader_server = config_->GetServer(leader);
  term_before = leader_server->currentTerm;
  last_log_before = leader_server->lastLogIndex;
  Log_info("TEST 12: Leader before kill - term=%lu, lastLogIndex=%lu", term_before, last_log_before);

  // Kill the leader
  config_->Kill(leader);
  Log_info("TEST 12: Leader %d killed", leader);

  // Wait for new leader election
  Fiber::sleep(ELECTIONTIMEOUT);
  int new_leader = config_->OneLeader();
  AssertOneLeader(new_leader);
  AssertReElection(new_leader, leader);
  Log_info("TEST 12: New leader elected: %d", new_leader);

  // Commit entries with new leader
  DoAgreeAndAssertIndex(107, NSERVERS - 1, index_++);
  Log_info("TEST 12: Committed entry with new leader");

  // Restart the old leader
  Log_info("TEST 12: Restarting old leader %d", leader);
  config_->Restart(leader);
  Log_info("TEST 12: Old leader %d restarted", leader);

  // Give it time to catch up
  Fiber::sleep(ELECTIONTIMEOUT);

  // Verify old leader recovered
  leader_server = config_->GetServer(leader);
  term_after = leader_server->currentTerm;
  last_log_after = leader_server->lastLogIndex;
  Log_info("TEST 12: Old leader after restart - term=%lu, lastLogIndex=%lu", term_after, last_log_after);

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
  Log_info("TEST 13: Leader elected: %d", leader);

  // Commit some entries
  Log_info("TEST 13: Committing initial entries");
  DoAgreeAndAssertIndex(1301, NSERVERS, index_++);
  DoAgreeAndAssertIndex(1302, NSERVERS, index_++);
  DoAgreeAndAssertIndex(1303, NSERVERS, index_++);
  Log_info("TEST 13: Committed 3 entries");

  // Pick two followers to kill
  siteid_t victim1 = config_->getNextServerId(leader, 1);
  siteid_t victim2 = config_->getNextServerId(leader, 2);
  Log_info("TEST 13: Killing two followers: %d and %d", victim1, victim2);

  // Get state before killing
  auto victim1_server = config_->GetServer(victim1);
  auto victim2_server = config_->GetServer(victim2);
  uint64_t term_before1 = victim1_server->currentTerm;
  uint64_t term_before2 = victim2_server->currentTerm;
  uint64_t last_log_before1 = victim1_server->lastLogIndex;
  uint64_t last_log_before2 = victim2_server->lastLogIndex;
  Log_info("TEST 13: Victim1 before kill - term=%lu, lastLogIndex=%lu", term_before1, last_log_before1);
  Log_info("TEST 13: Victim2 before kill - term=%lu, lastLogIndex=%lu", term_before2, last_log_before2);

  // Kill both servers
  config_->Kill(victim1);
  Log_info("TEST 13: Server %d killed", victim1);
  config_->Kill(victim2);
  Log_info("TEST 13: Server %d killed", victim2);

  // Sleep to ensure they're really gone
  Fiber::sleep(ELECTIONTIMEOUT / 2);

  // We still have quorum (3 out of 5), commit more entries
  Log_info("TEST 13: Committing entries with %d servers", NSERVERS - 2);
  DoAgreeAndAssertIndex(1304, NSERVERS - 2, index_++);
  DoAgreeAndAssertIndex(1305, NSERVERS - 2, index_++);
  Log_info("TEST 13: Committed 2 more entries with reduced cluster");

  // Restart victim1 first
  Log_info("TEST 13: Restarting server %d", victim1);
  config_->Restart(victim1);
  Log_info("TEST 13: Server %d restarted", victim1);

  // Give it time to catch up
  Fiber::sleep(ELECTIONTIMEOUT / 2);

  // Restart victim2
  Log_info("TEST 13: Restarting server %d", victim2);
  config_->Restart(victim2);
  Log_info("TEST 13: Server %d restarted", victim2);

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
  Log_info("TEST 13: Victim1 after restart - term=%lu, lastLogIndex=%lu", term_after1, last_log_after1);
  Log_info("TEST 13: Victim2 after restart - term=%lu, lastLogIndex=%lu", term_after2, last_log_after2);

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
  Log_info("TEST 13: Committing with all %d servers", NSERVERS);
  DoAgreeAndAssertWaitSuccess(1306, NSERVERS);
  Log_info("TEST 13: Final commit successful with all servers");

  // Verify leader is still stable
  int final_leader = config_->OneLeader();
  AssertOneLeader(final_leader);
  Log_info("TEST 13: Leader after all restarts: %d", final_leader);

  Passed2();
}

int RaftLabTest::testLeaderFollowerPersistence(void) {
  Init2(14, "Persistence across leader + follower kill and restart");

  Log_info("TEST 14: Waiting for initial election");
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 14: Leader elected: %d", leader);

  // Commit some entries
  Log_info("TEST 14: Committing initial entries");
  DoAgreeAndAssertIndex(1401, NSERVERS, index_++);
  DoAgreeAndAssertIndex(1402, NSERVERS, index_++);
  DoAgreeAndAssertIndex(1403, NSERVERS, index_++);
  Log_info("TEST 14: Committed 3 entries");

  // Pick one follower to kill along with the leader
  siteid_t victim_follower = config_->getNextServerId(leader, 1);
  siteid_t victim_leader = leader;
  Log_info("TEST 14: Killing leader %d and follower %d", victim_leader, victim_follower);

  // Get state before killing
  auto leader_server = config_->GetServer(victim_leader);
  auto follower_server = config_->GetServer(victim_follower);
  uint64_t term_before_leader = leader_server->currentTerm;
  uint64_t term_before_follower = follower_server->currentTerm;
  uint64_t last_log_before_leader = leader_server->lastLogIndex;
  uint64_t last_log_before_follower = follower_server->lastLogIndex;
  Log_info("TEST 14: Leader before kill - term=%lu, lastLogIndex=%lu", term_before_leader, last_log_before_leader);
  Log_info("TEST 14: Follower before kill - term=%lu, lastLogIndex=%lu", term_before_follower, last_log_before_follower);

  // Kill both servers (leader first, then follower)
  config_->Kill(victim_leader);
  Log_info("TEST 14: Leader %d killed", victim_leader);
  config_->Kill(victim_follower);
  Log_info("TEST 14: Follower %d killed", victim_follower);

  // Wait for new leader election among remaining 3 servers
  Log_info("TEST 14: Waiting for new leader election");
  Fiber::sleep(ELECTIONTIMEOUT);

  int new_leader = config_->OneLeader();
  AssertOneLeader(new_leader);
  Assert2(new_leader != victim_leader, "new leader should not be the killed leader");
  Assert2(new_leader != victim_follower, "new leader should not be the killed follower");
  Log_info("TEST 14: New leader elected: %d", new_leader);

  // We still have quorum (3 out of 5), commit more entries
  Log_info("TEST 14: Committing entries with %d servers", NSERVERS - 2);
  DoAgreeAndAssertIndex(1404, NSERVERS - 2, index_++);
  DoAgreeAndAssertIndex(1405, NSERVERS - 2, index_++);
  Log_info("TEST 14: Committed 2 more entries with reduced cluster");

  // Restart the follower first
  Log_info("TEST 14: Restarting follower %d", victim_follower);
  config_->Restart(victim_follower);
  Log_info("TEST 14: Follower %d restarted", victim_follower);

  // Give it time to catch up
  Fiber::sleep(ELECTIONTIMEOUT / 2);

  // Restart the old leader
  Log_info("TEST 14: Restarting old leader %d", victim_leader);
  config_->Restart(victim_leader);
  Log_info("TEST 14: Old leader %d restarted", victim_leader);

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
  Log_info("TEST 14: Old leader after restart - term=%lu, lastLogIndex=%lu", term_after_leader, last_log_after_leader);
  Log_info("TEST 14: Follower after restart - term=%lu, lastLogIndex=%lu", term_after_follower, last_log_after_follower);

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
  Log_info("TEST 14: Committing with all %d servers", NSERVERS);
  DoAgreeAndAssertWaitSuccess(1406, NSERVERS);
  Log_info("TEST 14: Final commit successful with all servers");

  // Verify we have a stable leader
  int final_leader = config_->OneLeader();
  AssertOneLeader(final_leader);
  Log_info("TEST 14: Leader after all restarts: %d", final_leader);

  Passed2();
}

int RaftLabTest::testComprehensiveCrashRecovery(void) {
  Init2(15, "Comprehensive crash-recovery with random server selection");

  Log_info("TEST 15: Waiting for initial election");
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 15: Initial leader elected: %d", leader);

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
    Log_info("TEST 15: ===== ROUND %d =====", round);

    // Phase 1: Kill 2 random servers (maintain quorum with 3 remaining)
    Log_info("TEST 15: Phase 1 - Killing 2 random servers");

    siteid_t victim1 = pickRandom(alive_servers);
    alive_servers.erase(victim1);
    dead_servers.insert(victim1);

    siteid_t victim2 = pickRandom(alive_servers);
    alive_servers.erase(victim2);
    dead_servers.insert(victim2);

    Log_info("TEST 15: Round %d - Killing servers %d and %d", round, victim1, victim2);
    config_->Kill(victim1);
    config_->Kill(victim2);

    // Wait for potential leader election if we killed the leader
    Fiber::sleep(ELECTIONTIMEOUT);

    // Verify we still have a leader among surviving servers
    leader = config_->OneLeader();
    AssertOneLeader(leader);
    Assert2(alive_servers.count(leader) > 0, "Leader %d should be among alive servers", leader);
    Log_info("TEST 15: Round %d - Leader after kills: %d", round, leader);

    // Commit with 3 servers (quorum)
    Log_info("TEST 15: Round %d - Committing with %zu alive servers", round, alive_servers.size());
    DoAgreeAndAssertIndex(cmd_base++, (int)alive_servers.size(), index_++);

    // Phase 2: Restart one of the dead servers
    Log_info("TEST 15: Phase 2 - Restarting one dead server");

    siteid_t restart1 = pickRandom(dead_servers);
    dead_servers.erase(restart1);
    alive_servers.insert(restart1);

    Log_info("TEST 15: Round %d - Restarting server %d", round, restart1);
    config_->Restart(restart1);

    // Wait for it to catch up
    Fiber::sleep(ELECTIONTIMEOUT);

    // Verify leader and commit
    leader = config_->OneLeader();
    AssertOneLeader(leader);
    Log_info("TEST 15: Round %d - Leader after restart1: %d", round, leader);

    DoAgreeAndAssertIndex(cmd_base++, (int)alive_servers.size(), index_++);

    // Phase 3: Kill another random alive server (back to 3 alive)
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

    Log_info("TEST 15: Round %d - Killing server %d (was leader: %s)",
             round, victim3, victim3 == leader ? "yes" : "no");
    config_->Kill(victim3);

    // Wait for potential leader election
    Fiber::sleep(ELECTIONTIMEOUT);

    leader = config_->OneLeader();
    AssertOneLeader(leader);
    Log_info("TEST 15: Round %d - Leader after kill3: %d", round, leader);

    // Commit with remaining servers
    DoAgreeAndAssertIndex(cmd_base++, (int)alive_servers.size(), index_++);

    // Phase 4: Restart all dead servers
    Log_info("TEST 15: Phase 4 - Restarting all dead servers");

    std::vector<siteid_t> to_restart(dead_servers.begin(), dead_servers.end());
    for (siteid_t svr : to_restart) {
      Log_info("TEST 15: Round %d - Restarting server %d", round, svr);
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
    Log_info("TEST 15: Round %d - Leader after all restarts: %d", round, leader);

    Assert2(alive_servers.size() == NSERVERS,
            "Expected %d alive servers, got %zu", NSERVERS, alive_servers.size());
    Assert2(dead_servers.empty(),
            "Expected 0 dead servers, got %zu", dead_servers.size());

    // Final commit with all servers
    Log_info("TEST 15: Round %d - Final commit with all %d servers", round, NSERVERS);
    DoAgreeAndAssertWaitSuccess(cmd_base++, NSERVERS);

    Log_info("TEST 15: ===== ROUND %d COMPLETE =====", round);
  }

  // Final verification
  Log_info("TEST 15: Final verification after %d rounds", NUM_ROUNDS);

  leader = config_->OneLeader();
  AssertOneLeader(leader);

  // One more commit to verify everything works
  DoAgreeAndAssertWaitSuccess(cmd_base++, NSERVERS);

  Log_info("TEST 15: All %d rounds completed successfully!", NUM_ROUNDS);

  Passed2();
}

int RaftLabTest::testPartitionPlusRestart(void) {
  Init2(16, "Partition plus restart - one server partitioned, another killed/restarted");

  Log_info("TEST 16: Waiting for initial election");
  Fiber::sleep(ELECTIONTIMEOUT);
  int leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 16: Initial leader elected: %d", leader);

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

  Log_info("TEST 16: Will partition server %d and kill/restart server %d",
           partitioned_server, killed_server);

  // Step 1: Partition server A
  Log_info("TEST 16: Step 1 - Partitioning server %d", partitioned_server);
  config_->Disconnect(partitioned_server);

  // Step 2: Kill server B
  Log_info("TEST 16: Step 2 - Killing server %d", killed_server);
  config_->Kill(killed_server);

  // Wait for potential leader re-election (if we killed/partitioned the leader)
  Fiber::sleep(ELECTIONTIMEOUT);

  // Verify cluster still works with 3 servers (quorum)
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 16: Leader after partition+kill: %d", leader);

  // Commit with 3 servers
  Log_info("TEST 16: Committing with 3 servers");
  DoAgreeAndAssertIndex(1603, 3, index_++);

  // Step 3: Restart server B (while A is still partitioned)
  Log_info("TEST 16: Step 3 - Restarting server %d (while %d is still partitioned)",
           killed_server, partitioned_server);
  config_->Restart(killed_server);

  // Wait for server B to catch up
  Fiber::sleep(ELECTIONTIMEOUT);

  // Verify cluster works with 4 servers
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 16: Leader after restart: %d", leader);

  // Commit with 4 servers
  Log_info("TEST 16: Committing with 4 servers");
  DoAgreeAndAssertIndex(1604, 4, index_++);

  // Step 4: Heal partition (reconnect server A)
  // This is the critical test: A was partitioned when B restarted
  // A's connection to B should be stale, but B's retry mechanism should fix it
  Log_info("TEST 16: Step 4 - Healing partition (reconnecting server %d)", partitioned_server);
  config_->Reconnect(partitioned_server);

  // Wait for server A to catch up and for NotifyRestart retry to work
  Log_info("TEST 16: Waiting for partition to heal and connections to refresh...");
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  // Verify all 5 servers are working
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 16: Leader after partition heal: %d", leader);

  // Step 5: Final commit with all 5 servers
  // This verifies that A can communicate with B (the restarted server)
  Log_info("TEST 16: Step 5 - Final commit with all %d servers", NSERVERS);
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
  Log_info("TEST 17: Initial leader elected: %d", leader);

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
  Log_info("TEST 17: Server A (partition first): %d", server_A);
  Log_info("TEST 17: Server B (kill/restart): %d", server_B);
  Log_info("TEST 17: Server C (partition second): %d", server_C);

  // ========================================
  // T1: Partition A → Healthy: {B,C,D,E} = 4
  // ========================================
  Log_info("TEST 17: Step 1 - Partitioning server A (%d)", server_A);
  config_->Disconnect(server_A);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 17: Leader after partitioning A: %d", leader);

  // Commit with 4 servers
  Log_info("TEST 17: Committing with 4 servers (A partitioned)");
  DoAgreeAndAssertIndex(1703, 4, index_++);

  // ========================================
  // T2: Kill B → Healthy: {C,D,E} = 3
  // ========================================
  Log_info("TEST 17: Step 2 - Killing server B (%d)", server_B);
  config_->Kill(server_B);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 17: Leader after killing B: %d", leader);

  // Commit with 3 servers
  Log_info("TEST 17: Committing with 3 servers (A partitioned, B dead)");
  DoAgreeAndAssertIndex(1704, 3, index_++);

  // ========================================
  // T3: Restart B → B sends NotifyRestart, A is PENDING
  // ========================================
  Log_info("TEST 17: Step 3 - Restarting server B (%d) while A (%d) is still partitioned",
           server_B, server_A);
  config_->Restart(server_B);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 17: Leader after restarting B: %d", leader);

  // Commit with 4 servers (B is back, A still partitioned)
  Log_info("TEST 17: Committing with 4 servers (A partitioned, B restarted)");
  DoAgreeAndAssertIndex(1705, 4, index_++);

  // ========================================
  // T4: Partition C → Healthy: {B,D,E} = 3 (A and C both isolated)
  // ========================================
  Log_info("TEST 17: Step 4 - Partitioning server C (%d) while A (%d) is still partitioned",
           server_C, server_A);
  config_->Disconnect(server_C);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 17: Leader after partitioning C: %d", leader);

  // Commit with 3 servers (A and C partitioned)
  Log_info("TEST 17: Committing with 3 servers (A and C partitioned)");
  DoAgreeAndAssertIndex(1706, 3, index_++);

  // ========================================
  // T5: Heal A → A has stale connection to B, B's retry fixes it
  // ========================================
  Log_info("TEST 17: Step 5 - Healing partition for server A (%d)", server_A);
  Log_info("TEST 17: A was partitioned when B restarted, so A has stale connection to B");
  config_->Reconnect(server_A);

  // Wait for A to catch up and for B's retry mechanism to fix A's stale connection
  Log_info("TEST 17: Waiting for A to reconnect and B's retry to fix stale connection...");
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 17: Leader after healing A: %d", leader);

  // Commit with 4 servers (A is back, C still partitioned)
  Log_info("TEST 17: Committing with 4 servers (A healed, C still partitioned)");
  DoAgreeAndAssertIndex(1707, 4, index_++);

  // ========================================
  // T6: Heal C → C has stale connection to B, B's retry fixes it
  // ========================================
  Log_info("TEST 17: Step 6 - Healing partition for server C (%d)", server_C);
  Log_info("TEST 17: C was partitioned when B restarted, so C has stale connection to B");
  config_->Reconnect(server_C);

  // Wait for C to catch up and for B's retry mechanism to fix C's stale connection
  Log_info("TEST 17: Waiting for C to reconnect and B's retry to fix stale connection...");
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 17: Leader after healing C: %d", leader);

  // ========================================
  // T7: Verify all 5 servers work
  // ========================================
  Log_info("TEST 17: Step 7 - Final verification with all %d servers", NSERVERS);
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
  Log_info("TEST 18: Initial leader elected: %d", leader);

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
  Log_info("TEST 18: Server A (partition): %d", server_A);
  Log_info("TEST 18: Server B (multiple restarts): %d", server_B);
  Log_info("TEST 18: Server C (single restart): %d", server_C);

  // ========================================
  // T1: Partition A → Healthy: {B,C,D,E} = 4
  // ========================================
  Log_info("TEST 18: Step 1 - Partitioning server A (%d)", server_A);
  config_->Disconnect(server_A);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 18: Leader after partitioning A: %d", leader);

  DoAgreeAndAssertIndex(1803, 4, index_++);

  // ========================================
  // T2: Kill B → Healthy: {C,D,E} = 3
  // ========================================
  Log_info("TEST 18: Step 2 - Killing server B (%d) [first time]", server_B);
  config_->Kill(server_B);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 18: Leader after killing B: %d", leader);

  DoAgreeAndAssertIndex(1804, 3, index_++);

  // ========================================
  // T3: Restart B → B sends NotifyRestart, A is PENDING
  // ========================================
  Log_info("TEST 18: Step 3 - Restarting server B (%d) [first time]", server_B);
  config_->Restart(server_B);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 18: Leader after restarting B [first time]: %d", leader);

  DoAgreeAndAssertIndex(1805, 4, index_++);

  // ========================================
  // T4: Kill B again → Healthy: {C,D,E} = 3
  //     B's retry state is lost!
  // ========================================
  Log_info("TEST 18: Step 4 - Killing server B (%d) [second time] - retry state will be lost!", server_B);
  config_->Kill(server_B);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 18: Leader after killing B [second time]: %d", leader);

  DoAgreeAndAssertIndex(1806, 3, index_++);

  // ========================================
  // T5: Restart B again → B sends NotifyRestart again, A still PENDING
  // ========================================
  Log_info("TEST 18: Step 5 - Restarting server B (%d) [second time]", server_B);
  config_->Restart(server_B);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 18: Leader after restarting B [second time]: %d", leader);

  DoAgreeAndAssertIndex(1807, 4, index_++);

  // ========================================
  // T6: Heal A → A receives NotifyRestart from B, fixes stale connection
  // ========================================
  Log_info("TEST 18: Step 6 - Healing partition for server A (%d)", server_A);
  Log_info("TEST 18: A was partitioned through TWO restart cycles of B");
  config_->Reconnect(server_A);

  // Wait for A to catch up and for B's retry mechanism to fix A's stale connection
  Log_info("TEST 18: Waiting for A to reconnect and B's retry to fix stale connection...");
  Fiber::sleep(ELECTIONTIMEOUT * 2);

  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 18: Leader after healing A: %d", leader);

  // Verify all 5 servers work
  Log_info("TEST 18: Verifying all 5 servers work after A healed");
  DoAgreeAndAssertWaitSuccess(1808, NSERVERS);

  // ========================================
  // T7: Kill C → Healthy: {A,B,D,E} = 4
  // ========================================
  Log_info("TEST 18: Step 7 - Killing server C (%d)", server_C);
  config_->Kill(server_C);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 18: Leader after killing C: %d", leader);

  DoAgreeAndAssertIndex(1809, 4, index_++);

  // ========================================
  // T8: Restart C → C notifies all (all respond since everyone is connected)
  // ========================================
  Log_info("TEST 18: Step 8 - Restarting server C (%d)", server_C);
  config_->Restart(server_C);

  Fiber::sleep(ELECTIONTIMEOUT);
  leader = config_->OneLeader();
  AssertOneLeader(leader);
  Log_info("TEST 18: Leader after restarting C: %d", leader);

  // ========================================
  // T9: Verify all 5 servers work
  // ========================================
  Log_info("TEST 18: Step 9 - Final verification with all %d servers", NSERVERS);
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
  // Log_info("Current leader: %d", leader);
  // Log_info("Current term: %ld", term);
  // Log_info("init_rpcs_ value: %ld", init_rpcs_);
  // Log_info("index_ value: %ld", index_);
  // Log_info("All servers connected: %s", config_->NDisconnected() == 0 ? "true" : "false");
  // Log_info("Network reliable: %s", !config_->IsUnreliable() ? "true" : "false");
  // Log_info("==========================================================");
  
  Passed2();
}

int RaftLabTest::testReElection(void) {
  Init2(2, "Re-election after network failure");
  // Log_info("TEST 2: Starting re-election test");
  
  // find current leader
  // Log_info("TEST 2: Finding current leader");
  int leader = config_->OneLeader();
  // Log_info("TEST 2: Current leader is %d", leader);
  
  // Check if OneLeader returned a valid leader
  if (leader == -1) {
    // Log_info("TEST 2: No leader found, test cannot proceed");
    Failed("No leader found in initial election");
    return -1;
  }
  
  AssertOneLeader(leader);
  
  // disconnect leader - make sure a new one is elected
  // Log_info("TEST 2: Disconnecting old leader %d", leader);
  config_->Disconnect(leader);
  int oldLeader = leader;
  // Log_info("TEST 2: Old leader %d disconnected, sleeping for election timeout", oldLeader);
  Fiber::sleep(ELECTIONTIMEOUT);
  
  // Log_info("TEST 2: Finding new leader after old leader disconnected");
  leader = config_->OneLeader();
  // Log_info("TEST 2: New leader is %d", leader);
  
  // Check if OneLeader returned a valid leader
  if (leader == -1) {
    // Log_info("TEST 2: No new leader found after disconnecting old leader");
    Failed("No new leader elected after disconnecting old leader");
    return -1;
  }
  
  AssertOneLeader(leader);
  AssertReElection(leader, oldLeader);
  
  // reconnect old leader - should not disturb new leader
  // Log_info("TEST 2: Reconnecting old leader %d", oldLeader);
  config_->Reconnect(oldLeader);
  // Log_info("TEST 2: Old leader reconnected, sleeping for election timeout");
  Fiber::sleep(ELECTIONTIMEOUT);
  AssertOneLeader(config_->OneLeader(leader));
  
  // no quorum -> no leader
  // Log_info("TEST 2: Disconnecting more servers to break quorum");
  // Log_info("TEST 2: Current leader is %d", leader);
  
  siteid_t next1 = config_->getNextServerId(leader, 1);
  // Log_info("TEST 2: Next server 1 offset from leader %d is %d", leader, next1);
  config_->Disconnect(next1);
  
  siteid_t next2 = config_->getNextServerId(leader, 2);
  // Log_info("TEST 2: Next server 2 offset from leader %d is %d", leader, next2);
  config_->Disconnect(next2);
  
  // Log_info("TEST 2: Disconnecting leader %d", leader);
  config_->Disconnect(leader);
  
  // Log_info("TEST 2: Checking for no leader condition");
  Assert(config_->NoLeader());
  
  // quorum restored
  // Log_info("TEST 2: Reconnecting a server to restore quorum");
  siteid_t reconnect_server = config_->getNextServerId(leader, 2);
  // Log_info("TEST 2: Reconnecting server %d", reconnect_server);
  config_->Reconnect(reconnect_server);
  Fiber::sleep(ELECTIONTIMEOUT);
  AssertOneLeader(config_->OneLeader());
  
  // rejoin all servers
  // Log_info("TEST 2: Rejoining all servers");
  siteid_t rejoin1 = config_->getNextServerId(leader, 1);
  // Log_info("TEST 2: Rejoining server %d", rejoin1);
  config_->Reconnect(rejoin1);
  
  // Log_info("TEST 2: Rejoining leader %d", leader);
  config_->Reconnect(leader);
  Fiber::sleep(ELECTIONTIMEOUT);
  AssertOneLeader(config_->OneLeader());
  
  // Log carryover context after test 2
  // Log_info("=== CARRYOVER CONTEXT AFTER TEST 2 (testReElection) ===");
  // int final_leader = config_->OneLeader();
  // uint64_t final_term = config_->OneTerm();
  // Log_info("Current leader: %d", final_leader);
  // Log_info("Current term: %ld", final_term);
  // Log_info("init_rpcs_ value: %ld", init_rpcs_);
  // Log_info("index_ value: %ld", index_);
  // Log_info("All servers connected: %s", config_->NDisconnected() == 0 ? "true" : "false");
  // Log_info("Network reliable: %s", !config_->IsUnreliable() ? "true" : "false");
  // Log_info("==========================================================");
  
  Passed2();
}

int RaftLabTest::testBasicAgree(void) {
  Init2(3, "Basic agreement");
  
  // Log carryover context at start of test 3
  // Log_info("=== CARRYOVER CONTEXT AT START OF TEST 3 (testBasicAgree) ===");
  int current_leader = config_->OneLeader();
  uint64_t current_term = config_->OneTerm();
  // Log_info("Current leader: %d", current_leader);
  // Log_info("Current term: %ld", current_term);
  // Log_info("init_rpcs_ value: %ld", init_rpcs_);
  // Log_info("index_ value: %ld", index_);
  // Log_info("All servers connected: %s", config_->NDisconnected() == 0 ? "true" : "false");
  // Log_info("Network reliable: %s", !config_->IsUnreliable() ? "true" : "false");
  // Log_info("=============================================================");
  
  for (int i = 1; i <= 3; i++) {
    // make sure no commits exist before any agreements are started
    AssertNoneCommitted(index_);
    // complete 1 agreement and make sure its index is as expected
    int temp_index = index_;
    int command_value = (int)(temp_index + 300);
    // Log_info("TEST 3: About to test agreement for command %d (iteration %d/3)", command_value, i);
    // Log_info("Starting Agreement for command %d", command_value);
    DoAgreeAndAssertIndex(command_value, NSERVERS, index_);
    index_++;
    // Log_info("Agreement for command %d completed", command_value);
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
  Assert2(init_rpcs_ > 1 && init_rpcs_ <= 40,
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
    Log_info("TEST 19: Attempt %d", again + 1);

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
    Log_info("TEST 19: Initial entry committed at index %ld, term %ld", index1, term1);

    // 2. Kill 3 followers, leaving S1 + 1 follower (S2)
    //    S2 is getNextServerId(leader1, 4) to match original Figure 8 structure
    siteid_t follower_s2 = config_->getNextServerId(leader1, 4);
    siteid_t killed1 = config_->getNextServerId(leader1, 1);
    siteid_t killed2 = config_->getNextServerId(leader1, 2);
    siteid_t killed3 = config_->getNextServerId(leader1, 3);

    Log_info("TEST 19: Killing 3 followers: %d, %d, %d (keeping leader %d and follower %d)",
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
    Log_info("TEST 19: Started C1 (1901) at index %ld, term %ld - should NOT be committed", index1, term1);
    Fiber::sleep(ELECTIONTIMEOUT);

    // C1 is at index1 for S1 and S2, but NOT committed (only 2 servers)
    AssertNoneCommitted(index1);
    Log_info("TEST 19: Verified C1 is not committed (only on 2 servers)");

    // 4. Kill S1 and S2, restart the other 3 to elect new leader S3
    Log_info("TEST 19: Killing S1 (%d) and S2 (%d), restarting other 3", leader1, follower_s2);
    config_->Kill(leader1);
    config_->Kill(follower_s2);
    config_->Restart(killed1);
    config_->Restart(killed2);
    config_->Restart(killed3);

    // 5. New leader S3 elected among the 3 restarted servers
    Fiber::sleep(ELECTIONTIMEOUT);
    auto leader2 = config_->OneLeader();
    AssertOneLeader(leader2);
    Log_info("TEST 19: New leader S3 elected: %d", leader2);

    // 6. Restart S1 and S2 - they recover from disk with C1 in their logs
    Log_info("TEST 19: Restarting S1 (%d) and S2 (%d) - they should recover C1 from disk",
             leader1, follower_s2);
    config_->Restart(leader1);
    config_->Restart(follower_s2);
    Fiber::sleep(ELECTIONTIMEOUT);

    // S3 should still be leader
    int current_leader = config_->OneLeader();
    AssertOneLeader(current_leader);
    Log_info("TEST 19: Current leader after S1/S2 restart: %d", current_leader);

    // 7. Kill all except S3, have S3 start C2 at the same index as C1
    //    We know exactly which servers exist: leader1, follower_s2, killed1, killed2, killed3
    //    One of killed1/killed2/killed3 is now leader2, so we kill the others
    Log_info("TEST 19: Isolating leader S3 (%d) by killing all others", leader2);

    // Kill using explicit server IDs we tracked, not getServerIdByIndex
    std::vector<siteid_t> all_servers = {leader1, follower_s2, killed1, killed2, killed3};
    std::vector<siteid_t> killed_in_step7;
    for (siteid_t svr : all_servers) {
      if (svr != leader2) {
        Log_info("TEST 19: Step 7 - Killing server %d", svr);
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
    Log_info("TEST 19: Started C2 (1902) at index %ld, term %ld", index2, term2);
    Assert2(index2 == index1, "Start() returned index %ld (%ld expected)", index2, index1);
    Assert2(term2 > term1, "Start() returned term %ld (expected > %ld)", term2, term1);
    Fiber::sleep(ELECTIONTIMEOUT);
    AssertNoneCommitted(index1);
    Log_info("TEST 19: Verified neither C1 nor C2 is committed yet");

    // 8. Kill S3, restart S1 (has C1), S2 (has C1), and one other
    //    S1 or S2 should become leader because they have C1 (longer log)
    Log_info("TEST 19: Killing S3 (%d), restarting S1, S2, and one other", leader2);
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
        Log_info("TEST 19: Also restarted server %d as third member", svr);
        break;
      }
    }

    Fiber::sleep(ELECTIONTIMEOUT);
    auto leader3 = config_->OneLeader();
    AssertOneLeader(leader3);
    Log_info("TEST 19: New leader after S3 killed: %d", leader3);

    // Leader3 should ideally be S1 or S2 (they have longer logs with C1)
    // But if not, we retry
    if (leader3 != leader1 && leader3 != follower_s2) {
      Log_info("TEST 19: Leader %d is not S1 or S2, retrying (1/3 chance)", leader3);
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
    Log_info("TEST 19: New entry committed at index %ld", new_index);

    // 11. Now C1 SHOULD be committed (indirectly, by the new commit in current term)
    AssertNCommitted(index1, NSERVERS - 2);
    Assert2(config_->ServerCommitted(leader3, index1, 1901),
            "value 1901 not committed at index %ld when it should be", index1);
    Log_info("TEST 19: Verified C1 (1901) is now committed at index %ld", index1);

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
  Reactor::create_sp_event<TimeoutEvent>(microseconds)->wait();
}

// ============================================================================
// SPECULATIVE RAFT TESTS (Phase 7)
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
  Log_info("[SPEC-TEST] Leader elected: index=%d, site_id=%d", leader, leader_id);

  // Check initial speculative state
  // Note: By the time we check, VoteDurable messages may have already arrived
  // So we can't assert securedLeader == false here. Instead, check that
  // the speculative state accessors work and invariants hold.

  size_t specVoters = config_->GetSpecVotersCount(leader_id);
  size_t durableVoters = config_->GetDurableVotersCount(leader_id);

  Log_info("[SPEC-TEST] Leader %d: specVoters=%zu, durableVoters=%zu",
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

  Log_info("[SPEC-TEST] After waiting: secured=%d, durableVoters=%zu",
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

  Log_info("[SPEC-TEST] Initial state: specCommitIndex=%lu, securedLogIndex=%lu",
           initialSpecCommit, initialSecuredLog);

  // Submit an entry
  int cmd = 100 + rand() % 1000;
  uint64_t index = 0;
  uint64_t term = 0;
  bool ok = config_->Start(leader_id, cmd, &index, &term);
  Assert2(ok, "Failed to submit command to leader");

  Log_info("[SPEC-TEST] Submitted command %d at index %lu, term %lu", cmd, index, term);

  // Wait a short time for memory acks to arrive
  Fiber::sleep(200000);  // 200ms

  // Check specCommitIndex advanced
  uint64_t newSpecCommit = config_->GetSpecCommitIndex(leader_id);

  Log_info("[SPEC-TEST] After waiting: specCommitIndex=%lu (was %lu)",
           newSpecCommit, initialSpecCommit);

  // specCommitIndex should have advanced (at least to our submitted entry)
  Assert2(newSpecCommit >= index, "specCommitIndex (%lu) did not reach submitted index (%lu)",
          newSpecCommit, index);

  // Wait longer for durable commit
  Fiber::sleep(500000);  // 500ms more

  // Check that securedLogIndex also advances (if leader is secured)
  bool secured = config_->IsSecuredLeader(leader_id);
  uint64_t newSecuredLog = config_->GetSecuredLogIndex(leader_id);

  Log_info("[SPEC-TEST] After more waiting: secured=%d, securedLogIndex=%lu (was %lu)",
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

    Log_info("[SPEC-TEST] Submitted command %d at index %lu", cmd, index);

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

  Log_info("[SPEC-TEST] Final state: securedLogIndex=%lu, specCommitIndex=%lu, lastLogIndex=%lu",
           securedLog, specCommit, lastLog);

  Assert2(securedLog <= specCommit, "securedLogIndex (%lu) > specCommitIndex (%lu)",
          securedLog, specCommit);
  Assert2(specCommit <= lastLog, "specCommitIndex (%lu) > lastLogIndex (%lu)",
          specCommit, lastLog);

  Passed2();
}

#endif

}
