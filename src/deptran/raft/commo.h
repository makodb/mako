#pragma once

#include "../__dep__.h"
#include "../constants.h"
#include "../communicator.h"
#include "../replication_quorum.h"
#include "messages.hpp"
#include <map>
#include <mutex>

// @external: {
//   Log_info: [safe, (...) -> void],
//   Log_debug: [safe, (...) -> void],
//   Log_warn: [safe, (...) -> void],
//   Log_error: [safe, (...) -> void],
//   verify: [safe, (bool) -> void],
//   Reactor::create_sp_event: [safe, () -> rusty::Arc<IntEvent>],
//   Config::GetConfig: [safe, () -> Config*],
//   MarshallDeputy: [safe, (...) -> janus::Command],
//   Future::safe_release: [safe, (Future*) -> void],
//   vote_yes: [safe, () -> void],
//   vote_no: [safe, () -> void]
// }

namespace janus {

/**
 * NotifyRestartStatus - Status of NotifyRestart RPC for each peer
 *
 * Used to track which peers have acknowledged our restart notification.
 * - ACKNOWLEDGED: Peer received notification and reconnected to us
 * - PENDING: Should send/retry NotifyRestart (not yet acknowledged, timed out,
 *   or the peer's one-shot reconnect attempt failed)
 */
enum class NotifyRestartStatus {
  ACKNOWLEDGED,  // Peer reconnected to us
  PENDING        // Need to send/retry NotifyRestart
};

// @unsafe - inherits from non-@interface base QuorumEvent
class RaftVoteQuorumEvent: public QuorumEventBase {
 private:
  // SPECULATIVE VOTING: Track which sites voted yes (memory votes)
  std::set<siteid_t> spec_voters_;
  std::mutex voters_mtx_;

 public:
  using QuorumEventBase::QuorumEventBase;
  // @safe
  bool HasAcceptedValue() {
    return false;
  }

  // @safe - Extended to track voter site IDs for speculative voting
  void FeedResponse(bool y, ballot_t term, siteid_t voter_id = 0) {
    if (y) {
      // @unsafe
      { vote_yes(); }  // 1 unsafe line: calls @unsafe parent method
      // Track the voter for speculative voting
      if (voter_id != 0) {
        std::lock_guard<std::mutex> lock(voters_mtx_);
        spec_voters_.insert(voter_id);
      }
    } else {
      vote_no();
      if(term > q().highest_term_.get())
      {
        q().highest_term_.set(term);
      }
    }
  }

  // Legacy overload for backward compatibility
  void FeedResponse(bool y, ballot_t term) {
    FeedResponse(y, term, 0);
  }

  // @safe
  int64_t Term() {
    return q().highest_term_.get();
  }

  // @unsafe - Get the set of sites that voted yes (memory votes)
  std::set<siteid_t> GetSpecVoters() {
    std::lock_guard<std::mutex> lock(voters_mtx_);
    return spec_voters_;
  }
};

// @unsafe - contains std::recursive_mutex (non-borrow-checked type)
class SendAppendEntriesResults {
 public:
  std::recursive_mutex mtx;
  bool done = false;
  uint64_t ok = 0;
  uint64_t followerTerm = 0;
  uint64_t followerLastLogIndex = 0;
  uint64_t followerAckType = 0;  // 0=Memory, 1=Durable
  bool empty = true;
};

/**
 * AckType - Speculative Replication acknowledgment type
 *
 * Memory: Entry appended to in-memory log (immediate response)
 * Durable: Entry persisted to disk (sent via AppendEntriesDurable RPC)
 */
enum class AckType : uint64_t {
  Memory = 0,
  Durable = 1
};

// Response data for async AppendEntries RPC
// Uses shared_ptr semantics to ensure memory validity when callback fires.
// `event` is a nullable Arc handle: the struct is default-constructed (event =
// None) then the event is assigned via create_sp_event before the RPC is sent.
struct AppendEntriesResponse {
  rusty::Option<rusty::Arc<IntEvent>> event{rusty::None};
  uint64_t status = 0;
  uint64_t term = 0;
  uint64_t last_log_index = 0;
  uint64_t ack_type = 0;  // 0=Memory, 1=Durable (see AckType enum)
};


// @unsafe - inherits from non-@interface base Communicator
class RaftCommo : public Communicator {

friend class RaftProxy;
 private:
  struct NotifyRestartState {
    std::mutex mutex;
    std::map<siteid_t, NotifyRestartStatus> status;
    siteid_t self_site_id = 0;
    parid_t self_par_id = 0;
    uint64_t generation = 0;
  };

  // Futures may finish after a RaftCommo is destroyed during RAFT_TEST Kill.
  // Callbacks retain only this shared state, never a raw communicator pointer.
  std::shared_ptr<NotifyRestartState> notify_restart_state_ =
      std::make_shared<NotifyRestartState>();

 public:
#ifdef RAFT_TEST_CORO
  std::recursive_mutex rpc_mtx_ = {};
  uint64_t rpc_count_ = 0;
#endif

  RaftCommo() = delete;
  // @safe
  RaftCommo(
      rusty::Option<rusty::Arc<rrr::PollThread>> poll = rusty::None);

  // @safe
  // Returns shared_ptr to response data - callback captures this to ensure memory validity
  // take janus::Command (was shared_ptr<Marshallable>);
  // shared_ptr<Marshallable> callers auto-convert via implicit Command ctor.
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
                    const janus::Command& cmd,
                    uint64_t cmdLogTerm
                    );

  // @unsafe - C-style cast, raw pointers
  // take janus::Command (was shared_ptr<Marshallable>);
  // shared_ptr<Marshallable> callers auto-convert via implicit Command ctor.
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
                    const janus::Command& cmd,
                    uint64_t cmdLogTerm,
                    bool trigger_election_now = false);
  // @unsafe - C-style cast
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

  // @unsafe - C-style cast, std::function
  void SendTimeoutNow(siteid_t site_id,
                      parid_t par_id,
                      uint64_t leader_term,
                      siteid_t leader_site_id,
                      std::function<void(bool success, uint64_t follower_term)> callback);

  /**
   * SendVoteDurable - Send VoteDurable RPC to candidate after vote is persisted
   *
   * Called after a follower has durably persisted its vote to disk.
   * Enables speculative voting by notifying the candidate that this vote
   * is now durable and can count towards secured leader status.
   *
   * @param candidate_id - The site ID of the candidate who received the vote
   * @param par_id - Partition ID
   * @param term - Term of the vote
   * @param voter_id - Our own site ID (the voter)
   */
  // @safe
  void SendVoteDurable(siteid_t candidate_id,
                       parid_t par_id,
                       ballot_t term,
                       siteid_t voter_id);

  /**
   * SendAppendEntriesDurable - Send durable ack to leader after log fsync
   *
   * Called after a follower has durably persisted log entries to disk.
   * Enables speculative commits by notifying the leader that entries up to
   * lastLogIndex are now durable and can count towards secured commit.
   *
   * @param leader_id - The site ID of the current leader
   * @param par_id - Partition ID
   * @param term - Current term when entries were persisted
   * @param follower_id - Our own site ID (the follower)
   * @param lastLogIndex - Highest log index that is now durable
   */
  // @safe
  void SendAppendEntriesDurable(siteid_t leader_id,
                                parid_t par_id,
                                ballot_t term,
                                siteid_t follower_id,
                                uint64_t lastLogIndex);

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
   * Called periodically to retry notifications that have not yet succeeded.
   * Peers in ACKNOWLEDGED state are skipped (already done).
   */
  void RetryPendingNotifyRestart();

  /**
   * HasPendingNotifyRestart - Check if any peers still need notification
   *
   * @return true if any peer is still in PENDING state
   */
  bool HasPendingNotifyRestart();

  /**
   * SendInstallSnapshot - Send snapshot to a follower that is too far behind
   *
   * Sends the full snapshot in one RPC (no chunking). The follower will
   * replace its state machine state and discard old log entries.
   *
   * @param site_id - Target follower site ID
   * @param par_id - Partition ID
   * @param term - Leader's current term
   * @param leader_id - Leader's site ID
   * @param last_included_index - Last log index included in snapshot
   * @param last_included_term - Term of last included log entry
   * @param data - Serialized snapshot data
   * @param callback - Called when RPC completes with follower's term
   */
  // @unsafe - C-style cast, std::function
  void SendInstallSnapshot(siteid_t site_id,
                           parid_t par_id,
                           uint64_t term,
                           uint64_t leader_id,
                           uint64_t last_included_index,
                           uint64_t last_included_term,
                           const std::string& data,
                           std::function<void(uint64_t follower_term)> callback);

  // ==========================================================================
  // callback-shaped variants of the quorum RPCs.
  //
  // The existing SendAppendEntries / BroadcastVote methods return
  // shared_ptr<QuorumEvent> shapes that fit the fiber-based wait path in
  // RaftServer. The new *Cb variants deliver each peer's reply via a plain
  // callback, which is the shape RrrTransportAdapter wires into TransportBase. Both
  // variants share the same underlying rrr async_* call site; the *Cb
  // variants are merely a different projection of the reply.
  // ==========================================================================

  // @unsafe - C-style cast, std::function
  // Called once per reply (for the single target site). `on_reply` fires
  // with the site_id that replied; on error, it does not fire at all, so
  // callers should treat absence of reply as a timeout.
  // take janus::Command (was shared_ptr<Marshallable>);
  // shared_ptr<Marshallable> callers auto-convert via implicit Command ctor.
  void SendAppendEntriesCb(
      siteid_t site_id,
      parid_t par_id,
      slotid_t slot_id,
      ballot_t ballot,
      bool isLeader,
      siteid_t leader_site_id,
      uint64_t currentTerm,
      uint64_t prevLogIndex,
      uint64_t prevLogTerm,
      uint64_t commitIndex,
      const janus::Command& cmd,
      uint64_t cmdLogTerm,
      bool trigger_election_now,
      std::function<void(siteid_t, raft::AppendEntriesReply)> on_reply);

  // @unsafe - C-style cast, std::function
  // Broadcasts to every peer in the partition except self. `on_reply`
  // fires once per replying peer with that peer's site_id.
  void BroadcastVoteCb(
      parid_t par_id,
      slotid_t lst_log_idx,
      ballot_t lst_log_term,
      siteid_t self_id,
      ballot_t cur_term,
      std::function<void(siteid_t, raft::VoteReply)> on_reply);
};

} // namespace janus
