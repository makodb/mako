#pragma once

#include "../__dep__.h"
#include "../constants.h"
#include "../scheduler.h"
#include "../classic/tpc_command.h"
#include "commo.h"
#include <rusty/box.hpp>

namespace janus {
class Command;
class CmdData;

#define INVALID_SITEID  ((siteid_t)-1)
#define NUM_BATCH_TIMER_RESET  (100)
#define SEC_BATCH_TIMER_RESET  (1)

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
 private:
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


  // @safe - Check if I am the preferred leader
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

  // @unsafe - Returns raw pointer cast
  RaftCommo* commo() {
    return (RaftCommo*) commo_;
  }

  // @unsafe
  void doVote(const slotid_t& lst_log_idx,
              const ballot_t& lst_log_term,
              const siteid_t& can_id,
              const ballot_t& can_term,
              ballot_t *reply_term,
              bool_t *vote_granted,
              bool_t vote,
              const function<void()> &cb) {
      *vote_granted = vote ;
      *reply_term = currentTerm ;
#ifdef RAFT_LEADER_ELECTION_DEBUG
      siteid_t prev_vote_for = vote_for_;
      Log_info("[RAFT_VOTE] server %d (loc %d) vote=%d candidate=%d can_term=%lu cur_term=%lu prev_vote_for=%d is_leader=%d lst_idx=%lu lst_term=%lu",
               site_id_, loc_id_, vote, can_id, can_term, currentTerm, prev_vote_for, is_leader_, lst_log_idx, lst_log_term);
#endif
                    
      if( can_term > currentTerm)
      {
          // is_leader_ = false ;  // TODO recheck
          // Any higher term seen means we must immediately step down.
          setIsLeader(false);
          auto prev_term = currentTerm;
          currentTerm = can_term ;
          vote_for_ = INVALID_SITEID;  // Reset vote when advancing to new term
          LogTermChange("vote request carried newer term", prev_term, currentTerm, can_id);
      }

      if(vote)
      {
          setIsLeader(false) ;
          vote_for_ = can_id ;
#ifdef RAFT_LEADER_ELECTION_DEBUG
          Log_info("[RAFT_VOTE] server %d recorded vote_for=%d at term=%lu", site_id_, vote_for_, currentTerm);
#endif
          //reset timeout
          resetTimer("granted vote");
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

  // @safe
  double randDuration()
  {
    // election timeout between 0.4 and 0.7 seconds
    return RandomGenerator::rand_double(0.4, 0.7) ;
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

  // @safe
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
        // de->Wait();
        //clock_gettime(CLOCK_MONOTONIC, &end);
        //Log_info("Time of Write: %d", end.tv_nsec - begin.tv_nsec);
      }
    } else {
			int value = -1;
			int value_;
			// auto de = IO::write(filename, &value, sizeof(int), 1);
			struct timespec begin, end;
			//clock_gettime(CLOCK_MONOTONIC, &begin);
      // de->Wait();
			//clock_gettime(CLOCK_MONOTONIC, &end);
			//Log_info("Time of Write: %d", end.tv_nsec - begin.tv_nsec);
    }
#endif
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

  // @unsafe - Calls undeclared doVote() and uses std::function callback
  void OnRequestVote(const slotid_t& lst_log_idx,
                     const ballot_t& lst_log_term,
                     const siteid_t& can_id,
                     const ballot_t& can_term,
                     ballot_t *reply_term,
                     bool_t *vote_granted,
                     const function<void()> &cb) ;

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
                       const function<void()> &cb,
                       bool trigger_election_now = false);

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
                    const function<void()> &cb);

  // @unsafe - Modifies connection state and proxy maps
  void Disconnect(const bool disconnect = true);

  // @safe - Calls Disconnect (@unsafe) and resetTimer (@unsafe)
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
};
} // namespace janus
