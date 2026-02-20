#pragma once

#include "../__dep__.h"
#include "../constants.h"
#include "../scheduler.h"
#include "../classic/tpc_command.h"
#include "commo.h"
#include <rusty/box.hpp>
#include <rusty/arc.hpp>
#include "rpc/log_storage.hpp"
#include "rpc/recovery_manager.hpp"
#include "rpc/snapshot_manager.hpp"

namespace janus {
class Command;
class CmdData;

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
enum class StepDownReason {
  UnsecuredFailure,  // Lost spec quorum while unsecured
  SecuredFailure,    // Lost quorum but was secured
  HigherTerm         // Saw higher term from another server
};

/**
 * CommitStatus - Notification status for client callbacks
 *
 * Used by client callback infrastructure to notify clients of entry status:
 * - SPECULATIVE: Entry reached memory quorum, likely to commit
 * - DURABLE: Entry reached disk quorum with secured leader, guaranteed
 * - ROLLEDBACK: Entry will not commit (leader stepped down gracefully)
 */
enum class CommitStatus {
  SPECULATIVE,  // Entry reached memory quorum
  DURABLE,      // Entry reached disk quorum with secured leader
  ROLLEDBACK    // Entry will not commit (best-effort notification)
};

struct RaftData {
  ballot_t max_ballot_seen_ = 0;
  ballot_t max_ballot_accepted_ = 0;
  shared_ptr<Marshallable> accepted_cmd_{nullptr};
  shared_ptr<Marshallable> committed_cmd_{nullptr};

  ballot_t term;
  shared_ptr<Marshallable> log_{nullptr};

	//for retries
	ballot_t prevTerm;
	slotid_t slot_id;
	ballot_t ballot;
};

struct KeyValue {
	int key;
	i32 value;
};

#ifdef RAFT_TEST_CORO
#define HEARTBEAT_INTERVAL 100000
#else
#define HEARTBEAT_INTERVAL 5000
#endif


class RaftServer : public TxLogServer {
  friend class RaftTestConfig;  // Allow test config to access private members for kill/restart
 private:
  // ============================================================================
  // LOG PERSISTENCE (Phase 1.3)
  // ============================================================================
  std::shared_ptr<rrr::LogStorage> log_storage_;  // Optional persistent storage
  bool async_persistence_ = false;  // Runtime: sync (default) vs async disk persistence

  // ============================================================================
  // SNAPSHOT SUPPORT (Phase 3.1)
  // ============================================================================
  std::shared_ptr<rrr::SnapshotManager> snapshot_manager_;  // Optional snapshot manager

  // Metadata keys for LogStorage persistence
  static constexpr const char* META_TERM = "currentTerm";
  static constexpr const char* META_VOTE_FOR = "vote_for";
  static constexpr const char* META_COMMIT_INDEX = "commitIndex";

  // LogStorage-based persistence helper methods
  void PersistTermAndVoteToLogStorage();
  void PersistVoteToLogStorage();
  void PersistCommitIndexToLogStorage();
  void PersistLogEntryToLogStorage(slotid_t slot_id, const RaftData& data);
  void PersistLogEntriesToLogStorage(const std::vector<std::pair<slotid_t, std::shared_ptr<RaftData>>>& entries);

  // ============================================================================

  std::map<siteid_t, uint64_t> match_index_{};
  std::map<siteid_t, uint64_t> next_index_{};
  std::vector<std::thread> timer_threads_ = {};
  void timer_thread(bool *vote) ;
  rusty::Box<Timer> timer_;  // Owned timer, auto-cleaned on destruction
  uint64_t last_heartbeat_time_ = 0;
  void LogTermChange(const char* reason, uint64_t old_term, uint64_t new_term, siteid_t source = INVALID_SITEID);
  bool stop_ = false ;
  siteid_t vote_for_ = INVALID_SITEID ;
  bool init_ = false ;
  bool is_leader_ = false ;
  slotid_t snapidx_ = 0 ;
  ballot_t snapterm_ = 0 ;
  int32_t wait_int_ = 100000 ;
  bool disconnected_ = false;
  bool req_voting_ = false ;
  bool in_applying_logs_ = false ;
  std::atomic<bool> apply_pending_{false};  // Tracks if new work arrived while applying logs
#ifdef RAFT_TEST_CORO
  bool failover_{true} ;
#else
  bool failover_{true} ;
#endif
  atomic<int64_t> counter_{0};
  const char *filename = "/db/data.txt";

  bool looping_ = false;
  bool heartbeat_ = true;
  bool heartbeat_setup_ = false;
	enum { STOPPED, RUNNING } status_;
	std::function<void(bool)> leader_change_cb_{};

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
  std::atomic<bool> leadership_monitor_stop_{false};       // Signal to stop monitoring thread
  std::thread leadership_monitor_thread_;                   // Background thread monitoring for transfer
  uint64_t startup_timestamp_ = 0;                          // When server started (for grace period)

  // ============================================================================
  // SPECULATIVE REPLICATION STATE (Phase 1.1)
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
  std::mutex async_threads_mtx_;
  std::vector<std::pair<std::thread, rusty::Arc<AtomicFlag>>> async_threads_;

  // Client notification callbacks
  // Key: log index, Value: callback to notify on commit status change
  // Callbacks are invoked with: SPECULATIVE (memory quorum), DURABLE (disk quorum),
  // or ROLLEDBACK (leader stepped down gracefully)
  std::map<uint64_t, std::function<void(CommitStatus)>> pendingCallbacks_;
  uint64_t lastSpecNotifiedIndex_ = 0;    // last index notified with SPECULATIVE
  uint64_t lastDurableNotifiedIndex_ = 0; // last index notified with DURABLE

  // @unsafe - Uses INVALID_SITEID macro with integer cast
  bool AmIPreferredLeader() const {
    return preferred_leader_site_id_ != INVALID_SITEID &&
           site_id_ == preferred_leader_site_id_;
  }

  // @safe - Check if I have caught up to the current leader's commit level
  bool HaveCaughtUp() const {
    // We've caught up if our commitIndex >= leader's last known commitIndex
    // Note: leader_last_commit_index_ is updated from AppendEntries heartbeats
    return commitIndex >= leader_last_commit_index_;
  }

  // ============================================================================
  
	bool RequestVote() ;

	void Setup();
	void HeartbeatLoop() ;

  // @unsafe
  // SPECULATIVE VOTING: Respond immediately, then persist and send VoteDurable async
  void doVote(const slotid_t& lst_log_idx,
              const ballot_t& lst_log_term,
              const siteid_t& can_id,
              const ballot_t& can_term,
              ballot_t *reply_term,
              bool_t *vote_granted,
              bool_t vote,
              rusty::Function<void()> cb) {
      *vote_granted = vote ;
      *reply_term = currentTerm ;
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
          vote_for_ = INVALID_SITEID;  // Reset vote when advancing to new term

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
            // SPECULATIVE VOTING (async mode): Respond IMMEDIATELY (memory vote)
            // Then start async persistence and send VoteDurable after fsync
            n_vote_++ ;
            cb() ;  // Respond now - this is the memory vote

            // Start async vote persistence
            // Capture necessary state for the async operation
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

            return;  // Already called cb() above
          } else {
            // SYNC PERSISTENCE (traditional Raft): Persist FIRST, then respond
            // No separate VoteDurable RPC needed - the vote response implies durability
            PersistState(currentTerm, can_id, "doVote: sync vote persist");
            n_vote_++ ;
            cb() ;
            return;
          }
      }

      n_vote_++ ;
      cb() ;
  }

  void applyLogs();

  void resetTimerBatch()
  {
    // Log_info("!!!!!!! if (!failover_)");
    if (!failover_) return ;
    auto cur_count = counter_++;
    if (cur_count > NUM_BATCH_TIMER_RESET ) {
      if (timer_->elapsed() > SEC_BATCH_TIMER_RESET) {
        resetTimer("batch timer adjustment");
      }
      counter_.store(0);
    }
  }
  void OnJetpackPullCmd(const epoch_t& jepoch,
                        const epoch_t& oepoch,
                        const std::vector<key_t>& keys,
                        bool_t* ok,
                        epoch_t* reply_jepoch,
                        epoch_t* reply_oepoch,
                        MarshallDeputy* reply_old_view,
                        MarshallDeputy* reply_new_view,
                        shared_ptr<KeyCmdBatchData>& batch) override;

  // @unsafe - Modifies timer state, calls timer_->start()
  void resetTimer(const char* reason = "unspecified") {
    const char* why = reason ? reason : "unspecified";
    auto prev_time = last_heartbeat_time_;
    last_heartbeat_time_ = Time::now();
    // Log only important timer resets (elections, votes), not routine heartbeats
    if (strcmp(why, "granted vote") == 0 || strcmp(why, "start election timer") == 0) {
      Log_info("[TIMER_RESET] Site %d: reset timer (%s) - prev_hb_time=%lu new_hb_time=%lu delta=%lu",
               site_id_, why, prev_time, last_heartbeat_time_, last_heartbeat_time_ - prev_time);
    }
    if (failover_) {
      timer_->start() ;
    }
  }

  // @unsafe - Calls RandomGenerator::rand_double (not annotated)
  double randDuration()
  {
    // election timeout between 0.4 and 0.7 seconds
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
  uint64_t GetElectionTimeout();
 public:
  // @unsafe - Returns raw pointer cast
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

  void StartElectionTimer() ;
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
  
  // Made public to allow Jetpack recovery to restore leader state
  void setIsLeader(bool isLeader);

  void RegisterLeaderChangeCallback(std::function<void(bool)> cb);

  // @unsafe
  bool Start(shared_ptr<Marshallable> &cmd, uint64_t *index, uint64_t *term, slotid_t slot_id = -1, ballot_t ballot = 1);

  // @unsafe - Dereferences raw pointers
  void GetState(bool *is_leader, uint64_t *term) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    *is_leader = IsLeader();
    *term = currentTerm;
  }

  // @unsafe
  void SetLocalAppend(shared_ptr<Marshallable>& cmd, uint64_t* term, uint64_t* index, slotid_t slot_id = -1, ballot_t ballot = 1 ){
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    *index = lastLogIndex ;
    lastLogIndex += 1;
    auto instance = GetRaftInstance(lastLogIndex);
    instance->log_ = cmd;
		instance->prevTerm = currentTerm;
    instance->term = currentTerm;
		instance->slot_id = slot_id;
		instance->ballot = ballot;

    // CRITICAL: Persist log entry before replicating to followers
    PersistLogEntry(lastLogIndex, *instance, "SetLocalAppend: leader log");

#ifndef RAFT_TEST_CORO
    if (cmd->kind_ == MarshallDeputy::CMD_TPC_COMMIT){
      auto p_cmd = dynamic_pointer_cast<TpcCommitCommand>(cmd);
      auto sp_vec_piece = dynamic_pointer_cast<VecPieceData>(p_cmd->cmd_)->sp_vec_piece_data_;

      // Check if this is Mako data (STR values) vs Janus data (I32 values)
      // Mako sends raw serialized transaction bytes wrapped as String values
      // This vestigial code was written for Janus I32 key-value pairs
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

      // Only process if this is Janus data (I32 values)
      // Skip for Mako data to avoid get_i32() crash on String values
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

        // auto de = IO::write(filename, key_values, sizeof(struct KeyValue), kv_vector.size());

        struct timespec begin, end;
        //clock_gettime(CLOCK_MONOTONIC, &begin);
        // de->wait();
        //clock_gettime(CLOCK_MONOTONIC, &end);
        //Log_info("Time of Write: %d", end.tv_nsec - begin.tv_nsec);
      }
    } else {
			int value = -1;
			int value_;
			// auto de = IO::write(filename, &value, sizeof(int), 1);
			struct timespec begin, end;
			//clock_gettime(CLOCK_MONOTONIC, &begin);
      // de->wait();
			//clock_gettime(CLOCK_MONOTONIC, &end);
			//Log_info("Time of Write: %d", end.tv_nsec - begin.tv_nsec);
    }
#endif

    // Persist the log entry
    PersistLogEntry(lastLogIndex, *instance);

    *term = currentTerm ;
  }

  // @safe
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

  // @unsafe
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


  RaftServer(Frame *frame) ;
  ~RaftServer() ;

  // ============================================================================
  // LOG PERSISTENCE PUBLIC API (Phase 1.3)
  // ============================================================================

  /**
   * Set the log storage backend for persistence.
   * Should be called before starting the server.
   * @param storage Shared pointer to LogStorage implementation
   */
  // @safe - Simple setter
  void SetLogStorage(std::shared_ptr<rrr::LogStorage> storage) {
    log_storage_ = std::move(storage);
  }

  /**
   * Get the current log storage backend.
   * @return Shared pointer to LogStorage, or nullptr if not set
   */
  // @safe - Simple getter
  std::shared_ptr<rrr::LogStorage> GetLogStorage() const {
    return log_storage_;
  }

  /**
   * Recover state from persistent storage.
   * Restores currentTerm, vote_for_, commitIndex, and log entries.
   * Should be called during initialization if storage is available.
   * @return true if recovery succeeded or storage is not set, false on error
   */
  // @unsafe - Uses LogStorage operations
  bool RecoverFromStorage();

  /**
   * Replay committed entries after recovery.
   * Called after app_next_ callback is registered to apply recovered entries.
   * Must be called AFTER RegLearnerAction() sets up the callback.
   */
  // @unsafe - Calls app_next_ which may have side effects
  void ReplayCommittedEntries();

  /**
   * Get count of uncommitted entries after recovery.
   * These entries will be resolved by the consensus protocol.
   * @return Number of uncommitted entries (lastLogIndex - commitIndex)
   */
  // @safe - Read-only accessor
  size_t GetUncommittedCount() const;

  // ============================================================================
  // SNAPSHOT SUPPORT PUBLIC API (Phase 3.1)
  // ============================================================================

  /**
   * Set the snapshot manager for this server.
   * Should be called before starting the server.
   * @param manager Shared pointer to SnapshotManager implementation
   */
  // @safe - Simple setter
  void SetSnapshotManager(std::shared_ptr<rrr::SnapshotManager> manager) {
    snapshot_manager_ = std::move(manager);
  }

  /**
   * Get the current snapshot manager.
   * @return Shared pointer to SnapshotManager, or nullptr if not set
   */
  // @safe - Simple getter
  std::shared_ptr<rrr::SnapshotManager> GetSnapshotManager() const {
    return snapshot_manager_;
  }

  /**
   * Compact log entries up to the given index (Phase 3.4).
   * Removes entries from storage that are covered by a snapshot.
   * @param up_to_index Remove entries with index <= this value
   * @return Number of entries removed
   */
  // @unsafe - Modifies log storage
  size_t CompactLog(slotid_t up_to_index);

  // ============================================================================

  // @unsafe - Calls undeclared doVote() and uses std::function callback
  void OnRequestVote(const slotid_t& lst_log_idx,
                     const ballot_t& lst_log_term,
                     const siteid_t& can_id,
                     const ballot_t& can_term,
                     ballot_t *reply_term,
                     bool_t *vote_granted,
                     rusty::Function<void()> cb) ;

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
                     bool_t* acknowledged,
                     rusty::Function<void()> cb);

  // @unsafe
  void OnAppendEntries(const slotid_t slot_id,
                       const ballot_t ballot,
                       const uint64_t leaderCurrentTerm,
                       const siteid_t leaderSiteId,
                       const uint64_t leaderPrevLogIndex,
                       const uint64_t leaderPrevLogTerm,
                       const uint64_t leaderCommitIndex,
                       shared_ptr<Marshallable> &cmd,
                       const uint64_t leaderNextLogTerm, // disabled in batched version (term recorded in the TpcCommitCommand)
                       uint64_t *followerAppendOK,
                       uint64_t *followerCurrentTerm,
                       uint64_t *followerLastLogIndex,
                       rusty::Function<void()> cb,
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
                              bool_t* acknowledged,
                              rusty::Function<void()> cb);

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
  
  // @unsafe
  void OnTimeoutNow(const uint64_t leaderTerm,
                    const siteid_t leaderSiteId,
                    uint64_t *followerTerm,
                    bool_t *success,
                    rusty::Function<void()> cb);

  // @unsafe - Modifies connection state and proxy maps
  void Disconnect(const bool disconnect = true);

  // @unsafe - Calls Disconnect (@unsafe) and resetTimer (@unsafe)
  void Reconnect() {
    Disconnect(false);
    resetTimer("reconnect");
  }

  bool IsDisconnected();

  virtual bool HandleConflicts(Tx& dtxn,
                               innid_t inn_id,
                               vector<string>& conflicts) {
    verify(0);
  };

  // @unsafe
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
  siteid_t GetPreferredLeader() const {
    return preferred_leader_site_id_;
  }

  /**
   * Check if leadership transfer should be initiated
   * Called by non-preferred leaders to check if preferred replica is ready
   */
  bool ShouldTransferLeadership();

  /**
   * Initiate leadership transfer to preferred replica
   * Called by non-preferred leader when preferred is caught up
   */
  void InitiateLeadershipTransfer();

  /**
   * Start background monitoring for leadership transfer opportunities
   * Called by non-preferred leaders
   */
  void StartLeadershipTransferMonitoring();

  /**
   * Stop leadership transfer monitoring
   */
  void StopLeadershipTransferMonitoring();

  // ============================================================================
  // PUBLIC API: Speculative Replication State (Phase 1.1)
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
  // @safe - Returns copy, read-only access
  std::set<siteid_t> GetSpecVoters() const {
    return specVoters_;
  }

  /**
   * Get the count of servers that have memory-voted for us in current term.
   * @return Number of servers in specVoters
   */
  // @safe - Read-only accessor
  size_t GetSpecVotersCount() const {
    return specVoters_.size();
  }

  /**
   * Get the set of servers that have durably-voted for us in current term.
   * @return copy of durableVoters set
   */
  // @safe - Returns copy, read-only access
  std::set<siteid_t> GetDurableVoters() const {
    return durableVoters_;
  }

  /**
   * Get the count of servers that have durably-voted for us in current term.
   * @return Number of servers in durableVoters
   */
  // @safe - Read-only accessor
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
  // @safe - Read-only accessor
  size_t GetMemoryAckCount(uint64_t index) const {
    auto it = memoryAcks_.find(index);
    return it != memoryAcks_.end() ? it->second.size() : 0;
  }

  /**
   * Get the number of durable acks for a specific log index.
   * @param index Log index to query
   * @return Number of nodes that have durably-acked this index
   */
  // @safe - Read-only accessor
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
                              std::function<void(CommitStatus)> callback);

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
   * Notify rollback for all pending callbacks above securedLogIndex.
   * Called during step-down when leader is still alive.
   */
  // @unsafe - Invokes callbacks, clears pendingCallbacks_
  void NotifyRollback();
};
} // namespace janus
