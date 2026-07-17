#pragma once

#include "../__dep__.h"
#include "../constants.h"
#include "../communicator.h"
#include "messages.hpp"
#include <map>
#include <mutex>
#include <rusty/function.hpp>
#include <rusty/arc.hpp>

// @external: {
//   Log_info: [safe, (...) -> void],
//   Log_debug: [safe, (...) -> void],
//   Log_warn: [safe, (...) -> void],
//   Log_error: [safe, (...) -> void],
//   verify: [safe, (bool) -> void],
//   Reactor::create_sp_event: [safe, () -> shared_ptr<IntEvent>],
//   Config::GetConfig: [safe, () -> Config*],
//   MarshallDeputy: [safe, (...) -> janus::Command],
//   Future::safe_release: [safe, (Future*) -> void],
//   vote_yes: [safe, () -> void],
//   vote_no: [safe, () -> void]
// }

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
#if RUSTYCPP_RUST
pub enum NotifyRestartStatus {
    ACKNOWLEDGED,
    DOWN,
    PENDING,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=commo.1 version=1 rust_sha256=49a2d41c8e5a2dbf51a9143474e2bcdffb9a9adaba6a56dbbebec0d97ba501c5*/
enum class NotifyRestartStatus;
inline constexpr NotifyRestartStatus NotifyRestartStatus_ACKNOWLEDGED();
inline constexpr NotifyRestartStatus NotifyRestartStatus_DOWN();
inline constexpr NotifyRestartStatus NotifyRestartStatus_PENDING();

enum class NotifyRestartStatus {
    ACKNOWLEDGED,
    DOWN,
    PENDING
};
inline constexpr NotifyRestartStatus NotifyRestartStatus_ACKNOWLEDGED() { return NotifyRestartStatus::ACKNOWLEDGED; }
inline constexpr NotifyRestartStatus NotifyRestartStatus_DOWN() { return NotifyRestartStatus::DOWN; }
inline constexpr NotifyRestartStatus NotifyRestartStatus_PENDING() { return NotifyRestartStatus::PENDING; }
/*RUSTYCPP:GEN-END id=commo.1*/

#if RUSTYCPP_RUST
pub fn commo_notify_restart_is_pending(status: NotifyRestartStatus) -> bool {
    status == NotifyRestartStatus::PENDING
}

pub fn commo_notify_restart_is_acknowledged(status: NotifyRestartStatus) -> bool {
    status == NotifyRestartStatus::ACKNOWLEDGED
}

pub fn commo_notify_restart_is_down(status: NotifyRestartStatus) -> bool {
    status == NotifyRestartStatus::DOWN
}
#endif
/*RUSTYCPP:GEN-BEGIN id=commo.notify_restart_helpers version=1 rust_sha256=4982eba0239b313218739f794edbf2bf5eb185851fb1bbca410230298075554c*/
inline bool commo_notify_restart_is_pending(NotifyRestartStatus status) {
    return status == NotifyRestartStatus::PENDING;
}

inline bool commo_notify_restart_is_acknowledged(NotifyRestartStatus status) {
    return status == NotifyRestartStatus::ACKNOWLEDGED;
}

inline bool commo_notify_restart_is_down(NotifyRestartStatus status) {
    return status == NotifyRestartStatus::DOWN;
}
/*RUSTYCPP:GEN-END id=commo.notify_restart_helpers*/

// @unsafe - inherits from non-@interface base QuorumEvent and tracks voters
// behind a std::mutex; keep hand-written until the event hierarchy migrates.
class RaftVoteQuorumEvent: public QuorumEvent {
 private:
  // SPECULATIVE VOTING: Track which sites voted yes (memory votes)
  std::set<siteid_t> spec_voters_;
  std::mutex voters_mtx_;

 public:
  using QuorumEvent::QuorumEvent;
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
      if(term > highest_term_)
      {
        highest_term_ = term ;
      }
    }
  }

  // Legacy overload for backward compatibility
  void FeedResponse(bool y, ballot_t term) {
    FeedResponse(y, term, 0);
  }

  // @safe
  int64_t Term() {
    return highest_term_;
  }

  // @unsafe - Get the set of sites that voted yes (memory votes)
  std::set<siteid_t> GetSpecVoters() {
    std::lock_guard<std::mutex> lock(voters_mtx_);
    return spec_voters_;
  }
};

struct SendAppendEntriesResults;

inline SendAppendEntriesResults send_append_entries_results_defaults();

#if RUSTYCPP_RUST
pub struct SendAppendEntriesResults {
    done: bool,
    ok: u64,
    followerTerm: u64,
    followerLastLogIndex: u64,
    followerAckType: u64,
    empty: bool,
}

impl SendAppendEntriesResults {
    fn defaults() -> SendAppendEntriesResults {
        send_append_entries_results_defaults()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=commo.send_append_entries_results version=1 rust_sha256=a26d13a9b3118404a9773198ad508022eff105b484c564e122efc81f6b094a77*/
struct SendAppendEntriesResults;

struct SendAppendEntriesResults {
    bool done;
    uint64_t ok;
    uint64_t followerTerm;
    uint64_t followerLastLogIndex;
    uint64_t followerAckType;
    bool empty;

    static SendAppendEntriesResults defaults();
};


inline SendAppendEntriesResults SendAppendEntriesResults::defaults() {
    return send_append_entries_results_defaults();
}
/*RUSTYCPP:GEN-END id=commo.send_append_entries_results*/

inline SendAppendEntriesResults send_append_entries_results_defaults() {
  SendAppendEntriesResults results{};
  results.done = false;
  results.ok = 0;
  results.followerTerm = 0;
  results.followerLastLogIndex = 0;
  results.followerAckType = 0;
  results.empty = true;
  return results;
}

// @safe - value-only interpretation of an AppendEntries callback result. The
// async callback lifetime, shared result object, and RPC fanout stay in
// RaftCommo; these helpers only classify already-copied scalar reply fields.
#if RUSTYCPP_RUST
pub fn commo_append_entries_empty_from_cmd(has_cmd: bool) -> bool {
    !has_cmd
}

pub fn commo_append_entries_reply_lost(ok: u64,
                                       term: u64,
                                       last_log_index: u64) -> bool {
    ok == 0 && term == 0 && last_log_index == 0
}

pub fn commo_append_entries_done_from_reply(ok: u64,
                                            term: u64,
                                            last_log_index: u64) -> bool {
    !commo_append_entries_reply_lost(ok, term, last_log_index)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=commo.append_entries_result_helpers version=1 rust_sha256=99dfd0b1a3cd17a6af5e8a87efebc27952bf26da06424da6c76ba5ebb557ccf0*/
inline bool commo_append_entries_empty_from_cmd(bool has_cmd);
inline bool commo_append_entries_reply_lost(uint64_t ok, uint64_t term, uint64_t last_log_index);
inline bool commo_append_entries_done_from_reply(uint64_t ok, uint64_t term, uint64_t last_log_index);

inline bool commo_append_entries_empty_from_cmd(bool has_cmd) {
    return !has_cmd;
}

inline bool commo_append_entries_reply_lost(uint64_t ok, uint64_t term, uint64_t last_log_index) {
    return ok == 0 && term == 0 && last_log_index == 0;
}

inline bool commo_append_entries_done_from_reply(uint64_t ok, uint64_t term, uint64_t last_log_index) {
    return !commo_append_entries_reply_lost(ok, term, last_log_index);
}
/*RUSTYCPP:GEN-END id=commo.append_entries_result_helpers*/

/**
 * AckType - Speculative Replication acknowledgment type
 *
 * Memory: Entry appended to in-memory log (immediate response)
 * Durable: Entry persisted to disk (sent via AppendEntriesDurable RPC)
 */
#if RUSTYCPP_RUST
#[repr(u64)]
pub enum AckType {
    Memory = 0,
    Durable = 1,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=commo.ack_type version=1 rust_sha256=caffdb0a63dea9cf984c3f302fc0dab2480826a989ae350fd27b63222f73e2d5*/
enum class AckType : uint64_t;
inline constexpr AckType AckType_Memory();
inline constexpr AckType AckType_Durable();

enum class AckType : uint64_t {
    Memory = 0,
    Durable = 1
};
inline constexpr AckType AckType_Memory() { return AckType::Memory; }
inline constexpr AckType AckType_Durable() { return AckType::Durable; }
/*RUSTYCPP:GEN-END id=commo.ack_type*/

// @safe - wire-format predicates for the scalar ack_type field. The durable
// ack callback path and server-side quorum bookkeeping remain hand-C++.
#if RUSTYCPP_RUST
pub fn commo_ack_type_is_memory(ack_type: u64) -> bool {
    ack_type == AckType::Memory as u64
}

pub fn commo_ack_type_is_durable(ack_type: u64) -> bool {
    ack_type == AckType::Durable as u64
}
#endif
/*RUSTYCPP:GEN-BEGIN id=commo.ack_type_helpers version=1 rust_sha256=7ee87e627fd2c0d1ba0229b3295f4b88bc77aeb18aa6195d41e0457405ac3318*/
inline bool commo_ack_type_is_memory(uint64_t ack_type);
inline bool commo_ack_type_is_durable(uint64_t ack_type);

inline bool commo_ack_type_is_memory(uint64_t ack_type) {
    return ack_type == static_cast<uint64_t>(AckType::Memory);
}

inline bool commo_ack_type_is_durable(uint64_t ack_type) {
    return ack_type == static_cast<uint64_t>(AckType::Durable);
}
/*RUSTYCPP:GEN-END id=commo.ack_type_helpers*/

// Response data for async AppendEntries RPC.
// Uses shared_ptr semantics to ensure memory validity when callback fires.
struct AppendEntriesResponse;

inline AppendEntriesResponse append_entries_response_defaults();

#if RUSTYCPP_RUST
pub struct AppendEntriesResponse {
    event: shared_ptr<IntEvent>,
    status: u64,
    term: u64,
    last_log_index: u64,
    ack_type: u64,
}

impl AppendEntriesResponse {
    fn defaults() -> AppendEntriesResponse {
        append_entries_response_defaults()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=commo.append_entries_response version=1 rust_sha256=c051d7c6b60ae5f3a5a0a80b20bdecb4f483c9d80f80d57489b8d5395135f6e9*/
struct AppendEntriesResponse;

struct AppendEntriesResponse {
    shared_ptr<IntEvent> event;
    uint64_t status;
    uint64_t term;
    uint64_t last_log_index;
    uint64_t ack_type;

    static AppendEntriesResponse defaults();
};


inline AppendEntriesResponse AppendEntriesResponse::defaults() {
    return append_entries_response_defaults();
}
/*RUSTYCPP:GEN-END id=commo.append_entries_response*/

inline AppendEntriesResponse append_entries_response_defaults() {
  AppendEntriesResponse response{};
  response.event = shared_ptr<IntEvent>();
  response.status = 0;
  response.term = 0;
  response.last_log_index = 0;
  response.ack_type = 0;
  return response;
}


// @unsafe - legacy RPC communicator. It owns no peer proxies directly; proxy
// tables live in Communicator and are downcast at RPC boundaries.
class RaftCommo : public Communicator {

friend class RaftProxy;
 private:
  // NotifyRestart status tracking for each peer.
  // @unsafe - guarded by std::mutex outside RustyCpp borrow checking.
  std::map<siteid_t, NotifyRestartStatus> notify_restart_status_;
  std::mutex notify_restart_mtx_;
  siteid_t self_site_id_ = 0;  // Our own site ID (set when SendNotifyRestart is called)
  parid_t self_par_id_ = 0;    // Our partition ID

 public:
#ifdef RAFT_TEST_CORO
  // @unsafe - test-only RPC accounting shared with RaftTestConfig.
  std::recursive_mutex rpc_mtx_ = {};
  uint64_t rpc_count_ = 0;
#endif

  RaftCommo() = delete;
  // @safe
  RaftCommo(rusty::Option<rusty::Arc<PollThread>> poll = rusty::None);

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

  // @unsafe - legacy RPC boundary: implementation uses raw RaftProxy casts and
  // async FutureAttr callbacks. Public completion handler has been migrated to
  // rusty::Function, but the implementation still bridges into legacy RPC code.
  void SendTimeoutNow(siteid_t site_id,
                    parid_t par_id,
                    uint64_t leader_term,
                    siteid_t leader_site_id,
                    rusty::Function<void(bool, uint64_t)> callback);

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
   * Called periodically to retry notifications to peers that haven't responded.
   * Peers in DOWN state are skipped (they will reconnect when they restart).
   * Peers in ACKNOWLEDGED state are skipped (already done).
   */
  // @unsafe - retries legacy async RPCs and reads/writes notify_restart_status_
  // under std::mutex.
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
  // @unsafe - reads notify_restart_status_ under std::mutex, which is outside
  // RustyCpp borrow checking.
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
  // @unsafe - legacy RPC boundary: single-target snapshot RPC uses raw RaftProxy
  // casts and async FutureAttr callback. Completion callback uses rusty::Function.
  void SendInstallSnapshot(siteid_t site_id,
                           parid_t par_id,
                           uint64_t term,
                           uint64_t leader_id,
                           uint64_t last_included_index,
                           uint64_t last_included_term,
                           const std::string& data,
                           rusty::Function<void(uint64_t)> callback);

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

  // @unsafe - legacy RPC boundary: single-target AppendEntries callback API.
  // on_reply fires once for the target site if a reply arrives; on transport
  // error, it does not fire, so callers should treat absence as timeout.
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
      rusty::Function<void(siteid_t, raft::AppendEntriesReply)> on_reply);

  // @unsafe - legacy RPC fanout boundary: broadcasts to every peer except self.
  // on_reply is shared across multiple async replies using the implementation's
  // shared_ptr bridge because rusty::Function is move-only.
  void BroadcastVoteCb(
      parid_t par_id,
      slotid_t lst_log_idx,
      ballot_t lst_log_term,
      siteid_t self_id,
      ballot_t cur_term,
      rusty::Function<void(siteid_t, raft::VoteReply)> on_reply);
};

} // namespace janus
