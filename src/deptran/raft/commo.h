#pragma once

#include "../__dep__.h"
#include "../constants.h"
#include "../communicator.h"
#include <map>
#include <mutex>

namespace janus {

class TxData;

/**
 * NotifyRestartStatus - Status of NotifyRestart RPC for each peer
 *
 * Used to track which peers have acknowledged our restart notification.
 * - ACKNOWLEDGED: Peer received notification and reconnected to us
 * - DOWN: Peer responded with "I'm down" (svr_ == nullptr), no retry needed
 * - PENDING: Should send/retry NotifyRestart (not yet acknowledged or timed out)
 */
enum class NotifyRestartStatus {
  ACKNOWLEDGED,  // Peer reconnected to us
  DOWN,          // Peer told us it's down (no retry needed, will reconnect when it restarts)
  PENDING        // Need to send/retry NotifyRestart
};

class RaftVoteQuorumEvent: public QuorumEvent {
 public:
  using QuorumEvent::QuorumEvent;
  // @safe
  bool HasAcceptedValue() {
    return false;
  }

  // @safe
  void FeedResponse(bool y, ballot_t term) {
    if (y) {
      // @unsafe
      { vote_yes(); }  // 1 unsafe line: calls @unsafe parent method
    } else {
      // @unsafe
      { vote_no(); }   // 1 unsafe line: calls @unsafe parent method
      if(term > highest_term_)
      {
        highest_term_ = term ;
      }
    }
  }

  // @safe
  int64_t Term() {
    return highest_term_;
  }
};

class SendAppendEntriesResults {
 public:
  std::recursive_mutex mtx;
  bool done = false;
  uint64_t ok = 0;
  uint64_t followerTerm = 0;
  uint64_t followerLastLogIndex = 0;
  bool empty = true;
};

// Response data for async AppendEntries RPC
// Uses shared_ptr semantics to ensure memory validity when callback fires
struct AppendEntriesResponse {
  shared_ptr<IntEvent> event;
  uint64_t status = 0;
  uint64_t term = 0;
  uint64_t last_log_index = 0;
};


class RaftCommo : public Communicator {

friend class RaftProxy;
 private:
  // NotifyRestart status tracking for each peer
  std::map<siteid_t, NotifyRestartStatus> notify_restart_status_;
  std::mutex notify_restart_mtx_;
  siteid_t self_site_id_ = 0;  // Our own site ID (set when SendNotifyRestart is called)
  parid_t self_par_id_ = 0;    // Our partition ID

 public:
#ifdef RAFT_TEST_CORO
  std::recursive_mutex rpc_mtx_ = {};
  uint64_t rpc_count_ = 0;
#endif

  RaftCommo() = delete;
  // @safe
  RaftCommo(rusty::Option<rusty::Arc<PollThread>> poll = rusty::None);

  // @safe
  // Returns shared_ptr to response data - callback captures this to ensure memory validity
  shared_ptr<AppendEntriesResponse>
  SendAppendEntries2(siteid_t site_id,
                    parid_t par_id,
                    slotid_t slot_id,
                    ballot_t ballot,
                    bool isLeader,
                    siteid_t leader_site_id,
                    uint64_t currentTerm,
                    uint64_t prevLogIndex,
                    uint64_t prevLogTerm,
                    uint64_t commitIndex,
                    shared_ptr<Marshallable> cmd,
                    uint64_t cmdLogTerm
                    );

  // @safe
  shared_ptr<SendAppendEntriesResults>
  SendAppendEntries(siteid_t site_id,
                    parid_t par_id,
                    slotid_t slot_id,
                    ballot_t ballot,
                    bool isLeader,
                    siteid_t leader_site_id,
                    uint64_t currentTerm,
                    uint64_t prevLogIndex,
                    uint64_t prevLogTerm,
                    uint64_t commitIndex,
                    shared_ptr<Marshallable> cmd,
                    uint64_t cmdLogTerm,
                    bool trigger_election_now = false);
  // @safe
  shared_ptr<RaftVoteQuorumEvent>
  BroadcastVote(parid_t par_id,
                        slotid_t lst_log_idx,
                        ballot_t lst_log_term,
                        siteid_t self_id,
                        ballot_t cur_term );

  /**
   * SendTimeoutNow - Send TimeoutNow RPC to target replica
   *
   * Instructs target replica to start election immediately.
   * Used for leadership transfer protocol.
   *
   * @param site_id - Target replica (preferred leader)
   * @param par_id - Partition ID
   * @param leader_term - Current leader's term
   * @param leader_site_id - Current leader's site ID
   * @param callback - Called when RPC completes (success/failure)
   */

  // @safe
  void SendTimeoutNow(siteid_t site_id,
                      parid_t par_id,
                      uint64_t leader_term,
                      siteid_t leader_site_id,
                      std::function<void(bool success, uint64_t follower_term)> callback);

  /**
   * SendNotifyRestart - Broadcast restart notification to all peers
   *
   * Called after a server restarts to tell all other servers to reconnect
   * their client connections to this server. Initializes status tracking map
   * with all peers as PENDING.
   *
   * @param self_id - The site ID of the restarted server (self)
   * @param par_id - Partition ID
   */
  // @safe
  void SendNotifyRestart(siteid_t self_id, parid_t par_id);

  /**
   * RetryPendingNotifyRestart - Retry NotifyRestart for peers still in PENDING state
   *
   * Called periodically to retry notifications to peers that haven't responded.
   * Peers in DOWN state are skipped (they will reconnect when they restart).
   * Peers in ACKNOWLEDGED state are skipped (already done).
   */
  void RetryPendingNotifyRestart();

  /**
   * GetNotifyRestartStatus - Get the current status for a peer
   *
   * @param site_id - The site ID to query
   * @return The NotifyRestartStatus for that peer, or PENDING if not found
   */
  NotifyRestartStatus GetNotifyRestartStatus(siteid_t site_id);

  /**
   * HasPendingNotifyRestart - Check if any peers still need notification
   *
   * @return true if any peer is still in PENDING state
   */
  bool HasPendingNotifyRestart();
};

} // namespace janus

