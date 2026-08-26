#pragma once

#include "../__dep__.h"
#include "../constants.h"
#include "../communicator.h"
#include "../replication_quorum.h"
#include "messages.hpp"
#include <atomic>
#include <map>
#include <mutex>
#include <rusty/slice.hpp>
#include <type_traits>
#include <utility>

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
#if RUSTYCPP_RUST
#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum NotifyRestartStatus {
    ACKNOWLEDGED = 0,
    PENDING = 1,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_commo.notify_restart_status version=1 rust_sha256=89cd2d6a63f03f1708d794b6331d7d920d43868844847437a560e79d0eb5a8d6*/
enum class NotifyRestartStatus : int32_t;
constexpr NotifyRestartStatus NotifyRestartStatus_ACKNOWLEDGED();
constexpr NotifyRestartStatus NotifyRestartStatus_PENDING();

enum class NotifyRestartStatus : int32_t {
    ACKNOWLEDGED = 0,
    PENDING = 1
};
inline constexpr NotifyRestartStatus NotifyRestartStatus_ACKNOWLEDGED() { return NotifyRestartStatus::ACKNOWLEDGED; }
inline constexpr NotifyRestartStatus NotifyRestartStatus_PENDING() { return NotifyRestartStatus::PENDING; }
/*RUSTYCPP:GEN-END id=raft_commo.notify_restart_status*/

static_assert(std::is_same_v<int, int32_t>);
static_assert(std::is_same_v<std::underlying_type_t<NotifyRestartStatus>, int>);
static_assert(std::is_trivially_copyable_v<NotifyRestartStatus>);
static_assert(sizeof(NotifyRestartStatus) == sizeof(int32_t));
static_assert(alignof(NotifyRestartStatus) == alignof(int32_t));
static_assert(static_cast<int32_t>(NotifyRestartStatus::ACKNOWLEDGED) == 0);
static_assert(static_cast<int32_t>(NotifyRestartStatus::PENDING) == 1);
static_assert(NotifyRestartStatus{} == NotifyRestartStatus::ACKNOWLEDGED);

// Pure Raft communicator decisions over copied scalar values. RPC ownership,
// callback lifetimes, peer access, and restart-status locking stay in C++.
#if RUSTYCPP_RUST
pub const fn commo_append_entries_empty_from_cmd(has_cmd: bool) -> bool {
    !has_cmd
}

pub const fn commo_append_entries_reply_lost(ok: u64,
                                             term: u64,
                                             last_log_index: u64) -> bool {
    ok == 0 && term == 0 && last_log_index == 0
}

pub const fn commo_append_entries_done_from_reply(ok: u64,
                                                  term: u64,
                                                  last_log_index: u64) -> bool {
    !commo_append_entries_reply_lost(ok, term, last_log_index)
}

pub const fn commo_future_failed(error_code: i32) -> bool {
    error_code != 0
}

pub const fn commo_notify_restart_is_pending(status: NotifyRestartStatus) -> bool {
    (status as i32) == (NotifyRestartStatus::PENDING as i32)
}

pub const fn commo_retry_has_pending_sites(pending_count: usize) -> bool {
    pending_count != 0
}

pub const fn commo_quorum_should_record_voter(voter_id: u16) -> bool {
    voter_id != 0
}

pub const fn commo_quorum_should_advance_term(candidate_term: i64,
                                               highest_term: i64) -> bool {
    candidate_term > highest_term
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_commo.scalar_decisions version=1 rust_sha256=2f2c858337892ae597e2e835af358f1c25cf66287833f56a8fb2dc3b0741e88c*/
constexpr bool commo_append_entries_empty_from_cmd(bool has_cmd);
constexpr bool commo_append_entries_reply_lost(uint64_t ok, uint64_t term, uint64_t last_log_index);
constexpr bool commo_append_entries_done_from_reply(uint64_t ok, uint64_t term, uint64_t last_log_index);
constexpr bool commo_future_failed(int32_t error_code);
constexpr bool commo_retry_has_pending_sites(size_t pending_count);
constexpr bool commo_quorum_should_record_voter(uint16_t voter_id);
constexpr bool commo_quorum_should_advance_term(int64_t candidate_term, int64_t highest_term);
constexpr bool commo_append_entries_empty_from_cmd(bool has_cmd) {
    return !has_cmd;
}
constexpr bool commo_append_entries_reply_lost(uint64_t ok, uint64_t term, uint64_t last_log_index) {
    return ((rusty::detail::deref_if_pointer_like(ok) == static_cast<uint64_t>(0)) && (rusty::detail::deref_if_pointer_like(term) == static_cast<uint64_t>(0))) && (rusty::detail::deref_if_pointer_like(last_log_index) == static_cast<uint64_t>(0));
}
constexpr bool commo_append_entries_done_from_reply(uint64_t ok, uint64_t term, uint64_t last_log_index) {
    return !commo_append_entries_reply_lost(std::move(ok), std::move(term), std::move(last_log_index));
}
constexpr bool commo_future_failed(int32_t error_code) {
    return rusty::detail::deref_if_pointer_like(error_code) != static_cast<int32_t>(0);
}
constexpr bool commo_notify_restart_is_pending(NotifyRestartStatus status) {
    return ((static_cast<int32_t>(status))) == ((static_cast<int32_t>(NotifyRestartStatus_PENDING())));
}
constexpr bool commo_retry_has_pending_sites(size_t pending_count) {
    return rusty::detail::deref_if_pointer_like(pending_count) != static_cast<size_t>(0);
}
constexpr bool commo_quorum_should_record_voter(uint16_t voter_id) {
    return rusty::detail::deref_if_pointer_like(voter_id) != static_cast<uint16_t>(0);
}
constexpr bool commo_quorum_should_advance_term(int64_t candidate_term, int64_t highest_term) {
    return rusty::detail::deref_if_pointer_like(candidate_term) > rusty::detail::deref_if_pointer_like(highest_term);
}
/*RUSTYCPP:GEN-END id=raft_commo.scalar_decisions*/

static_assert(!commo_retry_has_pending_sites(0));
static_assert(commo_retry_has_pending_sites(1));
static_assert(!commo_quorum_should_record_voter(0));
static_assert(commo_quorum_should_record_voter(1));
static_assert(commo_quorum_should_advance_term(-1, -2));
static_assert(!commo_quorum_should_advance_term(-2, -1));

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
    {
      std::lock_guard<std::mutex> lock(voters_mtx_);
      // Every syntactically valid reply term dominates its vote bit. Negative
      // ballot_t values are sentinels/malformed wire values, not Raft terms.
      if (term >= 0 &&
          commo_quorum_should_advance_term(
              term, q().highest_term_.get())) {
        q().highest_term_.set(term);
      }
      if (y && commo_quorum_should_record_voter(voter_id)) {
        spec_voters_.insert(voter_id);
      }
    }
    if (y) {
      // Publish voter identity before the quorum wakeup; the candidate takes
      // its voter snapshot immediately after wait_timeout returns.
      // @unsafe
      { vote_yes(); }  // 1 unsafe line: calls @unsafe parent method
    } else {
      vote_no();
    }
  }

  // Legacy overload for backward compatibility
  void FeedResponse(bool y, ballot_t term) {
    FeedResponse(y, term, 0);
  }

  // @safe
  int64_t Term() {
    std::lock_guard<std::mutex> lock(voters_mtx_);
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
#if RUSTYCPP_RUST
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(u64)]
pub enum AckType {
    Memory = 0,
    Durable = 1,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_commo.ack_type version=1 rust_sha256=a4a8dc541c6c5e970e786f9c1e0f32a2c02cfd2eed148b63e2c8623f07058b56*/
enum class AckType : uint64_t;
constexpr AckType AckType_Memory();
constexpr AckType AckType_Durable();

enum class AckType : uint64_t {
    Memory = 0,
    Durable = 1
};
inline constexpr AckType AckType_Memory() { return AckType::Memory; }
inline constexpr AckType AckType_Durable() { return AckType::Durable; }
/*RUSTYCPP:GEN-END id=raft_commo.ack_type*/

static_assert(std::is_same_v<std::underlying_type_t<AckType>, uint64_t>);
static_assert(std::is_trivially_copyable_v<AckType>);
static_assert(sizeof(AckType) == sizeof(uint64_t));
static_assert(alignof(AckType) == alignof(uint64_t));
static_assert(static_cast<uint64_t>(AckType::Memory) == 0);
static_assert(static_cast<uint64_t>(AckType::Durable) == 1);
static_assert(AckType{} == AckType::Memory);

// Response data for async AppendEntries RPC
// Uses shared_ptr semantics to ensure memory validity when callback fires.
// `event` is a nullable Arc handle: the struct is default-constructed (event =
// None) then the event is assigned via create_sp_event before the RPC is sent.
struct AppendEntriesResponse {
  rusty::Option<rusty::Arc<IntEvent>> event{rusty::None};
  // The callback publishes all scalar response fields before setting this
  // flag. HeartbeatLoop can therefore retain and poll a response across
  // rounds without timing out (and permanently poisoning) its IntEvent.
  std::atomic_bool completed{false};
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
