#include "test.h"
#include <set>
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
  if (// testPersistence()
      // || testTwoFollowerPersistence()
      // || testLeaderFollowerPersistence()
      testComprehensiveCrashRecovery()
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
  Coroutine::Sleep(ELECTIONTIMEOUT);
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
  Coroutine::Sleep(ELECTIONTIMEOUT / 2);

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
  Coroutine::Sleep(ELECTIONTIMEOUT);

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
  Coroutine::Sleep(ELECTIONTIMEOUT);
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
  Coroutine::Sleep(ELECTIONTIMEOUT);

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
  Coroutine::Sleep(ELECTIONTIMEOUT);
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
  Coroutine::Sleep(ELECTIONTIMEOUT / 2);

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
  Coroutine::Sleep(ELECTIONTIMEOUT / 2);

  // Restart victim2
  Log_info("TEST 13: Restarting server %d", victim2);
  config_->Restart(victim2);
  Log_info("TEST 13: Server %d restarted", victim2);

  // Give both time to catch up
  Coroutine::Sleep(ELECTIONTIMEOUT);

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
  Coroutine::Sleep(ELECTIONTIMEOUT);
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
  Coroutine::Sleep(ELECTIONTIMEOUT);

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
  Coroutine::Sleep(ELECTIONTIMEOUT / 2);

  // Restart the old leader
  Log_info("TEST 14: Restarting old leader %d", victim_leader);
  config_->Restart(victim_leader);
  Log_info("TEST 14: Old leader %d restarted", victim_leader);

  // Give both time to catch up
  Coroutine::Sleep(ELECTIONTIMEOUT);

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
  Coroutine::Sleep(ELECTIONTIMEOUT);
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
    Coroutine::Sleep(ELECTIONTIMEOUT);

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
    Coroutine::Sleep(ELECTIONTIMEOUT);

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
    Coroutine::Sleep(ELECTIONTIMEOUT);

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
      Coroutine::Sleep(ELECTIONTIMEOUT / 2);
    }

    // Wait for all servers to catch up
    Coroutine::Sleep(ELECTIONTIMEOUT);

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
  Coroutine::sleep(ELECTIONTIMEOUT / 10);
  
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
  Coroutine::sleep(ELECTIONTIMEOUT);
  
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
  Coroutine::sleep(ELECTIONTIMEOUT);
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
  Coroutine::sleep(ELECTIONTIMEOUT);
  AssertOneLeader(config_->OneLeader());
  
  // rejoin all servers
  // Log_info("TEST 2: Rejoining all servers");
  siteid_t rejoin1 = config_->getNextServerId(leader, 1);
  // Log_info("TEST 2: Rejoining server %d", rejoin1);
  config_->Reconnect(rejoin1);
  
  // Log_info("TEST 2: Rejoining leader %d", leader);
  config_->Reconnect(leader);
  Coroutine::sleep(ELECTIONTIMEOUT);
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
  Coroutine::sleep(ELECTIONTIMEOUT);
  DoAgreeAndAssertIndex(403, NSERVERS - 2, index_++);
  DoAgreeAndAssertIndex(404, NSERVERS - 2, index_++);
  // reconnect followers
  Log_debug("reconnect servers");
  config_->Reconnect(config_->getNextServerId(leader, 1));
  config_->Reconnect(config_->getNextServerId(leader, 2));
  Coroutine::sleep(ELECTIONTIMEOUT);
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
  Coroutine::sleep(ELECTIONTIMEOUT);
  AssertNoneCommitted(index);
  // reconnect followers
  config_->Reconnect(config_->getNextServerId(leader, 1));
  config_->Reconnect(config_->getNextServerId(leader, 2));
  config_->Reconnect(config_->getNextServerId(leader, 3));
  // do agreement in restored quorum
  Coroutine::sleep(ELECTIONTIMEOUT);
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
  Coroutine::sleep(ELECTIONTIMEOUT);
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
  Coroutine::sleep(ELECTIONTIMEOUT);
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
      int cmd = config_->wait(index, NSERVERS, term);
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
  Coroutine::sleep(ELECTIONTIMEOUT);
  // disconnect the leader and its 1 follower, then reconnect the 3 servers
  Log_debug("disconnect the leader and its 1 follower, reconnect the 3 followers");
  config_->Disconnect(config_->getNextServerId(leader1, 1));
  config_->Disconnect(leader1);
  config_->Reconnect(config_->getNextServerId(leader1, 2));
  config_->Reconnect(config_->getNextServerId(leader1, 3));
  config_->Reconnect(config_->getNextServerId(leader1, 4));
  // do a bunch of agreements among the new quorum
  Coroutine::sleep(ELECTIONTIMEOUT);
  Log_debug("try to commit a lot of commands");
  for (int i = 1; i <= 50; i++) {
    DoAgreeAndAssertIndex(800 + i, NSERVERS - 2, index_++);
  }
  // reconnect the old leader and its follower
  Log_debug("reconnect the old leader and the follower");
  config_->Reconnect(config_->getNextServerId(leader1, 1));
  config_->Reconnect(leader1);
  Coroutine::sleep(ELECTIONTIMEOUT);
  // do an agreement all together to check the old leader's incorrect
  // entries are replaced in a timely manner
  int leader2 = config_->OneLeader();
  AssertOneLeader(leader2);
  AssertStartOk(config_->Start(leader2, 851, &index, &term));
  index_++;
  // 10 seconds should be enough to back up 50 incorrect logs
  Coroutine::sleep(2*ELECTIONTIMEOUT);
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
      auto r = config_->wait(startindex + i, NSERVERS, startterm);
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
    auto r = config_->wait(index1, NSERVERS, term1);
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
    Coroutine::sleep(ELECTIONTIMEOUT);
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
    Coroutine::sleep(ELECTIONTIMEOUT);
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
    Coroutine::sleep(ELECTIONTIMEOUT);
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
    Coroutine::sleep(ELECTIONTIMEOUT);
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
  Reactor::create_sp_event<TimeoutEvent>(microseconds)->wait();
}

#endif

}
