#pragma once

#include "frame.h"
#include "coordinator.h"
#include <map>
#include <rusty/function.hpp>

namespace janus {

#ifdef RAFT_TEST_CORO

// 5 servers in test configuration
#define NSERVERS 5
// slow network connections have latency up to 26 milliseconds
#define MAXSLOW 27
// servers have 1/10 chance of being disconnected to the network
#define DOWNRATE_N 1
#define DOWNRATE_D 10
// Give a generous 5 seconds for elections
#define ELECTIONTIMEOUT 5000000

// elections are expected to take at most 3 RPCs per server
#define ELECTIONRPCS (3 * NSERVERS)
// expected # of RPCs taken to commit n agreements
#define COMMITRPCS(n) ((n + 1) * NSERVERS)


#define Print(format, ...) fprintf(stderr, format "\n", ##__VA_ARGS__)

extern int _test_id_g;
#define Init(test_id, description) \
  Print("TEST %d: " description, test_id); \
  _test_id_g = test_id
#define Failed(msg, ...) Print("TEST %d Failed: " msg, _test_id_g, ##__VA_ARGS__)
#define Passed() Print("TEST %d Passed", _test_id_g)

class CommitIndex {
 private:
  uint64_t val_;
 public:
  CommitIndex(uint64_t val) : val_(val) {}
  uint64_t getval(void) { return val_; }
  void setval(uint64_t val) { val_ = val; }
};

class RaftTestConfig {

 private:
  // @unsafe - test harness registry of live RaftFrame objects. The initial
  // entries are copied from the caller's frame map; Kill() deletes and erases
  // an entry, Restart() allocates and reinserts a replacement.
  static std::map<siteid_t, RaftFrame*> replicas;
  // @unsafe - compatibility learner-action storage. RegLearnerAction still
  // accepts std::function, so these callbacks stay std::function for now.
  // Restart() re-registers a fresh callback on the recreated RaftServer.
  static std::map<siteid_t, std::function<int(int, janus::Command)>> commit_callbacks;
  static std::map<siteid_t, std::vector<int>> committed_cmds;
  static std::map<siteid_t, uint64_t> rpc_count_last;

  // disconnected_[svr] true if svr is disconnected by Disconnect()/Reconnect()
  std::map<siteid_t, bool> disconnected_;
  // @unsafe - test-only network control thread synchronization.
  // guards disconnected_ between Disconnect()/Reconnect() and netctlLoop
  std::mutex disconnect_mtx_;



 public:

  // Helper function to map server IDs to 0-4 range for test framework
  siteid_t mapServerId(siteid_t server_id) const;
  
  // Helper function to get server ID by index (0-4)
  siteid_t getServerIdByIndex(int index) const;
  
  // Helper function to get next server ID in sequence
  siteid_t getNextServerId(siteid_t current_server_id, int offset = 1) const;

  // @unsafe - copies borrowed/live frame pointers into the static test registry.
  // The registry becomes responsible for Kill()/Restart() replacement during
  // the test config lifetime.
  RaftTestConfig(std::map<siteid_t, RaftFrame*>& replicas);

  // sets up learner action functions for the servers
  // so that each committed command on each server is
  // logged to this test's data structures.
  void SetLearnerAction(void);

  // Returns index of leader on success, < 0 on error.
  // If expected is specified, only returns success if the leader == expected
  // Only looks at servers that are not disconnected
  int OneLeader(int expected = -1);

  bool NoLeader(void);

  // Returns true if at least 1 server has a currentTerm
  // number higher than term.
  bool TermMovedOn(uint64_t term);

  // Checks if all servers agree on a term
  // Returns agreed upon term on success
  // -1 if there's disagreement
  uint64_t OneTerm(void);

  // Returns number of servers that think log entry at index is committed.
  // Checks if the committed value for index is the same across servers.
  int NCommitted(uint64_t index);

  // removed `shared_ptr<CommitIndex>
  // StartAgreement(siteid_t, int);` — replaced by `Start()` long ago;
  // the impl in `testconf.cc` started with `verify(0); // this
  // function has been replaced by Start()` and had no callers.

  // Calls Start() to specified server
  bool Start(siteid_t svr, int cmd, uint64_t *index, uint64_t *term);

  // Waits for at least n servers to commit index
  // If commit takes too long, gives up after a while.
  // If term has moved on since the given start term, also gives up.
  // Returns the committed value on success.
  // -1 if it took too long for enough servers to commit
  // -2 if term changed
  // -3 if committed values for index differ
  int Wait(uint64_t index, int n, uint64_t term);

  // Does one agreement.
  // Submits a command with value cmd to the leader
  // Waits at most 2 seconds until n servers commit the command.
  // Makes sure the value of the commits is the same as what was given.
  // If retry == true, Retries the agreement until at most 10 seconds pass.
  // Returns index of committed agreement on success, 0 on error.
  uint64_t DoAgreement(int cmd, int n, bool retry);

  // Disconnects server from rest of servers
  void Disconnect(siteid_t svr);

  // Reconnects disconnected server
  void Reconnect(siteid_t svr);

  // Kills server (destroys it completely, clearing all in-memory state)
  void Kill(siteid_t svr);

  // Restarts server (creates new instance, loads state from disk)
  void Restart(siteid_t svr);

  // Returns number of disconnected servers
  int NDisconnected(void);

  // Sets/unsets network unreliable
  // Blocks until network successfully set to unreliable/reliable
  // If unreliable == true, previous call to SetUnreliable must have been false
  // and vice versa
  void SetUnreliable(bool unreliable = true);

  bool IsUnreliable(void);

  // Reconnects all disconnected servers
  // Waits on unreliable thread
  void Shutdown(void);

  // Resets RPC counts to zero
  void RpcCountReset(void);

  // Returns total RPC count for a server
  // if reset, the next time RpcCount called for
  // svr, the count will exclude all RPCs before this call
  uint64_t RpcCount(siteid_t svr, bool reset = true);

  // Returns total RPC count across all servers
  // since server setup.
  uint64_t RpcTotal(void);

  // Returns true if svr committed a log entry at index with value cmd
  bool ServerCommitted(siteid_t svr, uint64_t index, int cmd);

  // Starts a command with a callback for commit status notification
  // Returns same values as Start()
  bool StartWithCallback(siteid_t svr, int cmd, uint64_t *index, uint64_t *term,
                         rusty::Function<void(CommitStatus)> callback);

 private:
  // vars & subroutine for unreliable network setting
  // @unsafe - test-only thread/condvar state; keep hand-written.
  std::thread th_;
  std::mutex cv_m_; // guards cv_, unreliable_, finished_
  std::condition_variable cv_;
  bool unreliable_ = false;
  bool finished_ = false;
  void netctlLoop(void);

  // internal disconnect/reconnect/slow functions
  // @unsafe - recursive mutex protects test network mutation.
  std::recursive_mutex connection_m_;
  bool isDisconnected(siteid_t svr);
  void disconnect(siteid_t svr, bool ignore = false);
  void reconnect(siteid_t svr, bool ignore = false);
  void slow(siteid_t svr, uint32_t msec);

  // other internal helpers
  int waitOneLeader(bool want_leader, int expected);

 public:
  RaftServer *GetServer(siteid_t svr);

  // ============================================================================
  // SPECULATIVE RAFT STATE QUERIES
  // ============================================================================
  // Query speculative state from servers for testing

  /**
   * Check if a server is a secured leader (has durable vote quorum).
   * @param svr Server ID
   * @return true if server is leader and has secured status
   */
  bool IsSecuredLeader(siteid_t svr);

  /**
   * Get the speculative commit index for a server.
   * @param svr Server ID
   * @return specCommitIndex value, or 0 if server is not leader
   */
  uint64_t GetSpecCommitIndex(siteid_t svr);

  /**
   * Get the secured log index for a server.
   * @param svr Server ID
   * @return securedLogIndex value, or 0 if server is not leader
   */
  uint64_t GetSecuredLogIndex(siteid_t svr);

  /**
   * Get the count of speculative voters for a server.
   * @param svr Server ID
   * @return Number of servers in specVoters, or 0 if not leader
   */
  size_t GetSpecVotersCount(siteid_t svr);

  /**
   * Get the count of durable voters for a server.
   * @param svr Server ID
   * @return Number of servers in durableVoters, or 0 if not leader
   */
  size_t GetDurableVotersCount(siteid_t svr);

  /**
   * Verify speculative invariants hold for a server.
   * @param svr Server ID
   * @return true if securedLogIndex <= specCommitIndex <= lastLogIndex
   */
  bool VerifySpecInvariants(siteid_t svr);

  /**
   * Get the memory ack count for a specific log index.
   * @param svr Server ID
   * @param index Log index to query
   * @return Number of memory acks for that index, or 0 if not available
   */
  size_t GetMemoryAckCount(siteid_t svr, uint64_t index);

  /**
   * Get the durable ack count for a specific log index.
   * @param svr Server ID
   * @param index Log index to query
   * @return Number of durable acks for that index, or 0 if not available
   */
  size_t GetDurableAckCount(siteid_t svr, uint64_t index);

};

#endif

}
