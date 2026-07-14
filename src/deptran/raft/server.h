#pragma once

#include "../__dep__.h"
#include "../constants.h"
#include "../scheduler.h"
#include "../classic/tpc_command.h"
#include "commo.h"
#include <deque>
#include <rusty/box.hpp>
#include <rusty/arc.hpp>
#include "log_storage.hpp"
#include "recovery_manager.hpp"
#include "snapshot_manager.hpp"
#include <rusty/function.hpp>

// @external: {
//   Log_info: [safe, (...) -> void],
//   Log_debug: [safe, (...) -> void],
//   Log_warn: [safe, (...) -> void],
//   Log_error: [safe, (...) -> void],
//   Log_fatal: [safe, (...) -> void],
//   verify: [safe, (bool) -> void],
//   Config::GetConfig: [safe, () -> Config*],
//   Reactor::create_sp_event: [safe, () -> shared_ptr<IntEvent>],
//   Fiber::create_run: [safe, (...) -> void],
//   Fiber::sleep: [safe, (int) -> void],
//   RandomGenerator::rand_double: [safe, (double, double) -> double],
//   RandomGenerator::rand: [safe, (int, int) -> int],
//   Time::now: [safe, () -> uint64_t],
//   std::make_shared: [safe, (...) -> shared_ptr<T>],
//   dynamic_pointer_cast: [safe, (shared_ptr<T>) -> shared_ptr<U>],
//   strcmp: [safe, (const char*, const char*) -> int],
//   std::sort: [safe, (...) -> void],
//   std::max: [safe, (T, T) -> T],
//   std::min: [safe, (T, T) -> T],
//   std::stoull: [safe, (const string&) -> uint64_t],
//   std::stoll: [safe, (const string&) -> int64_t],
//   std::this_thread::sleep_for: [safe, (duration) -> void],
//   JetpackRecoveryEntry: [safe, (...) -> void],
//   RuleWitnessGC: [safe, (...) -> void]
// }

namespace janus {
class CmdData;
class ReplicatedDB;

#define INVALID_SITEID  ((siteid_t)-1)
#define NUM_BATCH_TIMER_RESET  (100)
#define SEC_BATCH_TIMER_RESET  (1)

/**
 * StepDownReason - Why the leader is stepping down
 *
 * Used by stepDown() to determine what action to take:
 * - UnsecuredFailure: Lost speculative quorum while unsecured leader.
 *   All current-term entries are suspect, clients should be notified.
 * - SecuredFailure: Lost quorum but was secured leader.
 *   Only unsecured entries (specCommitIndex, securedLogIndex] are suspect.
 * - HigherTerm: Saw higher term from another server.
 *   Entries may still be valid, no automatic rollback notification.
 */
#if RUSTYCPP_RUST
pub enum StepDownReason {
    UnsecuredFailure,
    SecuredFailure,
    HigherTerm,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.1 version=1 rust_sha256=258d018ce58e8adb11536a93cbb5afb4bdf975e92088befcc4a110b7537276a7*/
enum class StepDownReason;
inline constexpr StepDownReason StepDownReason_UnsecuredFailure();
inline constexpr StepDownReason StepDownReason_SecuredFailure();
inline constexpr StepDownReason StepDownReason_HigherTerm();

enum class StepDownReason {
    UnsecuredFailure,
    SecuredFailure,
    HigherTerm
};
inline constexpr StepDownReason StepDownReason_UnsecuredFailure() { return StepDownReason::UnsecuredFailure; }
inline constexpr StepDownReason StepDownReason_SecuredFailure() { return StepDownReason::SecuredFailure; }
inline constexpr StepDownReason StepDownReason_HigherTerm() { return StepDownReason::HigherTerm; }
/*RUSTYCPP:GEN-END id=server.1*/

/**
 * CommitStatus - Notification status for client callbacks
 *
 * Used by client callback infrastructure to notify clients of entry status:
 * - SPECULATIVE: Entry reached memory quorum, likely to commit
 * - DURABLE: Entry reached disk quorum with secured leader, guaranteed
 * - ROLLEDBACK: Entry will not commit (leader stepped down gracefully)
 */
#if RUSTYCPP_RUST
pub enum CommitStatus {
    SPECULATIVE,
    DURABLE,
    ROLLEDBACK,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.2 version=1 rust_sha256=c556ede4067b5cb3936397f9027b1a2b153fa316d70eb7ebe5be557414044b09*/
enum class CommitStatus;
inline constexpr CommitStatus CommitStatus_SPECULATIVE();
inline constexpr CommitStatus CommitStatus_DURABLE();
inline constexpr CommitStatus CommitStatus_ROLLEDBACK();

enum class CommitStatus {
    SPECULATIVE,
    DURABLE,
    ROLLEDBACK
};
inline constexpr CommitStatus CommitStatus_SPECULATIVE() { return CommitStatus::SPECULATIVE; }
inline constexpr CommitStatus CommitStatus_DURABLE() { return CommitStatus::DURABLE; }
inline constexpr CommitStatus CommitStatus_ROLLEDBACK() { return CommitStatus::ROLLEDBACK; }
/*RUSTYCPP:GEN-END id=server.2*/

// @safe - data struct with shared_ptr fields (shared_ptr marked @external)
//
// polymorphic command fields
// (`accepted_cmd_` / `committed_cmd_` / `log_`) migrated from
// `shared_ptr<Marshallable>` to `janus::Command`.  Internal storage
// inside Command remains `shared_ptr<Marshallable>` (boundary calls
// to APIs still taking `shared_ptr<Marshallable>` use
// `cmd.inner_marshallable()`).  Wire format unchanged.  See
// `docs/dev/l10-unblock-plan.md`.
struct RaftData {
  ballot_t max_ballot_seen_ = 0;
  ballot_t max_ballot_accepted_ = 0;
  Command accepted_cmd_{};
  Command committed_cmd_{};

  ballot_t term;
  Command log_{};

	//for retries
	ballot_t prevTerm;
	slotid_t slot_id;
	ballot_t ballot;
};

// @safe - simple POD struct
#if RUSTYCPP_RUST
pub struct KeyValue {
    key: i32,
    value: i32,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.3 version=1 rust_sha256=8fbf17da15ef0ea0f533c3d6b345b770b027c42155db17045c72e6a3de5ab946*/
struct KeyValue;

struct KeyValue {
    int32_t key;
    int32_t value;
};
/*RUSTYCPP:GEN-END id=server.3*/

#ifdef RAFT_TEST_CORO
#define HEARTBEAT_INTERVAL 100000
#else
#define HEARTBEAT_INTERVAL 5000
#endif


// @unsafe - large stateful Raft core. This remains a Part 1 reshape target:
// raw frame/commo pointers come from TxLogServer, threading/atomics stay
// hand-written, and storage/snapshot backends are shared legacy boundaries.
class RaftServer : public TxLogServer {
  friend class RaftTestConfig;  // Allow test config to access private members for kill/restart
  friend class RaftLabTest;     // Allow test cases to access private members for verification
 private:
  // ============================================================================
  // LOG PERSISTENCE
  // ============================================================================
  // @unsafe - optional shared storage backend. Kept as std::shared_ptr
  // because storage implementations are polymorphic legacy boundaries.
  std::shared_ptr<janus::raft::LogStorage> log_storage_;
  bool async_persistence_ = false;  // Runtime: sync (default) vs async disk persistence

  // ============================================================================
  // SNAPSHOT SUPPORT
  // ============================================================================
  // @unsafe - optional shared snapshot backend; polymorphic and file/RocksDB
  // backed implementations remain outside early DSL migration.
  std::shared_ptr<janus::raft::SnapshotManager> snapshot_manager_;
  uint64_t snapshot_threshold_ = 10000;  // Entries between snapshots (configurable)

  // State machine snapshot callbacks (set by ReplicatedDB or other state machines)
  // @safe - stores move-only RustyCpp callbacks for later invocation
  rusty::Function<std::string()> create_sm_snapshot_cb_;
  rusty::Function<void(const std::string&)> load_sm_snapshot_cb_;

  // Optional replicated DB (created when MAKO_REPLICATED_DB=1 env var is set).
  // @unsafe - shared state-machine adapter that wraps RocksDB C handles.
  std::shared_ptr<ReplicatedDB> replicated_db_;

  // @unsafe - Initializes snapshot manager from environment config
  void InitializeSnapshotManager();

  // @unsafe - Creates a snapshot of current state, persists via snapshot_manager_,
  //           then compacts the log. Called from applyLogs() when threshold is met.
  void CreateSnapshot();

  // Metadata keys for LogStorage persistence
  static constexpr const char* META_TERM = "currentTerm";
  static constexpr const char* META_VOTE_FOR = "vote_for";
  static constexpr const char* META_COMMIT_INDEX = "commitIndex";
  static constexpr const char* META_SPEC_COMMIT_INDEX = "specCommitIndex";
  static constexpr const char* META_SECURED_LOG_INDEX = "securedLogIndex";

  // @safe - LogStorage-based persistence helper methods (external LogStorage API calls wrapped in @unsafe blocks)
  void PersistTermAndVoteToLogStorage();
  // @safe - Persists specCommitIndex and securedLogIndex to storage
  void PersistSpeculativeIndicesToLogStorage();
  // @safe - Persists vote_for only to storage
  void PersistVoteToLogStorage();
  // @safe - Persists commitIndex to storage
  void PersistCommitIndexToLogStorage();
  // @safe - Persists a single log entry
  void PersistLogEntryToLogStorage(slotid_t slot_id, const RaftData& data);
  // @safe - Persists multiple log entries
  void PersistLogEntriesToLogStorage(const std::vector<std::pair<slotid_t, std::shared_ptr<RaftData>>>& entries);

  // ============================================================================

  // Replication index state. Keep std containers here until RaftServer is
  // split into smaller DSL candidates; these maps participate in quorum and
  // heartbeat logic across many methods.
  std::map<siteid_t, uint64_t> match_index_{};
  std::map<siteid_t, uint64_t> next_index_{};
  // @unsafe - election/heartbeat timer threads are hard-deferred threading
  // state; joined/stopped manually by RaftServer teardown.
  std::vector<std::thread> timer_threads_ = {};
  // @unsafe - uses raw pointer parameter for thread signaling
  void timer_thread(bool *vote) ;
  rusty::Box<Timer> timer_;  // Owned timer, auto-cleaned on destruction
  uint64_t last_heartbeat_time_ = 0;
  // @safe - logging calls wrapped in @unsafe blocks in implementation
  void LogTermChange(const char* reason, uint64_t old_term, uint64_t new_term, siteid_t source = INVALID_SITEID);
  bool stop_ = false ;
  siteid_t vote_for_ = INVALID_SITEID ;
  bool init_ = false ;
  bool is_leader_ = false ;
  siteid_t current_leader_id_ = INVALID_SITEID ;  // Last known leader (self if leader, sender of AppendEntries otherwise)
  slotid_t snapidx_ = 0 ;
  ballot_t snapterm_ = 0 ;
  int32_t wait_int_ = 100000 ;
  bool disconnected_ = false;
  bool req_voting_ = false ;
  bool in_applying_logs_ = false ;
  // @unsafe - cross-thread apply signal; keep atomic semantics.
  std::atomic<bool> apply_pending_{false};  // Tracks if new work arrived while applying logs
#ifdef RAFT_TEST_CORO
  bool failover_{true} ;
#else
  bool failover_{true} ;
#endif
  // @unsafe - cross-thread timer batching counter; not a Cell candidate.
  atomic<int64_t> counter_{0};
  const char *filename = "/db/data.txt";

  bool looping_ = false;
  bool heartbeat_ = true;
  bool heartbeat_setup_ = false;
  uint64_t heartbeat_interval_us_ = HEARTBEAT_INTERVAL;  // Runtime-configurable heartbeat interval (microseconds)
  uint64_t log_retention_window_ = 5000;  // Configurable log retention window (entries to keep after compaction)
	enum { STOPPED, RUNNING } status_;
	rusty::Function<void(bool)> leader_change_cb_;

  // ============================================================================
  // PREFERRED REPLICA SYSTEM - Leadership Transfer
  // ============================================================================
  // Implements leadership transfer protocol where one replica is designated as
  // the "preferred leader". The system works via:
  // 1. Standard Raft voting (no bias) - any replica can win initial election
  // 2. Non-preferred leader monitors for preferred replica
  // 3. When preferred is alive & caught up, non-preferred leader:
  //    - Ensures preferred has all committed logs
  //    - Steps down from leadership
  //    - Preferred replica starts election and becomes leader
  // 4. All operations maintain Raft safety guarantees (no data loss)

  siteid_t preferred_leader_site_id_ = INVALID_SITEID;     // Site ID of preferred leader
  uint64_t leader_last_commit_index_ = 0;                   // Leader's commit index (from heartbeats)
  bool transferring_leadership_ = false;                    // True when transfer in progress
  uint64_t leadership_transfer_start_time_ = 0;             // When transfer started (for timeout)
  // @unsafe - leadership monitor thread coordination; hard-deferred. The
  // monitor thread captures `this` and is coordinated manually during
  // leadership transfer and shutdown.
  std::atomic<bool> leadership_monitor_stop_{false};       // Signal to stop monitoring thread
  std::thread leadership_monitor_thread_;                   // Background thread monitoring for transfer
  uint64_t startup_timestamp_ = 0;                          // When server started (for grace period)

  // ============================================================================
  // SPECULATIVE REPLICATION STATE
  // ============================================================================
  // Enables separation of "speculative" (memory quorum) from "secured" (durable
  // quorum) for both leadership and log entries. See docs/dev/phase1_speculative_state_plan.md

  // Leader security status - true when durable vote quorum achieved
  // When securedLeader_ = true, a quorum has votedFor = me on disk,
  // so no other candidate can win election in this term.
  bool securedLeader_ = false;

  // Vote tracking for current term (as candidate/leader)
  std::set<siteid_t> specVoters_;     // servers that have memory-voted for us
  std::set<siteid_t> durableVoters_;  // servers that have durably-voted for us

  // Log commit tracking
  // Invariant: securedLogIndex_ <= specCommitIndex_ <= lastLogIndex
  uint64_t securedLogIndex_ = 0;      // highest index with durable ack quorum
  uint64_t specCommitIndex_ = 0;      // highest index with memory ack quorum

  // Acknowledgment tracking per log index
  // Key: log index, Value: set of nodes that have acked at that level
  std::map<uint64_t, std::set<siteid_t>> memoryAcks_;   // track memory acks per index
  std::map<uint64_t, std::set<siteid_t>> durableAcks_;  // track durable acks per index

  // @unsafe - Thread completion flag wrapping std::atomic<bool> for use with rusty::Arc.
  // Arc only provides const access, so the atomic must be mutable to allow store().
  // Uses C++ mutable for interior mutability (analogous to UnsafeCell in Rust).
  struct AtomicFlag {
    mutable std::atomic<bool> value{false}; // @unsafe { mutable field for interior mutability }
    explicit AtomicFlag(bool v) : value(v) {}
    void set(bool v, std::memory_order order = std::memory_order_release) const {
      value.store(v, order);
    }
    bool get(std::memory_order order = std::memory_order_acquire) const {
      return value.load(order);
    }
  };

  // @safe - Tracked async persistence threads (joined in destructor to prevent UAF)
  // Each entry pairs a thread with a completion flag. The lambda sets the flag to true
  // when done, allowing us to prune finished threads at each new insertion to prevent
  // unbounded growth of thread handles.
  // @unsafe - async persistence thread registry; uses std::thread plus an
  // atomic completion flag to avoid use-after-free on shutdown.
  std::mutex async_threads_mtx_;
  std::vector<std::pair<std::thread, rusty::Arc<AtomicFlag>>> async_threads_;

  // Client notification callbacks
  // Key: log index, Value: callback to notify on commit status change
  // Callbacks are invoked with: SPECULATIVE (memory quorum), DURABLE (disk quorum),
  // or ROLLEDBACK (leader stepped down gracefully)
  // @safe - move-only callbacks stored by log index for later notification.
  std::map<uint64_t, rusty::Function<void(CommitStatus)>> pendingCallbacks_;
  uint64_t lastSpecNotifiedIndex_ = 0;    // last index notified with SPECULATIVE
  uint64_t lastDurableNotifiedIndex_ = 0; // last index notified with DURABLE

  // ============================================================================
  // MEMBERSHIP CONFIGURATION TRACKING
  // ============================================================================
  // Tracks the active set of replicas in this partition. Initialized from the
  // static partition config in Setup(), then modified by AddServer/RemoveServer.
  // All quorum calculations should use current_config_.size() instead of the
  // static Config::GetConfig()->GetPartitionSize().
  std::set<siteid_t> current_config_;          // Active replica set (site IDs)
  bool config_change_pending_ = false;         // True when a config entry is in-flight
  uint64_t pending_config_index_ = 0;          // Log index of pending config entry

  // ============================================================================
  // LEARNER / NEW SERVER CATCH-UP TRACKING
  // ============================================================================
  // Servers being caught up before joining the quorum. Learners receive log
  // entries via HeartbeatLoop (they are added to next_index_/match_index_)
  // but do NOT count towards quorum for commit index calculation.
  // Once a learner's match_index_ is within catchup_threshold_ of the
  // leader's lastLogIndex, it is promoted to a full member in current_config_.
  std::set<siteid_t> learners_;               // Servers being caught up (not yet in quorum)
  uint64_t catchup_threshold_ = 100;          // Entries within lastLogIndex to consider "caught up"

  // @safe - simple comparison of member fields
  bool AmIPreferredLeader() const {
    // @unsafe
    {
      return preferred_leader_site_id_ != INVALID_SITEID &&
             site_id_ == preferred_leader_site_id_;
    }
  }

  // @safe - Check if I have caught up to the current leader's commit level
  bool HaveCaughtUp() const {
    // We've caught up if our commitIndex >= leader's last known commitIndex
    // Note: leader_last_commit_index_ is updated from AppendEntries heartbeats
    return commitIndex >= leader_last_commit_index_;
  }

  // ============================================================================
  
  // @safe - external calls marked @external, mutex/pointer ops in @unsafe blocks
	bool RequestVote() ;

  // @safe - server setup (threading via @unsafe blocks)
	void Setup();
  // @safe - external calls marked @external, core replication loop
	void HeartbeatLoop() ;

  // @unsafe - raw pointer output parameters (reply_term, vote_granted)
  // SPECULATIVE VOTING: Respond immediately, then persist and send VoteDurable async
  void doVote(const slotid_t& lst_log_idx,
              const ballot_t& lst_log_term,
              const siteid_t& can_id,
              const ballot_t& can_term,
              ballot_t *reply_term,
              bool_t *vote_granted,
              bool_t vote) {
      // @unsafe
      {
        *vote_granted = vote ;
        *reply_term = currentTerm ;
      }
#ifdef RAFT_LEADER_ELECTION_DEBUG
      siteid_t prev_vote_for = vote_for_;
      Log_info("[RAFT_VOTE] server %d (loc %d) vote=%d candidate=%d can_term=%lu cur_term=%lu prev_vote_for=%d is_leader=%d lst_idx=%lu lst_term=%lu",
               site_id_, loc_id_, vote, can_id, can_term, currentTerm, prev_vote_for, is_leader_, lst_log_idx, lst_log_term);
#endif

      if( can_term > currentTerm)
      {
          // Any higher term seen means we must immediately step down.
          setIsLeader(false);
          auto prev_term = currentTerm;
          currentTerm = can_term ;
          // @unsafe
          {
            vote_for_ = INVALID_SITEID;  // Reset vote when advancing to new term
          }

          // SPECULATIVE: Still persist term change synchronously for correctness
          // (term changes must be durable before we can proceed)
          PersistState(currentTerm, vote_for_, "doVote: observed higher term");
          LogTermChange("vote request carried newer term", prev_term, currentTerm, can_id);
      }

      if(vote)
      {
          setIsLeader(false) ;
          vote_for_ = can_id ;

#ifdef RAFT_LEADER_ELECTION_DEBUG
          Log_info("[RAFT_VOTE] server %d recorded vote_for=%d at term=%lu", site_id_, vote_for_, currentTerm);
#endif
          // Reset timeout
          resetTimer("granted vote");

          if (async_persistence_) {
            // SPECULATIVE VOTING (async mode): return NOW (memory vote), then
            // start async persistence and send VoteDurable after fsync. The
            // outer fiber replies automatically once this function returns.
            n_vote_++ ;

            // Capture necessary state for the async persistence thread.
            ballot_t term_copy = currentTerm;
            siteid_t voter_copy = site_id_;
            siteid_t can_id_copy = can_id;
            parid_t par_id_copy = partition_id_;

            // Track async persistence thread (joined in destructor to prevent UAF)
            {
              std::lock_guard<std::mutex> lk(async_threads_mtx_);
              // Prune completed threads to prevent unbounded accumulation
              async_threads_.erase(
                std::remove_if(async_threads_.begin(), async_threads_.end(),
                  [](auto& entry) {
                    if (entry.second->get()) {
                      if (entry.first.joinable()) entry.first.join();
                      return true;
                    }
                    return false;
                  }),
                async_threads_.end());
              auto done = rusty::Arc<AtomicFlag>::make(false);
              async_threads_.emplace_back(
                std::thread([this, term_copy, voter_copy, can_id_copy, par_id_copy, done]() {
                  // Persist the vote durably
                  PersistState(term_copy, can_id_copy, "doVote: async vote persist");

                  // Send VoteDurable RPC to candidate
                  auto c = commo();
                  if (c != nullptr) {
                      c->SendVoteDurable(can_id_copy, par_id_copy, term_copy, voter_copy);
                  }
                  done->set(true);
              }), done);
            }

            return;
          } else {
            // SYNC PERSISTENCE (traditional Raft): Persist FIRST, then return.
            // No separate VoteDurable RPC needed — the ack implies durability.
            PersistState(currentTerm, can_id, "doVote: sync vote persist");
            n_vote_++ ;
            return;
          }
      }

      n_vote_++ ;
  }

  // @safe - shared_ptr/callback operations wrapped in @unsafe blocks in implementation
  void applyLogs();

  // Dedicated apply fiber and background apply thread. These capture `this`
  // and are hard-deferred concurrency boundaries for the RustyCpp migration.
  void StartApplyFiber();

  // @unsafe - background apply thread and queue are manually coordinated.
  // Destructor joins apply_thread_ before member teardown.
  std::thread apply_thread_;
  std::atomic<bool> apply_thread_running_{false};
  std::mutex apply_queue_mtx_;
  // apply_queue_ holds Command
  // instead of shared_ptr<Marshallable> — RaftData::log_ migrated in
  // prep2; this drops the boundary unwrap that
  // EnqueueCommittedEntries had to do.  Wire format unchanged.
  std::deque<std::pair<slotid_t, Command>> apply_queue_;

  void StartApplyThread();
  void EnqueueCommittedEntries(slotid_t old_commit, slotid_t new_commit);

  // @unsafe - timer and atomic operations include atomics/mutexes
  void resetTimerBatch()
  {
    // Log_info("!!!!!!! if (!failover_)");
    if (!failover_) return ;
    auto cur_count = counter_++;
    if (cur_count > NUM_BATCH_TIMER_RESET ) {
      // @unsafe
      {
      if (timer_->elapsed() > SEC_BATCH_TIMER_RESET) {
        resetTimer("batch timer adjustment");
      }
      }
      counter_.store(0);
    }
  }
  // @unsafe - raw pointer output params from base class virtual interface
  void OnJetpackPullCmd(const epoch_t& jepoch,
                        const epoch_t& oepoch,
                        const std::vector<key_t>& keys,
                        bool_t* ok,
                        epoch_t* reply_jepoch,
                        epoch_t* reply_oepoch,
                        janus::Command* reply_old_view,
                        janus::Command* reply_new_view,
                        shared_ptr<KeyCmdBatchData>& batch) override;

  // @unsafe - const char* parameter type requires unsafe context
  void resetTimer(const char* reason = "unspecified") {
    // @unsafe
    {
      const char* why = reason ? reason : "unspecified";
      auto prev_time = last_heartbeat_time_;
      last_heartbeat_time_ = Time::now();
      // Log only important timer resets (elections, votes), not routine heartbeats
      if (strcmp(why, "granted vote") == 0 || strcmp(why, "start election timer") == 0) {
        Log_info("[TIMER_RESET] Site %d: reset timer (%s) - prev_hb_time=%lu new_hb_time=%lu delta=%lu",
                 site_id_, why, prev_time, last_heartbeat_time_, last_heartbeat_time_ - prev_time);
      }
    }
    if (failover_) {
      timer_->start() ;
    }
  }

  // @safe - random number generation (external call wrapped in @unsafe block)
  double randDuration()
  {
    // election timeout between 0.4 and 0.7 seconds
    // @unsafe { RandomGenerator is external }
    return RandomGenerator::rand_double(0.4, 0.7) ;
  }

  // @unsafe - Uses LogStorage for persistence
  void PersistState(uint64_t term, siteid_t voted_for, const char* reason = "unspecified") {
    if (!log_storage_ || !log_storage_->is_open()) return;
    PersistTermAndVoteToLogStorage();
    Log_debug("[RAFT-PERSISTENCE] Persisted: term=%lu votedFor=%u (%s)",
              term, voted_for, reason);
  }

  // @unsafe - Uses LogStorage for persistence
  void PersistLogEntry(slotid_t slot_id, const RaftData& entry, const char* reason = "unspecified") {
    if (!log_storage_ || !log_storage_->is_open()) return;
    PersistLogEntryToLogStorage(slot_id, entry);
    Log_debug("[RAFT-PERSISTENCE] Persisted log: slot=%lu (%s)", slot_id, reason);
  }

  // @unsafe - Uses LogStorage for persistence
  void PersistCommitIndex(uint64_t commit_index, const char* reason = "unspecified") {
    if (!log_storage_ || !log_storage_->is_open()) return;
    PersistCommitIndexToLogStorage();
  }

  /**
   * Get dynamic election timeout based on preferred replica role and grace period
   *
   * Returns:
   * - Preferred replica: 150-300ms (short timeout to win elections quickly)
   * - Non-preferred during grace period (0-5s after startup): 1-2s (long timeout to allow preferred to win)
   * - Non-preferred after grace period: 500ms-1s (medium timeout to enable failover)
   *
   * This implements startup election bias for preferred replica system.
   */
  // @safe - election timeout calculation (external calls wrapped in @unsafe blocks)
  uint64_t GetElectionTimeout();
 public:
  // @unsafe - returns borrowed communicator pointer from TxLogServer base.
  // The owning RaftFrame/RaftWorker lifetime must outlive this server use.
  RaftCommo* commo() {
    return (RaftCommo*) commo_;
  }

  slotid_t min_active_slot_ = 1; // anything before (lt) this slot is freed
  slotid_t max_executed_slot_ = 0;
  slotid_t max_committed_slot_ = 0;
  map<slotid_t, shared_ptr<RaftData>> logs_{};
  int n_vote_ = 0;
  int n_prepare_ = 0;
  int n_accept_ = 0;
  int n_commit_ = 0;

  /* NOTE: I think I should move these to the RaftData class */
  /* TODO: talk to Shuai about it */
  uint64_t lastLogIndex = 0;
  uint64_t currentTerm = 0;
  uint64_t commitIndex = 0;
  uint64_t executeIndex = 0;
  map<slotid_t, shared_ptr<RaftData>> raft_logs_{};
//  vector<shared_ptr<RaftData>> raft_logs_{};

  // For looping_ control usage, once ready_for_replication_ is ready (set to 1), a specific coroutine will do replication
  std::recursive_mutex ready_for_replication_mtx_{};
  shared_ptr<IntEvent> ready_for_replication_;

  // @safe - election timer setup (threading via @unsafe blocks in implementation)
  void StartElectionTimer() ;
  // @safe - calls Setup
  void EnsureSetup();

  // @safe
  bool IsLeader() override {
    // Defensive check: if we're shutting down (looping_=false),
    // return false to prevent accessing member variables during destruction
    if (!looping_) {
      return false;
    }
    return is_leader_ ;
  }
  
  // @safe - leadership state transition (callbacks and logging wrapped in @unsafe blocks)
  void setIsLeader(bool isLeader);

  // @safe - stores callback for later invocation
  void RegisterLeaderChangeCallback(rusty::Function<void(bool)> cb);

  // @safe - external calls marked @external, output pointer writes in @unsafe blocks
  // take janus::Command;
  // shared_ptr<Marshallable> callers auto-convert via Command's
  // implicit ctor.
  bool Start(const janus::Command& cmd, uint64_t *index, uint64_t *term, slotid_t slot_id = -1, ballot_t ballot = 1);

  // @unsafe - output pointer writes and mutex operations
  void GetState(bool *is_leader, uint64_t *term) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    // @unsafe
    {
      *is_leader = IsLeader();
      *term = currentTerm;
    }
  }

  // @safe - returns POD field
  uint64_t GetHeartbeatInterval() const { return heartbeat_interval_us_; }

  // @safe - sets POD field
  void SetHeartbeatInterval(uint64_t micros) { heartbeat_interval_us_ = micros; }

  // @unsafe - Implements ReadIndex protocol for linearizable reads.
  // Returns true if this server is confirmed leader and safe to serve reads.
  // Waits for executeIndex to catch up to commitIndex.
  bool ReadIndex(uint64_t timeout_us = 5000000);

  // @safe - returns POD field
  uint64_t GetLogRetentionWindow() const { return log_retention_window_; }

  // @safe - sets POD field (minimum 1 to avoid division by zero)
  void SetLogRetentionWindow(uint64_t window) { log_retention_window_ = (window > 0) ? window : 1; }

  // @unsafe - external calls plus output pointer writes and shared_ptr ops
  // take janus::Command;
  // shared_ptr<Marshallable> callers auto-convert via Command's
  // implicit ctor.
  void SetLocalAppend(const janus::Command& cmd, uint64_t* term, uint64_t* index, slotid_t slot_id = -1, ballot_t ballot = 1 ){
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    // @unsafe
    {
      *index = lastLogIndex ;
    }
    lastLogIndex += 1;
    auto instance = GetRaftInstance(lastLogIndex);
    instance->log_ = cmd;
		instance->prevTerm = currentTerm;
    instance->term = currentTerm;
		instance->slot_id = slot_id;
		instance->ballot = ballot;

    // CRITICAL: Persist log entry before replicating to followers
    // @unsafe
    {
      PersistLogEntry(lastLogIndex, *instance, "SetLocalAppend: leader log");
    }

    // @unsafe
    {
#ifndef RAFT_TEST_CORO
      if (cmd.kind_ == TpcCommitCommand::static_kind()){
        auto p_cmd = marshallable_cast<TpcCommitCommand>(cmd);
        auto vec_piece_data = marshallable_cast<VecPieceData>(p_cmd->cmd_);
        verify(vec_piece_data != nullptr);
        auto sp_vec_piece = vec_piece_data->sp_vec_piece_data_;

        // Check if this is Mako data (STR values) vs Janus data (I32 values)
        bool is_mako_data = false;
        if (sp_vec_piece && !sp_vec_piece->empty()) {
          auto first_cmd = (*sp_vec_piece)[0];
          if (first_cmd && first_cmd->input.values_ && !first_cmd->input.values_->empty()) {
            auto first_val = first_cmd->input.values_->begin()->second;
            if (first_val.get_kind() == Value::STR) {
              is_mako_data = true;
              Log_debug("[RAFT-SETLOCALAPPEND] Skipping vestigial I/O code for Mako data (STR values)");
            }
          }
        }

        if (!is_mako_data) {
          vector<struct KeyValue> kv_vector;
          int index = 0;
          for (auto it = sp_vec_piece->begin(); it != sp_vec_piece->end(); it++){
            auto cmd_input = (*it)->input.values_;
            for (auto it2 = cmd_input->begin(); it2 != cmd_input->end(); it2++) {
              struct KeyValue key_value = {it2->first, it2->second.get_i32()};
              kv_vector.push_back(key_value);
            }
          }

          struct KeyValue key_values[kv_vector.size()];
          std::copy(kv_vector.begin(), kv_vector.end(), key_values);
        }
      } else {
        int value = -1;
        int value_;
      }
#endif
    }

    // @unsafe
    {
      *term = currentTerm ;
    }
  }

  // @unsafe - map access and shared_ptr mutation
  shared_ptr<RaftData> GetInstance(slotid_t id) {
    verify(id >= min_active_slot_ || lastLogIndex == 0);
    auto& sp_instance = logs_[id];
    if(!sp_instance)
      sp_instance = std::make_shared<RaftData>();
    return sp_instance;
  }

 /* shared_ptr<RaftData> GetRaftInstance(slotid_t id) {
    if ( id <= raft_logs_.size() )
    {
        return raft_logs_[id-1] ;
    }
    auto sp_instance = std::make_shared<RaftData>();
    raft_logs_.push_back(sp_instance) ;
    return sp_instance;
  }*/

  // @unsafe - map access and shared_ptr mutation
   shared_ptr<RaftData> GetRaftInstance(slotid_t id) {
    if (id < min_active_slot_ && id != 0) {
      Log_info("[RAFT_LOG] expanding min_active_slot_ from %lu to %lu", min_active_slot_, id);
      min_active_slot_ = id;
    }
    auto& sp_instance = raft_logs_[id];
    if(!sp_instance)
      sp_instance = std::make_shared<RaftData>();
    return sp_instance;
   }


  // @safe - raw pointer parameter is bounded (frame outlives server)
  RaftServer(Frame *frame) ;
  // @unsafe - thread join and timer cleanup require manual resource management
  ~RaftServer() ;

  // ============================================================================
  // LOG PERSISTENCE PUBLIC API
  // ============================================================================

  /**
   * Set the log storage backend for persistence.
   * Should be called before starting the server.
   * @param storage Shared pointer to LogStorage implementation
   */
  // @unsafe - moves shared_ptr into member field
  void SetLogStorage(std::shared_ptr<janus::raft::LogStorage> storage) {
    log_storage_ = std::move(storage);
  }

  /**
   * Get the current log storage backend.
   * @return Shared pointer to LogStorage, or nullptr if not set
   */
  // @unsafe - returns copy of shared_ptr
  std::shared_ptr<janus::raft::LogStorage> GetLogStorage() const {
    return log_storage_;
  }

  /**
   * Recover state from persistent storage.
   * Restores currentTerm, vote_for_, commitIndex, and log entries.
   * Should be called during initialization if storage is available.
   * @return true if recovery succeeded or storage is not set, false on error
   */
  // @safe - recovery from storage (external calls wrapped in @unsafe blocks)
  bool RecoverFromStorage();

  /**
   * Replay committed entries after recovery.
   * Called after app_next_ callback is registered to apply recovered entries.
   * Must be called AFTER RegLearnerAction() sets up the callback.
   */
  // @safe - replays committed entries (callbacks wrapped in @unsafe blocks)
  void ReplayCommittedEntries();

  /**
   * Get count of uncommitted entries after recovery.
   * These entries will be resolved by the consensus protocol.
   * @return Number of uncommitted entries (lastLogIndex - commitIndex)
   */
  // @safe - Read-only accessor
  size_t GetUncommittedCount() const;

  // ============================================================================
  // SNAPSHOT SUPPORT PUBLIC API
  // ============================================================================

  /**
   * Set the snapshot manager for this server.
   * Should be called before starting the server.
   * @param manager Shared pointer to SnapshotManager implementation
   */
  // @unsafe - moves shared_ptr into member field
  void SetSnapshotManager(std::shared_ptr<janus::raft::SnapshotManager> manager) {
    snapshot_manager_ = std::move(manager);
  }

  /**
   * Get the current snapshot manager.
   * @return Shared pointer to SnapshotManager, or nullptr if not set
   */
  // @unsafe - returns copy of shared_ptr
  std::shared_ptr<janus::raft::SnapshotManager> GetSnapshotManager() const {
    return snapshot_manager_;
  }

  /**
   * Get the ReplicatedDB instance, if one was created during Setup().
   * @return Shared pointer to ReplicatedDB, or nullptr if not enabled
   */
  // @unsafe - returns copy of shared_ptr
  std::shared_ptr<ReplicatedDB> GetReplicatedDB() const {
    return replicated_db_;
  }

  /**
   * Set state machine snapshot callbacks.
   * Called by ReplicatedDB (or other state machines) to hook into
   * CreateSnapshot() and OnInstallSnapshot().
   * @param create_cb Returns serialized state machine snapshot data
   * @param load_cb Loads serialized state machine snapshot data
   */
    // @safe - stores move-only snapshot callbacks for later invocation
    void SetStateMachineSnapshotCallbacks(
        rusty::Function<std::string()> create_cb,
        rusty::Function<void(const std::string&)> load_cb) {
      create_sm_snapshot_cb_ = std::move(create_cb);
      load_sm_snapshot_cb_ = std::move(load_cb);
    }
  /**
   * Check if a snapshot is available.
   * @return true if a snapshot exists in the snapshot manager
   */
  // @safe - read-only query
  bool HasSnapshot() const;

  /**
   * Get the last log index included in the most recent snapshot.
   * @return Last included index, or 0 if no snapshot exists
   */
  // @safe - returns POD field
  uint64_t GetSnapshotIndex() const;

  /**
   * Get the term of the last log entry included in the most recent snapshot.
   * @return Last included term, or 0 if no snapshot exists
   */
  // @safe - returns POD field
  uint64_t GetSnapshotTerm() const;

  /**
   * Compact log entries up to the given index.
   * Removes entries from storage that are covered by a snapshot.
   * @param up_to_index Remove entries with index <= this value
   * @return Number of entries removed
   */
  // @safe - log compaction (storage operations wrapped in @unsafe blocks)
  size_t CompactLog(slotid_t up_to_index);

  /**
   * Set the snapshot threshold (number of entries between snapshots).
   * @param threshold Number of log entries applied before taking a snapshot
   */
  // @safe - sets POD field
  void SetSnapshotThreshold(uint64_t threshold) {
    snapshot_threshold_ = threshold;
  }

  /**
   * Get the current snapshot threshold.
   * @return Current threshold value
   */
  // @safe - reads POD field
  uint64_t GetSnapshotThreshold() const {
    return snapshot_threshold_;
  }

  // ============================================================================

  // @safe - calls doVote which is @safe, output pointer writes in @unsafe blocks
  void OnRequestVote(const slotid_t& lst_log_idx,
                     const ballot_t& lst_log_term,
                     const siteid_t& can_id,
                     const ballot_t& can_term,
                     ballot_t *reply_term,
                     bool_t *vote_granted) ;

  /**
   * VoteDurable RPC Handler - Speculative Voting Protocol
   *
   * Receives VoteDurable RPC from a follower after it has durably persisted
   * its vote to disk. This allows the leader to track durable votes separately
   * from memory votes, enabling speculative leader election.
   *
   * @param term - Term of the vote (must match current term)
   * @param voter_id - Site ID of the voter
   * @param acknowledged - [OUT] true if vote was recorded
   * @param cb - Callback to invoke when handling complete
   */
  // @unsafe - Modifies durableVoters_ and securedLeader_
  void OnVoteDurable(const ballot_t& term,
                     const siteid_t& voter_id,
                     bool_t* acknowledged);

  // @safe - external calls marked @external, output pointer writes in @unsafe blocks
  // take janus::Command;
  // shared_ptr<Marshallable> callers auto-convert via Command's
  // implicit ctor.
  void OnAppendEntries(const slotid_t slot_id,
                       const ballot_t ballot,
                       const uint64_t leaderCurrentTerm,
                       const siteid_t leaderSiteId,
                       const uint64_t leaderPrevLogIndex,
                       const uint64_t leaderPrevLogTerm,
                       const uint64_t leaderCommitIndex,
                       const janus::Command& cmd,
                       const uint64_t leaderNextLogTerm, // disabled in batched version (term recorded in the TpcCommitCommand)
                       uint64_t *followerAppendOK,
                       uint64_t *followerCurrentTerm,
                       uint64_t *followerLastLogIndex,
                       bool trigger_election_now = false);

  /**
   * AppendEntriesDurable RPC Handler - Speculative Commit Protocol
   *
   * Receives AppendEntriesDurable RPC from a follower after it has durably
   * persisted log entries to disk. This allows the leader to track durable
   * acknowledgments separately from memory acks, enabling speculative commits.
   *
   * @param term - Term when entries were persisted (must match current term)
   * @param follower_id - Site ID of the follower
   * @param lastLogIndex - Highest log index that is now durable on follower
   * @param acknowledged - [OUT] true if ack was recorded
   * @param cb - Callback to invoke when handling complete
   */
  // @unsafe - Modifies durableAcks_ and securedLogIndex_
  void OnAppendEntriesDurable(const ballot_t& term,
                              const siteid_t& follower_id,
                              const uint64_t& lastLogIndex,
                              bool_t* acknowledged);

  /**
   * TimeoutNow RPC Handler - Leadership Transfer Protocol
   *
   * Receives TimeoutNow RPC from current leader instructing this replica
   * to start an election immediately (bypass random election timeout).
   *
   * Used for deterministic leadership transfer to preferred replica.
   *
   * @param leaderTerm - Current leader's term
   * @param leaderSiteId - Current leader's site ID
   * @param followerTerm - [OUT] This replica's current term
   * @param success - [OUT] true if election started, false otherwise
   * @param cb - Callback to invoke when handling complete
   */
  
  // @safe - external calls marked @external, output pointer writes in @unsafe blocks
  void OnTimeoutNow(const uint64_t leaderTerm,
                    const siteid_t leaderSiteId,
                    uint64_t *followerTerm,
                    bool_t *success);

  /**
   * InstallSnapshot RPC Handler - Snapshot Transfer Protocol
   *
   * Receives a full snapshot from the leader when this follower is too far
   * behind to catch up via AppendEntries. Replaces the follower's state machine
   * state, updates snapshot metadata, discards old log entries, and advances
   * commitIndex/executeIndex.
   *
   * @param term - Leader's current term
   * @param leader_id - Leader's site ID
   * @param last_included_index - Last log index included in the snapshot
   * @param last_included_term - Term of the last included log entry
   * @param data - Serialized snapshot data
   * @param term_out - [OUT] Follower's current term (for leader to update itself)
   * @param cb - Callback to invoke when handling complete
   */
  // @unsafe - Modifies log state, snapshot metadata, calls snapshot_manager_
  void OnInstallSnapshot(const uint64_t term,
                         const uint64_t leader_id,
                         const uint64_t last_included_index,
                         const uint64_t last_included_term,
                         const std::string& data,
                         uint64_t* term_out);

  // ============================================================================
  // MEMBERSHIP CHANGE PUBLIC API
  // ============================================================================

  /**
   * Get the current quorum size based on current_config_.
   * @return Majority size: current_config_.size() / 2 + 1
   */
  // @safe - Read-only computation on member field
  size_t GetQuorumSize() const;

  /**
   * Get the current membership configuration.
   * @return Reference to the active replica set
   */
  // @safe - Read-only accessor
  // @lifetime: (&'a) -> &'a
  const std::set<siteid_t>& GetCurrentConfig() const;

  /**
   * Check if a server is a learner (being caught up, not yet in quorum).
   */
  // @unsafe - Read-only lookup on std::set
  bool IsLearner(siteid_t id) const { return learners_.count(id) > 0; }

  /**
   * Get the current set of learners.
   */
  // @unsafe - returns reference to internal state (no @lifetime annotation)
  const std::set<siteid_t>& GetLearners() const { return learners_; }

  /**
   * Promote a learner to full member in current_config_.
   * Removes from learners_, inserts into current_config_, clears pending flag.
   */
  // @unsafe - Modifies config state
  void PromoteLearner(siteid_t id);

  /**
   * Check if any learners are caught up and promote them to full members.
   * Called from HeartbeatLoop after commit index calculation.
   */
  // @unsafe - Calls PromoteLearner which modifies config state
  void CheckAndPromoteLearners();

  /**
   * AddServer RPC Handler - Membership Change Protocol
   *
   * Adds a new server to the cluster configuration. Only the leader can
   * process this request. Rejects if a config change is already pending.
   * The server is first added as a learner (receives log entries but does not
   * count for quorum). Once caught up, it is promoted to full member.
   *
   * @param term - Client's known term
   * @param new_server_id - Site ID of the server to add
   * @param addr - Address of the new server (host:port)
   * @param success - [OUT] true if config change was accepted
   * @param error_msg - [OUT] error description if rejected
   * @param leader_hint - [OUT] current leader's site ID (for redirect)
   * @param defer - Deferred reply
   */
  // @unsafe - Modifies config state
  void OnAddServer(const uint64_t term, const uint64_t new_server_id,
                   const std::string& addr,
                   bool_t* success, std::string* error_msg,
                   uint64_t* leader_hint);

  /**
   * RemoveServer RPC Handler - Membership Change Protocol
   *
   * Removes a server from the cluster configuration. Only the leader can
   * process this request. Rejects if a config change is already pending,
   * or if this would remove the last server.
   *
   * @param term - Client's known term
   * @param server_id - Site ID of the server to remove
   * @param success - [OUT] true if config change was accepted
   * @param error_msg - [OUT] error description if rejected
   * @param leader_hint - [OUT] current leader's site ID (for redirect)
   * @param defer - Deferred reply
   */
  // @unsafe - Modifies config state
  void OnRemoveServer(const uint64_t term, const uint64_t server_id,
                      bool_t* success, std::string* error_msg,
                      uint64_t* leader_hint);

  // @unsafe - modifies proxy maps with C-style casts on raw pointers (non-trivial pointer arithmetic)
  void Disconnect(const bool disconnect = true);

  // @safe - calls Disconnect (wrapped in @unsafe block) and resetTimer
  void Reconnect() {
    // @unsafe
    {
      Disconnect(false);
    }
    // @unsafe
    { resetTimer("reconnect"); }
  }

  // @safe
  bool IsDisconnected();

  // @safe - verify(0) is always-abort, no actual unsafe operations
  virtual bool HandleConflicts(Tx& dtxn,
                               innid_t inn_id,
                               vector<string>& conflicts) {
    verify(0);
  };

  // @safe - external calls marked @external
  void removeCmd(slotid_t slot);

  // ============================================================================
  // PUBLIC API: Preferred Replica System - Leadership Transfer
  // ============================================================================

  /**
   * Set the preferred leader for this Raft group.
   *
   * @param site_id The site ID of the preferred leader (or INVALID_SITEID to disable)
   *
   * Behavior:
   * - All replicas should call this with the same site_id
   * - Standard Raft voting happens (any replica can win initial election)
   * - Non-preferred leaders monitor for preferred replica
   * - When preferred is alive and caught up, non-preferred leader transfers leadership
   *
   * Safety: This maintains all Raft safety guarantees via explicit transfer protocol.
   */
  // @unsafe - Log_info plus mutex operations
  void SetPreferredLeader(siteid_t site_id) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    siteid_t old_preferred = preferred_leader_site_id_;
    preferred_leader_site_id_ = site_id;

    if (old_preferred != site_id) {
      Log_info("[LEADERSHIP-TRANSFER] Site %d: Preferred leader set to %d",
               site_id_, site_id);
    }

    // If I'm a non-preferred leader, start monitoring for transfer opportunity
    if (!AmIPreferredLeader() && is_leader_ && looping_) {
      Log_info("[LEADERSHIP-TRANSFER] Site %d: I'm non-preferred leader, starting transfer monitoring",
               site_id_);
      StartLeadershipTransferMonitoring();
    }
  }

  /**
   * Get the current preferred leader site ID
   * @return Preferred leader site ID, or INVALID_SITEID if none
   */
  // @safe
  siteid_t GetPreferredLeader() const {
    return preferred_leader_site_id_;
  }

  /**
   * Get the last known leader's site_id for client redirection.
   * @return Leader site_id, or INVALID_SITEID if unknown
   */
  // @safe - read-only access to current_leader_id_ under lock
  siteid_t GetLeaderHint() const;

  /**
   * Check if leadership transfer should be initiated
   * Called by non-preferred leaders to check if preferred replica is ready
   */
  // @safe - checks conditions for leadership transfer (mutex/map access via @unsafe blocks)
  bool ShouldTransferLeadership();

  // @safe - initiates leadership transfer (RPC/mutex via @unsafe blocks)
  void InitiateLeadershipTransfer();

  // @safe - starts monitor thread (threading via @unsafe blocks)
  void StartLeadershipTransferMonitoring();

  // @safe - stops monitor thread (threading via @unsafe blocks)
  void StopLeadershipTransferMonitoring();

  // ============================================================================
  // PUBLIC API: Speculative Replication State
  // ============================================================================

  /**
   * Check if this leader has achieved secured status (durable vote quorum).
   * When securedLeader_ = true, a quorum has votedFor = me on disk,
   * so no other candidate can win election in this term.
   * @return true if leader has durable vote quorum
   */
  // @safe - Read-only accessor
  bool IsSecuredLeader() const {
    return securedLeader_;
  }

  /**
   * Get the speculative commit index (highest index with memory ack quorum).
   * @return specCommitIndex value
   */
  // @safe - Read-only accessor
  uint64_t GetSpecCommitIndex() const {
    return specCommitIndex_;
  }

  /**
   * Get the secured log index (highest index with durable ack quorum).
   * Invariant: securedLogIndex <= specCommitIndex <= lastLogIndex
   * @return securedLogIndex value
   */
  // @safe - Read-only accessor
  uint64_t GetSecuredLogIndex() const {
    return securedLogIndex_;
  }

  /**
   * Get the set of servers that have memory-voted for us in current term.
   * @return copy of specVoters set
   */
  // @unsafe - Returns copy, read-only access
  std::set<siteid_t> GetSpecVoters() const {
    return specVoters_;
  }

  /**
   * Get the count of servers that have memory-voted for us in current term.
   * @return Number of servers in specVoters
   */
  // @unsafe - Read-only accessor on std::set
  size_t GetSpecVotersCount() const {
    return specVoters_.size();
  }

  /**
   * Get the set of servers that have durably-voted for us in current term.
   * @return copy of durableVoters set
   */
  // @unsafe - Returns copy, read-only access
  std::set<siteid_t> GetDurableVoters() const {
    return durableVoters_;
  }

  /**
   * Get the count of servers that have durably-voted for us in current term.
   * @return Number of servers in durableVoters
   */
  // @unsafe - Read-only accessor on std::set
  size_t GetDurableVotersCount() const {
    return durableVoters_.size();
  }

  /**
   * Get the last log index.
   * @return lastLogIndex value
   */
  // @safe - Read-only accessor
  uint64_t GetLastLogIndex() const {
    return lastLogIndex;
  }

  /**
   * Get the number of memory acks for a specific log index.
   * @param index Log index to query
   * @return Number of nodes that have memory-acked this index
   */
  // @unsafe - Read-only accessor on std::map
  size_t GetMemoryAckCount(uint64_t index) const {
    auto it = memoryAcks_.find(index);
    return it != memoryAcks_.end() ? it->second.size() : 0;
  }

  /**
   * Get the number of durable acks for a specific log index.
   * @param index Log index to query
   * @return Number of nodes that have durably-acked this index
   */
  // @unsafe - Read-only accessor on std::map
  size_t GetDurableAckCount(uint64_t index) const {
    auto it = durableAcks_.find(index);
    return it != durableAcks_.end() ? it->second.size() : 0;
  }

  /**
   * Reset speculative state when becoming leader or stepping down.
   * Called during leadership transitions.
   *
   * On becoming leader:
   * - specVoters = {self}  (voted for self)
   * - durableVoters = {self}  (self vote is always durable)
   * - securedLogIndex = commitIndex (from previous term)
   * - specCommitIndex = commitIndex
   *
   * On stepping down:
   * - All speculative state is cleared
   */
  // @unsafe - Modifies state
  void ResetSpeculativeState();

  /**
   * Verify speculative state invariants.
   * Debug helper - asserts if invariants are violated.
   * Invariants:
   * - securedLogIndex <= specCommitIndex <= lastLogIndex
   * - durableVoters ⊆ specVoters (conceptually, not strictly enforced after crashes)
   */
  // @safe - Read-only check
  void VerifySpeculativeInvariants() const;

  /**
   * Handle notification that a peer has restarted.
   *
   * Called when we receive notifyRestart from another server. For speculative
   * replication, this means the restarted server has lost:
   * 1. Its memory vote (if it voted but didn't fsync)
   * 2. Memory-acked log entries (not yet fsynced)
   *
   * This method invalidates any speculative state that depended on the
   * restarted server and may trigger step-down if we're an unsecured leader
   * who has lost speculative quorum.
   *
   * @param restarted_site_id - Site ID of the server that restarted
   */
  // @unsafe - Modifies speculative state
  void OnPeerRestart(siteid_t restarted_site_id);

  /**
   * Step down as leader with specified reason.
   *
   * This is the central function for leader step-down in speculative Raft.
   * It handles:
   * 1. Logging the step-down event with reason
   * 2. Resetting speculative state
   * 3. Transitioning to follower state
   * 4. Resetting election timer
   *
   * Future: Will also notify pending clients based on reason:
   * - UnsecuredFailure: Rollback all current-term entries
   * - SecuredFailure: Rollback only unsecured entries
   * - HigherTerm: No automatic rollback (entries may still be valid)
   *
   * @param reason - Why the leader is stepping down
   */
  // @unsafe - Modifies state, calls setIsLeader
  void stepDown(StepDownReason reason);

  // ===========================================================================
  // CLIENT NOTIFICATION CALLBACKS
  // ===========================================================================

  /**
   * Register a callback to be notified when an entry's commit status changes.
   *
   * The callback will be invoked with:
   * - SPECULATIVE: When entry reaches memory quorum (specCommitIndex advances)
   * - DURABLE: When entry reaches disk quorum with secured leader
   * - ROLLEDBACK: If leader steps down gracefully (best-effort)
   *
   * Note: Callback is invoked while holding mtx_, keep it lightweight.
   * If index is already at or past the requested state, callback is invoked
   * immediately.
   *
   * @param index - Log index to monitor
   * @param callback - Function to call on status change
   */
  // @unsafe - Modifies pendingCallbacks_
  void RegisterCommitCallback(uint64_t index,
                            rusty::Function<void(CommitStatus)> callback);

  /**
   * Notify all registered callbacks for indices in range (from, to] with status.
   * Used internally by specCommitIndex/securedLogIndex advancement handlers.
   *
   * @param from - Exclusive lower bound
   * @param to - Inclusive upper bound
   * @param status - Commit status to notify
   */
  // @unsafe - Invokes callbacks, modifies pendingCallbacks_
  void NotifyCallbacks(uint64_t from, uint64_t to, CommitStatus status);

  /**
   * Notify rollback for pending callbacks based on step-down reason.
   * Called during step-down when leader is still alive.
   *
   * Behavior per reason:
   * - UnsecuredFailure: Rollback all entries in (commitIndex, lastLogIndex]
   * - SecuredFailure: Rollback only unsecured entries in (securedLogIndex_, specCommitIndex_]
   * - HigherTerm: No automatic rollback (entries may still be valid under new leader)
   *
   * Always clears pendingCallbacks_ and resets notification tracking regardless of reason.
   *
   * @param reason - Why the leader is stepping down
   */
  // @unsafe - Invokes callbacks, clears pendingCallbacks_
  void NotifyRollback(StepDownReason reason);
};
} // namespace janus
