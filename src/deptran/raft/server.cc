#include "server.h"
// #include "paxos_worker.h"
#include "exec.h"
#include "frame.h"
#include "coordinator.h"
#include "../classic/tpc_command.h"

// @external: {
//   rrr::RandomGenerator::rand_double: [unsafe, (double, double) -> double]
//   rrr::RandomGenerator::rand: [unsafe, (int, int) -> int]
//   std::make_shared: [unsafe, (...) -> owned]
//   rrr::Fiber::CreateRun: [unsafe, (...) -> owned]
//   std::map::find: [unsafe, (&'a, ...) -> iterator]
//   std::map::insert: [unsafe, (&'a mut, ...) -> pair]
//   std::map::end: [unsafe, (&'a) -> iterator]
//   operator bool: [unsafe, (&'a) -> bool]
//   std::atomic::store: [unsafe, (&'a mut, ...) -> void]
//   std::atomic::load: [unsafe, (&'a) -> T]
//   std::recursive_mutex::lock: [unsafe, (&'a mut) -> void]
//   std::recursive_mutex::unlock: [unsafe, (&'a mut) -> void]
//   std::vector::push_back: [unsafe, (&'a mut, T) -> void]
//   std::vector::operator[]: [unsafe, (&'a, size_t) -> &'a]
//   std::sort: [unsafe, (iterator, iterator) -> void]
//   std::dynamic_pointer_cast: [unsafe, (...) -> owned]
//   std::shared_ptr::operator=: [unsafe, (&'a mut, &'a) -> &'a mut]
//   janus::TpcBatchCommand::AddCmds: [unsafe, (&'a mut, &'a mut) -> void]
// }

namespace janus {

// ============================================================================
// LOG PERSISTENCE IMPLEMENTATION (Phase 1.3)
// ============================================================================

// @unsafe - Uses LogStorage API
void RaftServer::PersistTermAndVoteToLogStorage() {
  if (!log_storage_ || !log_storage_->is_open()) {
    return;
  }

  log_storage_->set_metadata(META_TERM, std::to_string(currentTerm));  // @unsafe
  log_storage_->set_metadata(META_VOTE_FOR, std::to_string(static_cast<int64_t>(vote_for_)));  // @unsafe
  log_storage_->sync();  // @unsafe - Durability guarantee
}

// @unsafe - Uses LogStorage API
void RaftServer::PersistVoteToLogStorage() {
  if (!log_storage_ || !log_storage_->is_open()) {
    return;
  }

  log_storage_->set_metadata(META_VOTE_FOR, std::to_string(static_cast<int64_t>(vote_for_)));  // @unsafe
  log_storage_->sync();  // @unsafe
}

// @unsafe - Uses LogStorage API
void RaftServer::PersistCommitIndexToLogStorage() {
  if (!log_storage_ || !log_storage_->is_open()) {
    return;
  }

  log_storage_->set_metadata(META_COMMIT_INDEX, std::to_string(commitIndex));  // @unsafe
  // Note: Don't sync for commitIndex - it can be recovered from logs
}

// @unsafe - Uses LogStorage API
void RaftServer::PersistLogEntryToLogStorage(slotid_t slot_id, const RaftData& data) {
  if (!log_storage_ || !log_storage_->is_open()) {
    return;
  }

  rrr::LogEntry entry(slot_id, data.term);
  entry.command = data.log_;
  entry.max_ballot_seen = data.max_ballot_seen_;
  entry.max_ballot_accepted = data.max_ballot_accepted_;
  entry.committed = (slot_id <= commitIndex);

  log_storage_->put(entry);  // @unsafe
  log_storage_->sync();  // @unsafe
}

// @unsafe - Uses LogStorage API
void RaftServer::PersistLogEntriesToLogStorage(const std::vector<std::pair<slotid_t, std::shared_ptr<RaftData>>>& entries) {
  if (!log_storage_ || !log_storage_->is_open() || entries.empty()) {
    return;
  }

  std::vector<rrr::LogEntry> log_entries;
  log_entries.reserve(entries.size());

  for (const auto& [slot_id, data] : entries) {
    rrr::LogEntry entry(slot_id, data->term);
    entry.command = data->log_;
    entry.max_ballot_seen = data->max_ballot_seen_;
    entry.max_ballot_accepted = data->max_ballot_accepted_;
    entry.committed = (slot_id <= commitIndex);
    log_entries.push_back(entry);
  }

  log_storage_->put_batch(log_entries);  // @unsafe
  log_storage_->sync();  // @unsafe
}

// @unsafe - Uses LogStorage API
bool RaftServer::RecoverFromStorage() {
  if (!log_storage_ || !log_storage_->is_open()) {
    return true;  // No storage configured, nothing to recover
  }

  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // Recover currentTerm
  auto term_opt = log_storage_->get_metadata(META_TERM);  // @unsafe
  if (term_opt.is_some()) {
    currentTerm = std::stoull(term_opt.unwrap());
  }

  // Recover vote_for
  auto vote_opt = log_storage_->get_metadata(META_VOTE_FOR);  // @unsafe
  if (vote_opt.is_some()) {
    int64_t vote_val = std::stoll(vote_opt.unwrap());
    vote_for_ = static_cast<siteid_t>(vote_val);
  }

  // Recover commitIndex
  auto commit_opt = log_storage_->get_metadata(META_COMMIT_INDEX);  // @unsafe
  if (commit_opt.is_some()) {
    commitIndex = std::stoull(commit_opt.unwrap());
  }

  // Recover lastLogIndex
  lastLogIndex = log_storage_->get_last_index();  // @unsafe

  // Recover log entries
  if (lastLogIndex > 0) {
    auto entries = log_storage_->get_range(1, lastLogIndex + 1);  // @unsafe
    for (const auto& entry : entries) {
      auto data = std::make_shared<RaftData>();
      data->term = entry.term;
      data->log_ = entry.command;
      data->max_ballot_seen_ = entry.max_ballot_seen;
      data->max_ballot_accepted_ = entry.max_ballot_accepted;
      raft_logs_[entry.slot_id] = data;
    }
  }

  Log_info("[RAFT-RECOVERY] Site %d: Recovered term=%lu vote_for=%d lastLogIndex=%lu commitIndex=%lu entries=%zu",
           site_id_, currentTerm, vote_for_, lastLogIndex, commitIndex, raft_logs_.size());

  return true;
}

// @unsafe - Calls app_next_ callback
void RaftServer::ReplayCommittedEntries() {
  if (!app_next_) {
    Log_warn("[RAFT-REPLAY] Site %d: No app_next_ callback, skipping replay", site_id_);
    return;
  }

  std::lock_guard<std::recursive_mutex> lock(mtx_);

  slotid_t start = executeIndex + 1;
  slotid_t end = commitIndex;

  if (start > end) {
    Log_info("[RAFT-REPLAY] Site %d: No entries to replay (executeIndex=%lu >= commitIndex=%lu)",
             site_id_, executeIndex, commitIndex);
    return;
  }

  Log_info("[RAFT-REPLAY] Site %d: Replaying entries %lu..%lu", site_id_, start, end);

  size_t replayed = 0;
  for (slotid_t id = start; id <= end; id++) {
    auto instance = GetRaftInstance(id);
    if (instance && instance->log_) {
      app_next_(id, instance->log_);
      executeIndex = id;
      replayed++;
    } else {
      Log_warn("[RAFT-REPLAY] Site %d: Missing log entry at slot %lu, stopping replay", site_id_, id);
      break;
    }
  }

  Log_info("[RAFT-REPLAY] Site %d: Replayed %zu entries, executeIndex now %lu",
           site_id_, replayed, executeIndex);

  // Phase 2.3: Log uncommitted entries status
  size_t uncommitted = GetUncommittedCount();
  if (uncommitted > 0) {
    Log_info("[RAFT-RECOVERY] Site %d: %zu uncommitted entries (lastLogIndex=%lu, commitIndex=%lu) - will be resolved by consensus",
             site_id_, uncommitted, lastLogIndex, commitIndex);
  }
}

// @safe - Read-only accessor
size_t RaftServer::GetUncommittedCount() const {
  if (lastLogIndex > commitIndex) {
    return lastLogIndex - commitIndex;
  }
  return 0;
}

// @unsafe - Modifies log storage
size_t RaftServer::CompactLog(slotid_t up_to_index) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  if (!log_storage_) {
    Log_debug("[RAFT-COMPACT] Site %d: No log storage, skipping compaction", site_id_);
    return 0;
  }

  // Safety check: don't compact beyond commit index
  if (up_to_index > commitIndex) {
    Log_warn("[RAFT-COMPACT] Site %d: Cannot compact beyond commitIndex (%lu > %lu)",
             site_id_, up_to_index, commitIndex);
    up_to_index = commitIndex;
  }

  // Get current first slot
  slotid_t first_slot = log_storage_->get_first_index();
  if (first_slot == 0 || log_storage_->empty()) {
    Log_debug("[RAFT-COMPACT] Site %d: Log is empty, nothing to compact", site_id_);
    return 0;
  }

  // Nothing to compact if up_to_index is before first slot
  if (up_to_index < first_slot) {
    Log_debug("[RAFT-COMPACT] Site %d: up_to_index %lu < first_slot %lu, nothing to compact",
              site_id_, up_to_index, first_slot);
    return 0;
  }

  // Remove entries from storage
  size_t to_remove = up_to_index - first_slot + 1;
  if (log_storage_->remove_range(first_slot, up_to_index + 1)) {
    Log_info("[RAFT-COMPACT] Site %d: Compacted %zu entries [%lu..%lu]",
             site_id_, to_remove, first_slot, up_to_index);

    // Also remove from in-memory logs
    for (slotid_t id = first_slot; id <= up_to_index; ++id) {
      raft_logs_.erase(id);
    }

    // Update min_active_slot_
    if (up_to_index + 1 > min_active_slot_) {
      min_active_slot_ = up_to_index + 1;
    }

    return to_remove;
  } else {
    Log_error("[RAFT-COMPACT] Site %d: Failed to compact log entries", site_id_);
    return 0;
  }
}

// ============================================================================

// @safe
void RaftServer::LogTermChange(const char* reason,
                               uint64_t old_term,
                               uint64_t new_term,
                               siteid_t source) {
  if (old_term == new_term) {
    return;
  }
  const char* why = reason ? reason : "unspecified";
  if (source != INVALID_SITEID) {
    Log_info("[RAFT-TERM] server %d term %lu -> %lu (%s, source_site=%d)",
             site_id_, old_term, new_term, why, source);
  } else {
    Log_info("[RAFT-TERM] server %d term %lu -> %lu (%s)",
             site_id_, old_term, new_term, why);
  }
}

namespace {

// @safe
bool JetpackRecoveryEnabled() {
  static const bool enabled = []() {
    const char* flag = std::getenv("MAKO_DISABLE_JETPACK");
    if (!flag || flag[0] == '\0') {
      return true;
    }
    std::string value(flag);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });

    auto is_true = [](const std::string& v) {
      return v == "1" || v == "true" || v == "yes" || v == "on";
    };
    auto is_false = [](const std::string& v) {
      return v == "0" || v == "false" || v == "no" || v == "off";
    };

    if (is_true(value)) {
      Log_info("[JETPACK-RUNTIME] MAKO_DISABLE_JETPACK=%s -> Jetpack recovery disabled", flag);
      return false;
    }
    if (is_false(value)) {
      return true;
    }

    Log_info("[JETPACK-RUNTIME] MAKO_DISABLE_JETPACK has unrecognised value '%s'; defaulting to disabled", flag);
    return false;
  }();
  return enabled;
}

}  // namespace

// @safe - Uses rusty::Box for timer ownership
RaftServer::RaftServer(Frame * frame)
  : timer_(rusty::Box<Timer>::make(Timer()))  // Initialize Box in member initializer list
{
  frame_ = frame ;
#ifdef RAFT_TEST_CORO
  setIsLeader(false);
#endif
  stop_ = false ;
}

// @safe
void RaftServer::OnJetpackPullCmd(const epoch_t& jepoch,
                                   const epoch_t& oepoch,
                                   const std::vector<key_t>& keys,
                                   bool_t* ok,
                                   epoch_t* reply_jepoch,
                                   epoch_t* reply_oepoch,
                                   MarshallDeputy* reply_old_view,
                                   MarshallDeputy* reply_new_view,
                                   shared_ptr<KeyCmdBatchData>& batch) {
  TxLogServer::OnJetpackPullCmd(jepoch, oepoch, keys, ok, reply_jepoch, reply_oepoch,
                                reply_old_view, reply_new_view, batch);
  if (!IsLeader()) {
    resetTimer("JetpackPullCmd RPC");
#ifdef RAFT_LEADER_ELECTION_DEBUG
    // Log_info("[RAFT_TIMER] server %d reset election timer due to JetpackPullCmd (keys=%zu)",
    //          site_id_, keys.size());
#endif
  }
}

// @safe
uint64_t RaftServer::GetElectionTimeout() {
  uint64_t base_timeout;
  uint64_t current_time = Time::now();
  bool in_grace_period = (current_time - startup_timestamp_) < 5000000; // 5 seconds in microseconds

  if (AmIPreferredLeader()) {
    // Preferred replica: Short timeout (150-300ms) to win elections quickly
    base_timeout = 150000; // 150ms
    uint64_t jitter = RandomGenerator::rand(0, 150000);
    return base_timeout + jitter; // 150-300ms
  } else if (in_grace_period) {
    // Non-preferred during grace period: Long timeout (1-2s) to allow preferred to win
    base_timeout = 1000000; // 1s
    uint64_t jitter = RandomGenerator::rand(0, 1000000);
    return base_timeout + jitter; // 1-2s
  } else {
    // Non-preferred after grace: Medium timeout (500ms-1s) to enable failover
    base_timeout = 500000; // 500ms
    uint64_t jitter = RandomGenerator::rand(0, 500000);
    return base_timeout + jitter; // 500ms-1s
  }
}

// @safe
void RaftServer::Setup() {
  // Record startup time for grace period logic
  startup_timestamp_ = Time::now();

  // ========== INITIALIZE PERSISTENCE (LogStorage + RecoveryManager) ==========
  const char* persistence_flag = std::getenv("MAKO_RAFT_PERSISTENCE");
  bool should_enable = (persistence_flag &&
                       (strcmp(persistence_flag, "1") == 0 ||
                        strcmp(persistence_flag, "true") == 0));

  if (should_enable) {
    // Check if async persistence is requested (default: sync)
    const char* async_flag = std::getenv("MAKO_RAFT_ASYNC_PERSISTENCE");
    async_persistence_ = (async_flag &&
                         (strcmp(async_flag, "1") == 0 ||
                          strcmp(async_flag, "true") == 0));

    Log_info("[RAFT-PERSISTENCE] Initializing LogStorage for site %d partition %d (mode=%s)",
             site_id_, partition_id_, async_persistence_ ? "async" : "sync");

    // Create RecoveryConfig
    rrr::RecoveryConfig config;
    std::string base_path = "/tmp";
    const char* custom_path = std::getenv("MAKO_RAFT_PERSISTENCE_PATH");
    if (custom_path && custom_path[0] != '\0') {
      base_path = custom_path;
    }
    config.storage_path = base_path + "/raft_" + std::to_string(site_id_) +
                         "_partition_" + std::to_string(partition_id_);

    // Create RecoveryManager and storage
    rrr::RecoveryManager manager(config);
    auto storage = manager.create_storage();

    if (!storage) {
      Log_error("[RAFT-PERSISTENCE] Failed to create LogStorage - continuing without persistence");
    } else {
      // Use RecoveryManager to orchestrate recovery
      auto result = manager.recover(
        [this](std::shared_ptr<rrr::LogStorage> s) { SetLogStorage(s); },
        [this]() { return RecoverFromStorage(); },
        [this](rrr::RecoveryResult& r) {
          r.recovered_term = currentTerm;
          r.recovered_entries = raft_logs_.size();
        }
      );

      if (result.success) {
        Log_info("[RAFT-PERSISTENCE] Recovery complete: mode=%d term=%lu entries=%lu time=%lums",
                 static_cast<int>(result.mode), result.recovered_term,
                 result.recovered_entries, result.recovery_time_ms);
      } else {
        Log_error("[RAFT-PERSISTENCE] Recovery failed: %s", result.error_message.c_str());
      }
    }
  } else {
    Log_info("[RAFT-PERSISTENCE] Disabled (set MAKO_RAFT_PERSISTENCE=1 to enable)");
  }

#ifdef RAFT_TEST_CORO
  if (heartbeat_) {
		Log_debug("starting heartbeat loop at site %d", site_id_);
    Fiber::create_run([this](){
      this->HeartbeatLoop(); 
    });
    // Start election timeout loop
    if (failover_) {
      Fiber::create_run([this](){
        StartElectionTimer(); 
      });
    }
	}
#endif

#ifndef RAFT_TEST_CORO
  if (heartbeat_) {
		Log_debug("starting heartbeat loop at site %d", site_id_);
    Fiber::create_run([this](){
      this->HeartbeatLoop(); 
    });
    // Start election timeout loop
    if (failover_) {
      Fiber::create_run([this](){
        StartElectionTimer(); 
      });
    }
	}
#endif
  // Election timer will be started in Start() method when first command is submitted
}

// @safe
void RaftServer::Disconnect(const bool disconnect) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  verify(disconnected_ != disconnect);
  // global map of rpc_par_proxies_ values accessed by partition then by site
  static map<parid_t, map<siteid_t, map<siteid_t, vector<SiteProxyPair>>>> _proxies{};
  if (_proxies.find(partition_id_) == _proxies.end()) {
    _proxies[partition_id_] = {};
  }
  RaftCommo *c = (RaftCommo*) commo();
  if (disconnect) {
    // Clear any stale proxy data from previous Kill/Restart cycle
    // This can happen when a server is killed, restarted (with new proxies),
    // and then killed again - the old _proxies data was never cleared
    if (_proxies[partition_id_][loc_id_].size() > 0) {
      Log_info("[DISCONNECT] Clearing stale proxy data for partition %d, site %d (had %zu entries)",
               partition_id_, loc_id_, _proxies[partition_id_][loc_id_].size());
      _proxies[partition_id_][loc_id_].clear();
    }
    verify(c->rpc_par_proxies_.size() > 0);
    auto sz = c->rpc_par_proxies_.size();
    _proxies[partition_id_][loc_id_].insert(c->rpc_par_proxies_.begin(), c->rpc_par_proxies_.end());
    c->rpc_par_proxies_ = {};
    verify(_proxies[partition_id_][loc_id_].size() == sz);
    verify(c->rpc_par_proxies_.size() == 0);
  } else {
    verify(_proxies[partition_id_][loc_id_].size() > 0);
    auto sz = _proxies[partition_id_][loc_id_].size();
    c->rpc_par_proxies_ = {};
    c->rpc_par_proxies_.insert(_proxies[partition_id_][loc_id_].begin(), _proxies[partition_id_][loc_id_].end());
    _proxies[partition_id_][loc_id_] = {};
    verify(_proxies[partition_id_][loc_id_].size() == 0);
    verify(c->rpc_par_proxies_.size() == sz);
  }
  disconnected_ = disconnect;
}

// @safe
bool RaftServer::IsDisconnected() {
  return disconnected_;
}

// @safe
void RaftServer::setIsLeader(bool isLeader) {
  bool prev_is_leader = is_leader_;
#ifdef RAFT_LEADER_ELECTION_DEBUG
  Log_info("[RAFT_STATE] setIsLeader invoked site %d (loc %d) term %lu: prev_is_leader=%d new_is_leader=%d",
           site_id_, frame_->site_info_->locale_id, currentTerm, prev_is_leader, isLeader);
#endif


  if (isLeader) {  // [Jetpack] This need to be done before new leader realized it is a leader, otherwise new leader will use incorrect next_index_ balabala
    // Add null check for communicator
    if (commo_ == nullptr) {
      Log_info("commo_ is null, skipping leader initialization");
    } else {
      // Reset leader volatile state
      RaftCommo *c = (RaftCommo*) commo();
      auto proxies = c->rpc_par_proxies_[partition_id_];
      if(failover_) {
        for (auto& p : proxies) {
          if (p.first != site_id_) {
            // set matchIndex = 0
            match_index_[p.first] = 0;
            // set nextIndex = lastLogIndex + 1
            next_index_[p.first] = lastLogIndex + 1;
            Log_debug("loc_id_=%d match_index_[%d]=%d, next_index_[%d]=%d", loc_id_, p.first, match_index_[p.first], p.first, next_index_[p.first]);
          }
        }
        // matchedIndex and nextIndex should have indices for all servers except self
        verify(match_index_.size() == Config::GetConfig()->GetPartitionSize(partition_id_) - 1);
        verify(next_index_.size() == Config::GetConfig()->GetPartitionSize(partition_id_) - 1);
      }
    }
  }


  // This 2 lines MUST put BEFORE is_leader_ = isLeader ! otherwise they will become 0, and new view will without leader
  bool become_new_leader = isLeader && (!is_leader_);
  bool become_new_follower = (!isLeader) && is_leader_;

  // Update the leader state after view handling
  is_leader_ = isLeader;

  // Only log on actual transitions, not no-op calls
  if (become_new_leader || become_new_follower) {
    Log_info("RaftServer::setIsLeader site_id_ %d become_new_leader %d become_new_follower %d isLeader %d", site_id_, become_new_leader, become_new_follower, isLeader);
  }

  // Only update view when transitioning from non-leader to leader
  if (become_new_leader) {
    Log_info("[RAFT_STATE] setIsLeader transition LEADER: site %d term %lu prev_is_leader=%d become_new_leader=%d",
             site_id_, currentTerm, prev_is_leader, become_new_leader);

    // ============================================================================
    // LEADERSHIP TRANSFER: Clear transfer flags when becoming leader
    // ============================================================================
    // If we just became leader, any previous transfer is now complete
    transferring_leadership_ = false;

    // Only update view if we have enough information (not during initialization)
    if (partition_id_ != 0xFFFFFFFF && site_id_ != -1 && frame_ != nullptr) {
      // Move current new_view to old_view before updating
      old_view_ = new_view_;
      
      // Update new_view with this server as the leader
      int n_replicas = Config::GetConfig()->GetPartitionSize(partition_id_);
      new_view_ = View(n_replicas, site_id_, currentTerm);
      Log_info("[RAFT_VIEW] Server %d became leader for partition %d, term=%lu, old_view=%s, new_view=%s", 
               site_id_, partition_id_, currentTerm, 
               old_view_.ToString().c_str(), new_view_.ToString().c_str());
      
      // IMPORTANT: Update the communicator's view so it knows this server is the leader
      if (commo_) {
        auto view_data = std::make_shared<ViewData>(new_view_, partition_id_);
        commo()->UpdatePartitionView(partition_id_, view_data);
        Log_info("[RAFT_VIEW] Updated communicator view for partition %d with new leader %d", 
                 partition_id_, site_id_);
      }
      
#ifndef RAFT_TEST_CORO
      if (JetpackRecoveryEnabled()) {
        JetpackRecoveryEntry();
      }
#endif
    }

    // ============================================================================
    // LEADERSHIP TRANSFER: Start monitoring if non-preferred leader
    // ============================================================================
    // If we just became a non-preferred leader, start monitoring for transfer
    // opportunity. This ensures that after failover/elections, non-preferred
    // leaders will transfer back to preferred leaders when they catch up.
    if (!AmIPreferredLeader() && looping_) {
      Log_info("[LEADERSHIP-TRANSFER] Site %d: Became non-preferred leader, starting transfer monitoring",
               site_id_);
      StartLeadershipTransferMonitoring();
    }
  } else if (become_new_follower) {
    Log_info("[RAFT_STATE] setIsLeader transition FOLLOWER: site %d term %lu prev_is_leader=%d become_new_follower=%d",
             site_id_, currentTerm, prev_is_leader, become_new_follower);

    // ============================================================================
    // CRITICAL FIX: Reset election timer when becoming follower
    // ============================================================================
    // This prevents instant elections after recovery/resume. When a node resumes
    // from SIGSTOP/pause, last_heartbeat_time_ is stale (from before pause).
    // Resetting it here ensures the election timer counts from NOW, giving the
    // current leader time to send heartbeats before this node starts an election.
    // This is standard Raft behavior: followers reset their timer when stepping down.
    resetTimer("became follower");
    Log_info("[RAFT_TIMER] Site %d reset election timer when becoming follower (last_hb now=%lu)",
             site_id_, last_heartbeat_time_);

    // When transitioning from leader to non-leader
    Log_info("[RAFT_VIEW] Server %d stepping down as leader for partition %d", site_id_, partition_id_);

    // ============================================================================
    // LEADERSHIP TRANSFER: Stop monitoring when stepping down
    // ============================================================================
    // Stop leadership transfer monitoring if we were running it
    StopLeadershipTransferMonitoring();

    // View will be updated when we learn about the new leader
  }

  // CRITICAL: Fire leadership change callback so RaftWorker can update its state
  // This allows clients to retarget to the new leader after elections
  if (leader_change_cb_) {
    if (become_new_leader) {
      Log_info("[LEADER_CALLBACK] Site %d: Firing leader_change_cb_(true) - became leader", site_id_);
      leader_change_cb_(true);
    } else if (become_new_follower) {
      Log_info("[LEADER_CALLBACK] Site %d: Firing leader_change_cb_(false) - became follower", site_id_);
      leader_change_cb_(false);
    }
  }
}

// @safe
void RaftServer::applyLogs() {
  // Log commit state for debugging
  Log_info("[APPLY-LOGS] site=%d commitIndex=%ld executeIndex=%ld",
           site_id_, commitIndex, executeIndex);

  // Only mark pending if there's actually new work to apply
  if (executeIndex < commitIndex) {
    apply_pending_.store(true, std::memory_order_release);
  }

  // If already applying, return - the current apply loop will pick up our work
  if (in_applying_logs_) {
    return;
  }

  in_applying_logs_ = true;

  // Keep applying logs until no more pending work arrives
  // This ensures we never drop work even under heavy load
  do {
    // Clear the pending flag before processing
    apply_pending_.store(false, std::memory_order_release);

    // Apply all committed logs
    for (slotid_t id = executeIndex + 1; id <= commitIndex; id++) {
      auto next_instance = GetRaftInstance(id);
      if (next_instance && next_instance->log_) {
        RuleWitnessGC(next_instance->log_);
        Log_info("[APPLY-LOGS] site=%d applying index=%ld", site_id_, id);
        app_next_(id, next_instance->log_);  // Pass both id and log (signature requires 2 args)
        executeIndex = id;
      } else {
        Log_info("[APPLY-LOGS] site=%d SKIP index=%ld (no instance or log)", site_id_, id);
        break;
      }
    }

    // Check if new work arrived while we were applying
    // If so, loop again to process it
  } while (apply_pending_.load(std::memory_order_acquire));

  in_applying_logs_ = false;

  // Cleanup old commands to prevent memory buildup
  int i = min_active_slot_;
  while (i + 60000 < executeIndex) {
    removeCmd(i);
    i++;
  }
  min_active_slot_ = i;
}

// @unsafe
// TODO: Revisit borrow checker errors in this function.
// The checker reports "use after move" for loop-local variables (matchedIndices,
// batch_buffer_, batch_cmd, cmd) due to 2-iteration loop simulation. These variables
// are declared fresh each iteration, but the checker may not be resetting state
// correctly for loop-local declarations. Additionally, SendAppendEntries2 takes
// shared_ptr<Marshallable> by value (moves), which compounds the issue.
// Potential fixes: (1) Change SendAppendEntries2 to take const shared_ptr&,
// (2) Investigate checker's loop-local variable handling.
// ============================================================================
// PARALLEL HEARTBEAT FIX
// ============================================================================
// This struct holds context for each pending AppendEntries RPC.
// Used to send RPCs in parallel and process responses without blocking.
// The response field uses shared_ptr to ensure memory validity when callback fires.
struct PendingAppendEntries {
  siteid_t follower_id;
  shared_ptr<AppendEntriesResponse> response;  // shared_ptr ensures callback memory safety
  shared_ptr<Marshallable> cmd;  // nullptr for heartbeat
  uint64_t sent_term;  // term when RPC was sent
};

void RaftServer::HeartbeatLoop() {
  auto hb_timer = new Timer();
  hb_timer->start();

  parid_t partition_id = partition_id_;
  // Log_info("!!!!!!! if (!failover_)");
  // if (!failover_) {
    auto proxies = commo()->rpc_par_proxies_[partition_id];
    for (auto& p : proxies) {
      if (p.first == site_id_) {
        continue;  // skip self
      }
      // set matchIndex = 0
      match_index_[p.first] = 0;
      // set nextIndex = 1
      next_index_[p.first] = 1;
    }
    // matchedIndex and nextIndex should have indices for all servers except self
    verify(match_index_.size() == Config::GetConfig()->GetPartitionSize(partition_id) - 1);
    verify(next_index_.size() == Config::GetConfig()->GetPartitionSize(partition_id) - 1);
  // }

  Log_debug("heartbeat loop init from site: %d", site_id_);
  looping_ = true;
  while(looping_) {
    uint64_t term;
    {
      {
        // std::lock_guard<std::recursive_mutex> lock(ready_for_replication_mtx_);
        // if (ready_for_replication_ == nullptr)
          ready_for_replication_ = Reactor::create_sp_event<IntEvent>();
        ready_for_replication_->set(0);
      }
      ready_for_replication_->wait(HEARTBEAT_INTERVAL);
      {
        // std::lock_guard<std::recursive_mutex> lock(ready_for_replication_mtx_);
        ready_for_replication_ = nullptr;
      }
      // Fiber::sleep(HEARTBEAT_INTERVAL);
      // Log_info("heartbeat loop at loc %d", loc_id_);
      if (!IsLeader()) {
        // Log_info("heartbeat loop at loc %d skip since not leader", loc_id_);
        continue;
      }

      auto nservers = Config::GetConfig()->GetPartitionSize(partition_id);

      // ========================================================================
      // PHASE 0: Calculate commit index ONCE per heartbeat round (not per-follower)
      // ========================================================================
      mtx_.lock();
      std::vector<uint64_t> matchedIndices{};
      for (auto it = match_index_.begin(); it != match_index_.end(); it++) {
        matchedIndices.push_back(it->second);
        Log_info("[COMMIT-CALC] match_index_[%d] = %lu", it->first, it->second);
      }
      Log_info("[COMMIT-CALC] nservers=%lu, matchedIndices.size()=%zu", nservers, matchedIndices.size());
      verify(matchedIndices.size() == nservers - 1);
      std::sort(matchedIndices.begin(), matchedIndices.end());
      uint64_t newCommitIndex = matchedIndices[(nservers - 1) / 2];
      Log_info("[COMMIT-CALC] newCommitIndex=%lu (median at index %lu), currentCommitIndex=%lu", newCommitIndex, (nservers - 1) / 2, commitIndex);

      if (newCommitIndex > lastLogIndex) {
        newCommitIndex = lastLogIndex;
      }

      if (newCommitIndex > commitIndex && (GetRaftInstance(newCommitIndex)->term == currentTerm)) {
        Log_debug("newCommitIndex %d", newCommitIndex);
        commitIndex = newCommitIndex;
        PersistCommitIndex(commitIndex, "HeartbeatLoop: leader commit");
      }
      if (commitIndex > executeIndex)
        applyLogs();
      term = currentTerm;
      uint64_t current_commit_index = commitIndex;
      uint64_t current_last_log_index = lastLogIndex;
      mtx_.unlock();

      // ========================================================================
      // PHASE 1: Send all AppendEntries RPCs in PARALLEL (non-blocking)
      // ========================================================================
      // Use unique_ptr to ensure stable memory addresses for the callback pointers.
      // The async RPC callback writes to ret_status/ret_term/ret_last_log_index,
      // so these must remain at fixed addresses until the RPC completes.
      std::vector<std::unique_ptr<PendingAppendEntries>> pending_rpcs;

      for (auto it = next_index_.begin(); it != next_index_.end(); it++) {
        auto site_id = it->first;
        if (site_id == site_id_) {
          continue;
        }
        if (!IsLeader()) {
          break;  // Stop sending if we lost leadership
        }

        mtx_.lock();
        uint64_t prevLogIndex = it->second - 1;
        if (prevLogIndex > lastLogIndex) {
          Log_info("[APPEND_ENTRIES] ERROR: prevLogIndex (%ld) > lastLogIndex (%ld), fixing next_index", prevLogIndex, lastLogIndex);
          it->second = lastLogIndex + 1;
          prevLogIndex = it->second - 1;
        }

        if (prevLogIndex > lastLogIndex) {
          Log_info("[APPEND_ENTRIES] WARNING: Cannot send AppendEntries to follower %d: prevLogIndex (%ld) > lastLogIndex (%ld), skipping",
                   site_id, prevLogIndex, lastLogIndex);
          it->second = 1;
          mtx_.unlock();
          continue;
        }

        verify(prevLogIndex <= lastLogIndex);
        auto instance = GetRaftInstance(prevLogIndex);

        if (!instance) {
          Log_error("[HEARTBEAT-SEND] [CRITICAL] GetRaftInstance(%lu) returned NULL! Skipping follower %d",
                    prevLogIndex, site_id);
          mtx_.unlock();
          continue;
        }

        uint64_t prevLogTerm = instance->term;
        shared_ptr<Marshallable> cmd = nullptr;
        uint64_t cmdLogTerm = 0;

#ifndef RAFT_BATCH_OPTIMIZATION
        Log_info("[BATCH_CHECK] site=%d follower=%d next_index=%lu min_active_slot_=%lu lastLogIndex=%lu",
                 site_id_, site_id, it->second, min_active_slot_, lastLogIndex);
        if (it->second <= lastLogIndex) {
          auto curInstance = GetRaftInstance(it->second);
          // @safe - Null check to prevent crash if instance doesn't exist
          if (!curInstance) {
            Log_error("[HEARTBEAT-SEND] GetRaftInstance(%lu) returned NULL, skipping", it->second);
          } else {
            cmd = curInstance->log_;
            cmdLogTerm = curInstance->term;
            Log_info("[APPEND_SEND] site=%d sending entry %lu to follower %d cmd=%p",
                site_id_, it->second, site_id, cmd.get());
          }
        }
#endif

#ifdef RAFT_BATCH_OPTIMIZATION
        vector<shared_ptr<TpcCommitCommand> > batch_buffer_;
        Log_info("[BATCH_CHECK] site=%d follower=%d next_index=%lu min_active_slot_=%lu lastLogIndex=%lu",
                 site_id_, site_id, it->second, min_active_slot_, lastLogIndex);
        for (int idx = std::max(it->second, min_active_slot_); idx <= lastLogIndex; idx++) {
          auto curInstance = GetRaftInstance(idx);
          // @safe - Null check to prevent crash if instance doesn't exist
          if (!curInstance) {
            Log_error("[HEARTBEAT-BATCH] GetRaftInstance(%d) returned NULL, skipping", idx);
            continue;
          }
          shared_ptr<TpcCommitCommand> curCmd = dynamic_pointer_cast<TpcCommitCommand>(curInstance->log_);
          // @safe - Null check for dynamic_pointer_cast result
          if (!curCmd) {
            Log_info("[BATCH_SKIP] site=%d idx=%d: log entry is not TpcCommitCommand (kind=%d), using raw log",
                     site_id_, idx, curInstance->log_ ? curInstance->log_->kind_ : -1);
            cmd = curInstance->log_;
            break;
          }
          curCmd->term = curInstance->term;
          batch_buffer_.push_back(curCmd);
        }
        if (batch_buffer_.size() > 0) {
          shared_ptr<TpcBatchCommand> batch_cmd = std::make_shared<TpcBatchCommand>();
          batch_cmd->AddCmds(batch_buffer_);
          cmd = dynamic_pointer_cast<Marshallable>(batch_cmd);
          Log_info("[BATCH_SEND] site=%d sending batch of %zu entries to follower %d",
                   site_id_, batch_buffer_.size(), site_id);
        }
#endif
        mtx_.unlock();

        // Create pending RPC context
        auto pending = std::make_unique<PendingAppendEntries>();
        pending->follower_id = site_id;
        pending->cmd = cmd;
        pending->sent_term = term;

        // Send RPC (non-blocking - just initiates the async call)
        // Response is allocated with shared_ptr - callback captures it to ensure memory validity
        pending->response = commo()->SendAppendEntries2(site_id,
                                              partition_id,
                                              -1,
                                              -1,
                                              IsLeader(),
                                              site_id_,
                                              term,
                                              prevLogIndex,
                                              prevLogTerm,
                                              current_commit_index,
                                              cmd,
                                              cmdLogTerm);

        pending_rpcs.push_back(std::move(pending));
      }

      // ========================================================================
      // PHASE 2: Wait for responses with SHORT timeout and process them
      // ========================================================================
      // Use a shorter per-RPC timeout (100ms) since we're processing in parallel.
      // Total round time is bounded by the slowest responder, not sum of all.
      const uint64_t PER_RPC_TIMEOUT = 100000;  // 100ms per RPC

      for (auto& pending_ptr : pending_rpcs) {
        if (!IsLeader()) {
          break;  // Stop processing if we lost leadership
        }

        auto& pending = *pending_ptr;  // Dereference unique_ptr for cleaner access
        auto& resp = *pending.response;  // Access response data

        resp.event->wait(PER_RPC_TIMEOUT);

        if (resp.event->status_.get() == Event::TIMEOUT) {
          Log_debug("[PARALLEL-HB] Timeout waiting for follower %d", pending.follower_id);
          continue;  // Skip this follower, try again next round
        }

        mtx_.lock();
        auto& next_index = next_index_[pending.follower_id];
        auto& match_index = match_index_[pending.follower_id];

        if (resp.status == false && resp.term == 0 && resp.last_log_index == 0) {
          // RPC failed or no response - do nothing
        } else if (currentTerm > pending.sent_term) {
          // Stale response from old term - ignore
        } else if (resp.status == 0 && resp.term > pending.sent_term) {
          // case 1: AppendEntries rejected because leader's term is expired
          if (currentTerm == pending.sent_term) {
            Log_info("[STEPDOWN] Site %d: Stepping down due to higher term from follower %d (my_term=%lu, follower_term=%lu)",
                     site_id_, pending.follower_id, pending.sent_term, resp.term);
            currentTerm = resp.term;
            stepDown(StepDownReason::HigherTerm);
            mtx_.unlock();
            break;  // Stop processing - we're no longer leader
          }
        } else if (resp.status == 0) {
          // case 2: AppendEntries rejected - log inconsistency
          if (resp.last_log_index > 0 && resp.last_log_index < next_index - 1) {
            uint64_t old_next = next_index;
            next_index = resp.last_log_index + 1;
            Log_info("[LOG-RECONCILE] Site %d: Fast backoff for follower %d: next_index %lu -> %lu (gap: %lu, follower reported last: %lu)",
                     site_id_, pending.follower_id, old_next, next_index, old_next - next_index, resp.last_log_index);
          } else if (next_index > 10) {
            uint64_t old_next = next_index;
            next_index = next_index / 2;
            Log_info("[LOG-RECONCILE] Site %d: Exponential backoff for follower %d: next_index %lu -> %lu (halved)",
                     site_id_, pending.follower_id, old_next, next_index);
          } else if (next_index > 1) {
            next_index--;
            Log_debug("[LOG-RECONCILE] Site %d: Linear backoff for follower %d: next_index %lu -> %lu",
                      site_id_, pending.follower_id, next_index + 1, next_index);
          } else {
            next_index = 1;
          }
        } else {
          // case 3: AppendEntries accepted
          verify(resp.status == true);

          // ==================================================================
          // SPECULATIVE REPLICATION: Track memory acks
          // ack_type=0 means Memory ack (immediate response before fsync)
          // ack_type=1 means Durable ack (handled via AppendEntriesDurable RPC)
          // ==================================================================
          if (resp.ack_type == 0) {  // Memory ack
            // Add follower to memoryAcks for all indices up to last_log_index
            for (uint64_t idx = 1; idx <= resp.last_log_index; ++idx) {
              memoryAcks_[idx].insert(pending.follower_id);
            }
            Log_debug("[SPEC-RAFT] Memory ack from follower %d for index %lu",
                      pending.follower_id, resp.last_log_index);
          }

          if (pending.cmd == nullptr) {
            Log_debug("case 3A: AppendEntries accepted for heartbeat msg");
            if (resp.last_log_index > match_index) {
              match_index = resp.last_log_index;
              if (match_index > lastLogIndex) {
                match_index = lastLogIndex;
              }
              Log_debug("heartbeat updated match_index for site %d: match_index=%lu", pending.follower_id, match_index);
            }
            if (resp.last_log_index >= next_index) {
              if (next_index <= lastLogIndex) {
                next_index++;
                Log_debug("empty heartbeat incrementing next_index for site: %d, next_index: %d", pending.follower_id, next_index);
              }
            }
          } else {
            Log_debug("case 3B: AppendEntries accepted for non-empty msg");
            if (resp.last_log_index < next_index) {
              next_index = resp.last_log_index + 1;
              match_index = resp.last_log_index;
              mtx_.unlock();
              continue;
            }
            Log_debug("loc %ld followerLastLogIndex=%ld followerNextIndex=%ld followerMatchedIndex=%ld",
                pending.follower_id, resp.last_log_index, next_index, match_index);
#ifndef RAFT_BATCH_OPTIMIZATION
            match_index = next_index;
            next_index++;
#endif
#ifdef RAFT_BATCH_OPTIMIZATION
            match_index = resp.last_log_index;
            next_index = resp.last_log_index + 1;
#endif
            if (match_index > lastLogIndex) {
              match_index = lastLogIndex;
            }
            Log_debug("leader site %d receiving site %ld followerLastLogIndex=%ld followerNextIndex=%ld followerMatchedIndex=%ld",
                site_id_, pending.follower_id, resp.last_log_index, next_index, match_index);
          }
        }
        mtx_.unlock();
      }

      // ========================================================================
      // PHASE 3: Recalculate commit index after all responses processed
      // ========================================================================
      // This ensures commits happen promptly after replication, matching the
      // original behavior where commit index was recalculated after each response.
      if (IsLeader()) {
        mtx_.lock();
        std::vector<uint64_t> finalMatchedIndices{};
        for (auto it = match_index_.begin(); it != match_index_.end(); it++) {
          finalMatchedIndices.push_back(it->second);
        }
        std::sort(finalMatchedIndices.begin(), finalMatchedIndices.end());
        uint64_t finalCommitIndex = finalMatchedIndices[(nservers - 1) / 2];
        if (finalCommitIndex > lastLogIndex) {
          finalCommitIndex = lastLogIndex;
        }
        if (finalCommitIndex > commitIndex && (GetRaftInstance(finalCommitIndex)->term == currentTerm)) {
          Log_debug("[PHASE3-COMMIT] Advancing commitIndex %lu -> %lu", commitIndex, finalCommitIndex);
          commitIndex = finalCommitIndex;
          PersistCommitIndex(commitIndex, "HeartbeatLoop: post-response commit");
        }
        if (commitIndex > executeIndex)
          applyLogs();

        // ==================================================================
        // SPECULATIVE REPLICATION: Update specCommitIndex based on memory acks
        // ==================================================================
        size_t quorum = (nservers / 2) + 1;

        // Find the highest index with memory ack quorum
        // Leader's own entry counts as a memory ack
        uint64_t newSpecCommitIndex = specCommitIndex_;
        for (uint64_t idx = specCommitIndex_ + 1; idx <= lastLogIndex; ++idx) {
          // Check if we have quorum for this index
          auto it = memoryAcks_.find(idx);
          size_t ack_count = (it != memoryAcks_.end()) ? it->second.size() : 0;
          // Leader's own log counts as an ack (we have the entry)
          ack_count += 1;  // +1 for leader's own entry

          if (ack_count >= quorum) {
            // Verify the entry is from current term
            auto instance = GetRaftInstance(idx);
            if (instance && instance->term == currentTerm) {
              newSpecCommitIndex = idx;
            }
          } else {
            // Stop at first index without quorum (monotonic advance)
            break;
          }
        }

        if (newSpecCommitIndex > specCommitIndex_) {
          uint64_t oldSpecCommitIndex = specCommitIndex_;
          Log_info("[SPEC-RAFT] Site %d: Advancing specCommitIndex %lu -> %lu",
                   site_id_, specCommitIndex_, newSpecCommitIndex);
          specCommitIndex_ = newSpecCommitIndex;

          // Phase 5.3: Notify clients with SPECULATIVE status for newly committed entries
          if (lastSpecNotifiedIndex_ < newSpecCommitIndex) {
            uint64_t notifyFrom = std::max(lastSpecNotifiedIndex_, oldSpecCommitIndex);
            NotifyCallbacks(notifyFrom, newSpecCommitIndex, CommitStatus::SPECULATIVE);
            lastSpecNotifiedIndex_ = newSpecCommitIndex;
          }
        }

        // Verify invariants
        VerifySpeculativeInvariants();

        mtx_.unlock();
      }
    }

    // ============================================================================
    // LEADERSHIP TRANSFER: Check if we should transfer to preferred replica
    // ============================================================================
    // ============================================================================
    // LEADERSHIP TRANSFER: Handled by Monitor Thread
    // ============================================================================
    // Leadership transfer is now handled by StartLeadershipTransferMonitoring() thread,
    // not here in HeartbeatLoop. This prevents race conditions and double-triggering.
    // The monitor thread is started in setIsLeader() when becoming a non-preferred leader.
    //
    // REMOVED: The piggybacked check that was here to avoid race with monitor thread.
	}
}

RaftServer::~RaftServer() {
  // CRITICAL: Set stop_ FIRST to signal all coroutines to stop
  // This must happen before vtable collapse to prevent race conditions
  stop_ = true;

  // Stop the HeartbeatLoop
  if (heartbeat_ && looping_) {
    Log_info("[SHUTDOWN] Stopping HeartbeatLoop for site=%d", site_id_);
    looping_ = false;

    // Wake up the HeartbeatLoop if it's sleeping so it can see looping_=false
    if (ready_for_replication_) {
      ready_for_replication_->set(1);
    }
  }

  // Stop leadership transfer monitoring thread if running
  StopLeadershipTransferMonitoring();

  // Join all async persistence threads to prevent use-after-free.
  // These threads capture `this` for PersistState/PersistLogEntry/commo() calls.
  // They must complete before this object is destroyed.
  // NOTE: We swap the vector out under the lock, then join WITHOUT the lock.
  // This prevents deadlock if an in-flight RPC handler tries to emplace_back
  // a new thread while we're joining (it would block on async_threads_mtx_).
  {
    std::vector<std::pair<std::thread, std::shared_ptr<std::atomic<bool>>>> threads_to_join;
    {
      std::lock_guard<std::mutex> lk(async_threads_mtx_);
      threads_to_join = std::move(async_threads_);
    }
    for (auto& [t, done_flag] : threads_to_join) {
      if (t.joinable()) {
        t.join();
      }
    }
  }

  // CRITICAL: Sleep briefly to allow detached coroutines to see stop_=true and exit
  // The election timer coroutine (StartElectionTimer) and leadership transfer coroutine
  // (InitiateLeadershipTransfer) are detached and check stop_ before calling RequestVote().
  // We need to give them time to notice stop_=true and return before the vtable collapses.
  // Without this sleep, there's a race where the coroutine wakes up, checks stop_=false,
  // then the destructor runs (vtable collapses), then the coroutine calls RequestVote()
  // through the base class vtable, hitting verify(0) and aborting.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  Log_info("site par %d, loc %d: prepare %d, accept %d, commit %d",
      partition_id_, loc_id_, n_prepare_, n_accept_, n_commit_);
}

bool RaftServer::RequestVote() {
  // FIX 2: Prevent RequestVote during shutdown
  // The election timer coroutine may fire after ~RaftServer destructor runs,
  // causing a call to the base class TxLogServer::RequestVote() which hits verify(0)
  // Check stop_ flag to avoid this crash during teardown
  if (stop_) {
    Log_debug("[RAFT-SHUTDOWN] RequestVote called during shutdown (site=%d), ignoring to prevent crash", site_id_);
    return false;
  }

  // for(int i = 0; i < 1000; i++) Log_info("not calling the wrong method");

  parid_t par_id = this->frame_->site_info_->partition_id_ ;
  parid_t loc_id = this->frame_->site_info_->locale_id ;

  uint32_t lstoff = 0  ;
  slotid_t lst_idx = 0 ;
  ballot_t lst_term = 0 ;
  ballot_t prev_term = 0;
  siteid_t prev_vote_for = INVALID_SITEID;

  {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    prev_term = currentTerm;
    prev_vote_for = vote_for_;
    auto prev_local_term = currentTerm;
    currentTerm++ ;
    vote_for_ = site_id_;  // Vote for ourselves when starting election

    // CRITICAL: Persist term and vote BEFORE sending RequestVote RPCs
    PersistState(currentTerm, vote_for_, "RequestVote: starting election");

    LogTermChange("starting election", prev_local_term, currentTerm);
    // PersistState() already called above - no need for duplicate persistence
    lstoff = lastLogIndex - snapidx_ ;
    if (lstoff == 0) {
      lst_idx = snapidx_;
      lst_term = snapterm_;
    } else {
      auto log = GetRaftInstance(lstoff) ; // causes min_active_slot_ verification error (server.h:247)
      lst_idx = lstoff + snapidx_ ;
      lst_term = log->term ;
    }
  }
  
  auto term = currentTerm;
#ifdef RAFT_LEADER_ELECTION_DEBUG
  Log_info("[RAFT_ELECTION] server %d (loc %d) starting election term %lu->%lu lastLogIdx=%lu lastLogTerm=%lu prev_vote_for=%d",
           site_id_, loc_id, prev_term, term, lst_idx, lst_term, prev_vote_for);
#endif
  auto sp_quorum = ((RaftCommo *)(this->commo_))->BroadcastVote(par_id,lst_idx,lst_term,loc_id, term );
  sp_quorum->wait(1000000);
  std::lock_guard<std::recursive_mutex> lock1(mtx_);
#ifdef RAFT_LEADER_ELECTION_DEBUG
  Log_info("[RAFT_ELECTION] server %d term %lu vote outcome yes=%d no=%d highest_term_seen=%ld timeout=%d",
           site_id_, term, sp_quorum->n_voted_yes_, sp_quorum->n_voted_no_, sp_quorum->Term(), sp_quorum->timeouted_);
#endif
  if (sp_quorum->yes()) {
    verify(currentTerm >= term);
    if (term != currentTerm) {
#ifdef RAFT_LEADER_ELECTION_DEBUG
      Log_info("[RAFT_ELECTION] server %d abandoning leadership claim because local term advanced to %lu", site_id_, currentTerm);
#endif
      return false;
    }

    // =========================================================================
    // SPECULATIVE VOTING: Initialize specVoters from vote responses
    // =========================================================================
    // These are memory votes - not yet durable
    specVoters_ = sp_quorum->GetSpecVoters();
    specVoters_.insert(site_id_);  // Add self vote

    // Self vote is always durable (we persisted before broadcasting)
    durableVoters_.clear();
    durableVoters_.insert(site_id_);

    // Reset commit indices
    specCommitIndex_ = commitIndex;
    securedLogIndex_ = commitIndex;

    // Clear ack tracking maps for new term
    memoryAcks_.clear();
    durableAcks_.clear();

    // Start as unsecured leader until we receive VoteDurable from quorum
    securedLeader_ = false;

    Log_info("[SPEC-RAFT] Site %d: Won election term %lu - specVoters=%zu durableVoters=%zu",
             site_id_, term, specVoters_.size(), durableVoters_.size());
    // =========================================================================

    // become a leader
    setIsLeader(true) ;
    // verify(currentTerm == term); // [Jetpack] Comment this since in failure recovery test this will fail after experiment end.
    Log_debug("site %d became leader for term %d", site_id_, term);

#ifdef RAFT_LEADER_ELECTION_DEBUG
    Log_info("[RAFT_ELECTION] server %d won election term %lu (votes yes=%d no=%d)",
             site_id_, term, sp_quorum->n_voted_yes_, sp_quorum->n_voted_no_);
#endif

    this->rep_frame_ = this->frame_ ;

    // auto co = ((TxLogServer *)(this))->CreateRepCoord(0);
    // auto empty_cmd = std::make_shared<TpcEmptyCommand>();
    // verify(empty_cmd->kind_ == MarshallDeputy::CMD_TPC_EMPTY);
    // auto sp_m = dynamic_pointer_cast<Marshallable>(empty_cmd);
    // ((CoordinatorRaft*)co)->Submit(sp_m);
    
    if(IsLeader()) {
	  	//for(int i = 0; i < 100; i++) Log_info("wait wait wait");
      Log_debug("vote accepted %d curterm %d", loc_id, currentTerm);
#ifdef RAFT_TEST_CORO
      // Skip JetpackRecovery in test environment to avoid RPC handler issues
#else
      if (JetpackRecoveryEnabled()) {
        JetpackRecoveryEntry(); // Trigger Jetpack recovery on new leader election
      }
#endif
  		req_voting_ = false ;
			return true;
    } else {
      Log_debug("vote rejected %d curterm %d, do rollback", loc_id, currentTerm);
      setIsLeader(false) ;
    	return false;
		}
  } else if (sp_quorum->no()) {
    // become a follower
    Log_debug("site %d requestvote rejected", site_id_);
    setIsLeader(false) ;
#ifdef RAFT_LEADER_ELECTION_DEBUG
    Log_info("[RAFT_ELECTION] server %d lost election term %lu (yes=%d no=%d) highest_term=%ld",
             site_id_, term, sp_quorum->n_voted_yes_, sp_quorum->n_voted_no_, sp_quorum->Term());
#endif
    //reset cur term if new term is higher
    ballot_t new_term = sp_quorum->Term() ;
    if (new_term > currentTerm) {
      auto prev_local_term = currentTerm;
      currentTerm = new_term;
      vote_for_ = INVALID_SITEID;  // Reset vote when advancing to new term

      // CRITICAL: Persist term after observing higher term from election responses
      PersistState(currentTerm, vote_for_, "RequestVote: observed higher term");

      LogTermChange("observed higher term from RequestVote replies", prev_local_term, currentTerm);
    }
  	req_voting_ = false ;
		return false;
  } else {
    Log_debug("vote timeout %d", loc_id);
#ifdef RAFT_LEADER_ELECTION_DEBUG
    Log_info("[RAFT_ELECTION] server %d election timed out term %lu (yes=%d no=%d)",
             site_id_, term, sp_quorum->n_voted_yes_, sp_quorum->n_voted_no_);
#endif
  	req_voting_ = false ;
		return false;
  }
}

void RaftServer::OnRequestVote(const slotid_t& lst_log_idx,
                               const ballot_t& lst_log_term,
                               const siteid_t& can_id,
                               const ballot_t& can_term,
                               ballot_t *reply_term,
                               bool_t *vote_granted,
                               rusty::Function<void()> cb) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  Log_debug("raft receives vote from candidate: %llx", can_id);

  uint64_t cur_term = currentTerm ;
  if( can_term < cur_term)
  {
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, false, std::move(cb)) ;
    return ;
  }

  // has voted to a machine in the same term, vote no
  // CRITICAL FIX: Only reject if we already voted for someone else in this term
  // Standard Raft allows voting for the SAME candidate multiple times (idempotent)
  // and allows voting if we haven't voted yet in this term
  if( can_term == cur_term && vote_for_ != INVALID_SITEID && vote_for_ != can_id )
  {
    Log_debug("site %d vote NO for %d (already voted for %d in term %lu)",
              site_id_, can_id, vote_for_, cur_term);
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, false, std::move(cb)) ;
    return ;
  }

  // If we already voted for this same candidate in this term, vote YES again (idempotent)
  if( can_term == cur_term && vote_for_ == can_id )
  {
    Log_debug("site %d vote YES for %d (already voted for them in term %lu, idempotent)",
              site_id_, can_id, cur_term);
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, true, std::move(cb)) ;
    return ;
  }

  // lstoff starts from 1
  uint32_t lstoff = lastLogIndex - snapidx_ ;

  ballot_t curlstterm = snapterm_ ;
  slotid_t curlstidx = lastLogIndex ;

  if(lstoff > 0 )
  {
    auto log = GetRaftInstance(lstoff) ;
    curlstterm = log->term ;
  }

  Log_debug("vote for lstoff %d, curlstterm %d, curlstidx %d", lstoff, curlstterm, curlstidx  );


  // TODO del only for test
  verify(lstoff == lastLogIndex ) ;

  if( lst_log_term > curlstterm || (lst_log_term == curlstterm && lst_log_idx >= curlstidx) )
  {
    Log_debug("site %d vote for request vote from %d, lastidx %d, lastterm %d", site_id_, can_id, curlstidx, curlstterm);
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, true, std::move(cb)) ;
    return ;
  }

  doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, false, std::move(cb)) ;

}

// ============================================================================
// VoteDurable RPC Handler - Speculative Voting Protocol
// ============================================================================

void RaftServer::OnVoteDurable(const ballot_t& term,
                                const siteid_t& voter_id,
                                bool_t* acknowledged,
                                rusty::Function<void()> cb) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // Reject stale votes from old terms
  if (term != currentTerm) {
    Log_debug("[SPEC-RAFT] Site %d: Ignoring VoteDurable from %d - term mismatch (got %lu, current %lu)",
              site_id_, voter_id, term, currentTerm);
    *acknowledged = false;
    cb();
    return;
  }

  // Only process if we're the leader
  if (!is_leader_) {
    Log_debug("[SPEC-RAFT] Site %d: Ignoring VoteDurable from %d - not leader",
              site_id_, voter_id);
    *acknowledged = false;
    cb();
    return;
  }

  // Add voter to durable voters set
  durableVoters_.insert(voter_id);
  *acknowledged = true;

  Log_info("[SPEC-RAFT] Site %d: Received VoteDurable from %d - durableVoters size=%zu",
           site_id_, voter_id, durableVoters_.size());

  // Check if we've achieved secured leader status
  size_t quorum = (Config::GetConfig()->GetPartitionSize(partition_id_) / 2) + 1;
  if (!securedLeader_ && durableVoters_.size() >= quorum) {
    securedLeader_ = true;
    Log_info("[SPEC-RAFT] Site %d: Became SECURED leader with %zu durable votes (quorum=%zu)",
             site_id_, durableVoters_.size(), quorum);
  }

  cb();
}

// ============================================================================
// AppendEntriesDurable RPC Handler - Speculative Commit Protocol
// ============================================================================

void RaftServer::OnAppendEntriesDurable(const ballot_t& term,
                                         const siteid_t& follower_id,
                                         const uint64_t& lastLogIndex,
                                         bool_t* acknowledged,
                                         rusty::Function<void()> cb) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // Reject stale acks from old terms
  if (term != currentTerm) {
    Log_debug("[SPEC-RAFT] Site %d: Ignoring AppendEntriesDurable from %d - term mismatch (got %lu, current %lu)",
              site_id_, follower_id, term, currentTerm);
    *acknowledged = false;
    cb();
    return;
  }

  // Only process if we're the leader
  if (!is_leader_) {
    Log_debug("[SPEC-RAFT] Site %d: Ignoring AppendEntriesDurable from %d - not leader",
              site_id_, follower_id);
    *acknowledged = false;
    cb();
    return;
  }

  // Add follower to durable acks for all indices up to lastLogIndex
  // We track this for all indices since the follower has durably persisted everything up to lastLogIndex
  for (uint64_t idx = 1; idx <= lastLogIndex; ++idx) {
    durableAcks_[idx].insert(follower_id);
  }
  *acknowledged = true;

  Log_info("[SPEC-RAFT] Site %d: Received AppendEntriesDurable from %d for index=%lu",
           site_id_, follower_id, lastLogIndex);

  // Check if we can advance securedLogIndex
  // Only if we're a secured leader (have durable vote quorum)
  if (securedLeader_) {
    size_t quorum = (Config::GetConfig()->GetPartitionSize(partition_id_) / 2) + 1;

    // Find the highest index with durable ack quorum
    uint64_t newSecuredIndex = securedLogIndex_;
    for (uint64_t idx = securedLogIndex_ + 1; idx <= lastLogIndex && idx <= specCommitIndex_; ++idx) {
      auto it = durableAcks_.find(idx);
      if (it != durableAcks_.end() && it->second.size() >= quorum) {
        newSecuredIndex = idx;
      } else {
        // Stop at first index without quorum (monotonic advance)
        break;
      }
    }

    if (newSecuredIndex > securedLogIndex_) {
      uint64_t oldSecuredLogIndex = securedLogIndex_;
      Log_info("[SPEC-RAFT] Site %d: Advancing securedLogIndex %lu -> %lu",
               site_id_, securedLogIndex_, newSecuredIndex);
      securedLogIndex_ = newSecuredIndex;

      // Phase 5.3: Notify clients with DURABLE status for newly secured entries
      if (lastDurableNotifiedIndex_ < newSecuredIndex) {
        uint64_t notifyFrom = std::max(lastDurableNotifiedIndex_, oldSecuredLogIndex);
        NotifyCallbacks(notifyFrom, newSecuredIndex, CommitStatus::DURABLE);
        lastDurableNotifiedIndex_ = newSecuredIndex;
      }
    }
  }

  // Verify invariants in debug mode
  VerifySpeculativeInvariants();

  cb();
}

// @safe - Calls undeclared Fiber::create_run()
void RaftServer::StartElectionTimer() {
  resetTimer("start election timer");
  last_heartbeat_time_ = Time::now();

  Fiber::create_run([this]() {
    Log_debug("start timer for election") ;

    while(!stop_) {
      // Use dynamic election timeout based on preferred replica role and grace period
      uint64_t election_timeout = GetElectionTimeout();

      // Sleep for a portion of the timeout before checking
      Fiber::sleep(RandomGenerator::rand(HEARTBEAT_INTERVAL * 2, HEARTBEAT_INTERVAL * 4));

      // Retry NotifyRestart for any PENDING peers
      // This handles the case where a peer was partitioned when we restarted
      auto c = commo();
      if (c != nullptr && c->HasPendingNotifyRestart()) {
        Log_debug("[NOTIFY-RESTART-RETRY] Site %d: retrying for pending peers", site_id_);
        c->RetryPendingNotifyRestart();
      }

      auto time_now = Time::now();
      auto time_elapsed = time_now - last_heartbeat_time_;

      // Only log when timeout actually fires or when debugging
      // Log_info("[ELECTION_TIMER] Site %d: checking - is_leader=%d time_elapsed=%lu election_timeout=%lu last_hb_time=%lu",
      //          site_id_, IsLeader(), time_elapsed, election_timeout, last_heartbeat_time_);

      if (!IsLeader() && time_elapsed > election_timeout) {
        Log_info("[ELECTION_TIMER] Site %d: TIMEOUT FIRED - starting election (elapsed=%lu > timeout=%lu)",
                 site_id_, time_elapsed, election_timeout);

        // ask to vote
        req_voting_ = true ;
        Log_info("[ELECTION_START] Site %d: TRIGGERING REQUESTVOTE - time_elapsed=%lu > timeout=%lu last_hb=%lu current_term=%lu vote_for=%d",
                 site_id_, time_elapsed, election_timeout, last_heartbeat_time_, currentTerm, vote_for_);
        // CRITICAL: Check stop_ before calling RequestVote() to prevent
        // calling through collapsed vtable after object destruction
        if (stop_) return;
        RequestVote() ;
        while(req_voting_) {
          Fiber::sleep(wait_int_);
          if(stop_) return ;
        }
      }
    }
  });
}

bool RaftServer::Start(shared_ptr<Marshallable> &cmd,
                       uint64_t *index,
                       uint64_t *term,
                       slotid_t slot_id,
                       ballot_t ballot) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // #ifndef RAFT_TEST_CORO
  // if (!heartbeat_setup_) {
  //   heartbeat_setup_ = true;
  //   if (heartbeat_) {
  //     Log_debug("starting heartbeat loop at site %d", site_id_);
  //     Fiber::create_run([this](){
  //       this->HeartbeatLoop(); 
  //     });
  //     // Start election timeout loop
  //     Log_info("!!!!!!! if (failover_)");
  //     if (failover_) {
  //       Fiber::create_run([this](){
  //         StartElectionTimer(); 
  //       });
  //     }
  //   }
  // }
  // #endif
  if (!IsLeader()) {
    *index = 0;
    *term = 0;
    return false;
  }
  SetLocalAppend(cmd, term, index, slot_id, ballot);
  // SetLocalAppend returns the old lastLogIndex value, but Start returns the
  // index of the newly appended instance
  verify(lastLogIndex == (*index) + 1);
  *index = lastLogIndex;
  Log_debug("Start(): ldr=%d index=%ld term=%ld", loc_id_, *index, *term);
  return true;
}

/* NOTE: same as ReceiveAppend */
/* NOTE: broadcast send to all of the host even to its own server 
 * should we exclude the execution of this function for leader? */
void RaftServer::OnAppendEntries(const slotid_t slot_id,
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
                                 bool trigger_election_now) {
  mtx_.lock();

  bool term_ok = (leaderCurrentTerm >= this->currentTerm);
  bool index_ok = (leaderPrevLogIndex <= this->lastLogIndex);
  uint64_t local_prev_term = 0;
  if (leaderPrevLogIndex > 0 && leaderPrevLogIndex <= this->lastLogIndex) {
      local_prev_term = GetRaftInstance(leaderPrevLogIndex)->term;
  }
  bool prev_term_ok = (leaderPrevLogIndex == 0 || local_prev_term == leaderPrevLogTerm);

  // Only log rejections or when cmd is present (actual log entries)
  if (!term_ok || !index_ok || !prev_term_ok || cmd != nullptr) {
  }

  // CRITICAL FIX: Reset timer if we hear from a current-term leader, even if log conflicts
  // This prevents followers with divergent logs from constantly starting elections
  // while the leader is trying to repair their log via backtracking
  if (term_ok) {
      resetTimer("AppendEntries from current-term leader");
      if (leaderCurrentTerm > this->currentTerm) {
          auto prev_term = currentTerm;
          currentTerm = leaderCurrentTerm;
          vote_for_ = INVALID_SITEID;  // Reset vote when advancing to new term

          // CRITICAL: Persist term before accepting any entries from new leader
          PersistState(currentTerm, vote_for_, "OnAppendEntries: new leader term");

          LogTermChange("AppendEntries leader term is newer", prev_term, currentTerm, leaderSiteId);
          Log_debug("server %d, set to be follower", loc_id_ ) ;
          setIsLeader(false) ;
          // PersistState() already called above - no need for duplicate persistence
      }
  }

  if (term_ok && index_ok && prev_term_ok) {
      Log_debug("refresh timer on appendentry");

      // // Update follower's view to track the current leader
      // if (!IsLeader() && leaderSiteId != INVALID_SITEID) {
      //     int prev_leader = new_view_.GetLeader();
      //     old_view_ = new_view_;
      //     int n_replicas = Config::GetConfig()->GetPartitionSize(partition_id_);
      //     new_view_ = View(n_replicas, leaderSiteId, leaderCurrentTerm);
      //     Log_info("[RAFT_VIEW_FOLLOWER] Server %d observed leader change %d->%d term=%lu prev_term=%lu",
      //              site_id_, prev_leader, leaderSiteId, leaderCurrentTerm, currentTerm);
      // }

      // ==================================================================
      // SPECULATIVE REPLICATION: Append to memory, respond immediately,
      // then persist asynchronously and send AppendEntriesDurable
      // ==================================================================

      // Capture state for async persistence before modifying in-memory state
      std::vector<std::pair<slotid_t, std::shared_ptr<RaftData>>> entries_to_persist;
      uint64_t log_index_for_durable_ack = 0;

      if (cmd != nullptr) {
#ifndef RAFT_BATCH_OPTIMIZATION
        lastLogIndex = leaderPrevLogIndex + 1;
        auto instance = GetRaftInstance(lastLogIndex);
        instance->log_ = cmd;
        instance->term = leaderNextLogTerm;

        // Capture entry for async persistence
        entries_to_persist.push_back({lastLogIndex, instance});
        log_index_for_durable_ack = lastLogIndex;
#endif
#ifdef RAFT_BATCH_OPTIMIZATION
        auto cmds = dynamic_pointer_cast<TpcBatchCommand>(cmd);
        int cnt = 0;
        for (shared_ptr<TpcCommitCommand>& c: cmds->cmds_) {
          cnt++;
          lastLogIndex = leaderPrevLogIndex + cnt;
          auto instance = GetRaftInstance(lastLogIndex);
          instance->log_ = c;
          instance->term = dynamic_pointer_cast<TpcCommitCommand>(c)->term;

          // Capture entry for async persistence
          entries_to_persist.push_back({lastLogIndex, instance});
        }
        log_index_for_durable_ack = lastLogIndex;  // Highest index in batch
#endif
      }

      // update commitIndex and trigger log application
      bool need_apply = false;
      if (leaderCommitIndex > commitIndex) {
        commitIndex = std::min(leaderCommitIndex, lastLogIndex);
        verify(lastLogIndex >= commitIndex);

        need_apply = true;
      }

      *followerAppendOK = 1;
      *followerCurrentTerm = this->currentTerm;
      *followerLastLogIndex = this->lastLogIndex;

      // Capture state needed for async persistence thread
      ballot_t term_copy = currentTerm;
      siteid_t follower_id_copy = site_id_;
      siteid_t leader_id_copy = leaderSiteId;
      parid_t par_id_copy = partition_id_;
      uint64_t commit_index_copy = commitIndex;

      // CRITICAL FIX: Release mutex before applying logs!
      // This allows concurrent AppendEntries to be processed
      // while we're applying the current batch
      mtx_.unlock();

      if (need_apply) {
        applyLogs();  // Now called WITHOUT holding the mutex!
      }

      // ==================================================================
      // PERSISTENCE: Either async (speculative) or sync (traditional)
      // ==================================================================
      if (async_persistence_) {
        // ASYNC MODE (speculative): Start async persistence and send durable ack later
        if (!entries_to_persist.empty()) {
          // Capture entries by move to avoid dangling references
          // Track async persistence thread (joined in destructor to prevent UAF)
          {
            std::lock_guard<std::mutex> lk(async_threads_mtx_);
            // Prune completed threads to prevent unbounded accumulation
            async_threads_.erase(
              std::remove_if(async_threads_.begin(), async_threads_.end(),
                [](auto& entry) {
                  if (entry.second->load(std::memory_order_acquire)) {
                    if (entry.first.joinable()) entry.first.join();
                    return true;
                  }
                  return false;
                }),
              async_threads_.end());
            auto done = std::make_shared<std::atomic<bool>>(false);
            async_threads_.emplace_back(
              std::thread([this, entries = std::move(entries_to_persist),
                         log_index_for_durable_ack, term_copy, follower_id_copy,
                         leader_id_copy, par_id_copy, commit_index_copy, done]() {
              // Persist all log entries
              for (const auto& entry : entries) {
                PersistLogEntry(entry.first, *entry.second, "OnAppendEntries: async follower entry");
              }

              // Also persist commit index (async is fine for commit index)
              PersistCommitIndex(commit_index_copy, "OnAppendEntries: async follower commit");

              // Send AppendEntriesDurable RPC to leader
              auto c = commo();
              if (c != nullptr) {
                c->SendAppendEntriesDurable(leader_id_copy, par_id_copy, term_copy,
                                            follower_id_copy, log_index_for_durable_ack);
              }
              done->store(true, std::memory_order_release);
            }), done);
          }
        }
      } else {
        // SYNC MODE (traditional): Persist entries synchronously before returning
        // No separate AppendEntriesDurable RPC needed - the ack implies durability
        if (!entries_to_persist.empty()) {
          for (const auto& entry : entries_to_persist) {
            PersistLogEntry(entry.first, *entry.second, "OnAppendEntries: sync follower entry");
          }
          PersistCommitIndex(commit_index_copy, "OnAppendEntries: sync follower commit");
        }
      }

      // Re-acquire mutex before returning (to handle remaining code safely)
      mtx_.lock();

#ifndef RAFT_TEST_CORO
      if (cmd != nullptr) {
        if (cmd->kind_ == MarshallDeputy::CMD_TPC_COMMIT){
          auto p_cmd = dynamic_pointer_cast<TpcCommitCommand>(cmd);
          auto sp_vec_piece = dynamic_pointer_cast<VecPieceData>(p_cmd->cmd_)->sp_vec_piece_data_;
          
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
          // de->wait();
        } else {
          int value = -1;
          // auto de = IO::write(filename, &value, sizeof(int), 1);
          // de->wait();
        }
      }
#endif
    }
    else {
        Log_info("[APPEND_REJECT] Site %d rejecting AppendEntries from leader %d - term_ok=%d index_ok=%d prev_term_ok=%d (leaderTerm=%lu myTerm=%lu prevIdx=%lu myLastIdx=%lu local_prev_term=%lu)",
                 site_id_, leaderSiteId, term_ok, index_ok, prev_term_ok, leaderCurrentTerm, currentTerm,
                 leaderPrevLogIndex, lastLogIndex, local_prev_term);
        *followerAppendOK = 0;
        *followerCurrentTerm = this->currentTerm;
        *followerLastLogIndex = this->lastLogIndex;
    }

/*if (rand() % 1000 == 0) {
	usleep(25*1000);
}*/

    // ============================================================================
    // PIGGYBACKED LEADERSHIP TRANSFER: Handle trigger_election_now flag
    // ============================================================================
    // The leader sends trigger_election_now=true to ALL replicas during transfer.
    // How we handle it depends on whether we're the preferred replica or not.
    if (trigger_election_now) {
        if (AmIPreferredLeader()) {
            // I'm the PREFERRED replica - start election (if not already leader)
            if (!IsLeader()) {
                Log_info("[PIGGYBACKED-TRANSFER] Site %d (preferred): Received transfer signal from leader %d - will start election after 30ms",
                         site_id_, leaderSiteId);

                // Wait before starting election to allow old leader's heartbeats
                // to reach other replicas. This prevents election storms.
                Fiber::create_run([this]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    // CRITICAL: Check stop_ before calling RequestVote() to prevent
                    // calling through collapsed vtable after object destruction
                    if (stop_) return;
                    RequestVote();
                });
            } else {
                Log_info("[PIGGYBACKED-TRANSFER] Site %d (preferred): Received transfer signal but already leader - ignoring",
                         site_id_);
            }
        } else {
            // I'm a NON-PREFERRED replica - just log and do nothing
            Log_info("[PIGGYBACKED-TRANSFER] Site %d (non-preferred): Received transfer signal (preferred=%d)",
                     site_id_, preferred_leader_site_id_);
        }
    }

    mtx_.unlock();
    cb();
}

void RaftServer::removeCmd(slotid_t slot) {
  auto cmd = dynamic_pointer_cast<TpcCommitCommand>(raft_logs_[slot]->log_);
  if (!cmd)
    return;
  tx_sched_->DestroyTx(cmd->tx_id_);
  raft_logs_.erase(slot);
}

void RaftServer::RegisterLeaderChangeCallback(std::function<void(bool)> cb) {
  leader_change_cb_ = std::move(cb);
}

void RaftServer::OnTimeoutNow(const uint64_t leaderTerm,
                               const siteid_t leaderSiteId,
                               uint64_t *followerTerm,
                               bool_t *success,
                               rusty::Function<void()> cb) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  *followerTerm = currentTerm;
  *success = false;

  // ============================================================================
  // Edge Case 0: Server shutting down
  // ============================================================================
  if (stop_) {
    Log_info("[TIMEOUT-NOW] Site %d: Ignoring TimeoutNow - server shutting down", site_id_);
    cb();
    return;
  }

  // ============================================================================
  // Edge Case 1: Stale TimeoutNow from old term
  // ============================================================================
  if (leaderTerm < currentTerm) {
    Log_info("[TIMEOUT-NOW] Site %d: Ignoring stale TimeoutNow from leader %d (leader_term=%lu < my_term=%lu)",
             site_id_, leaderSiteId, leaderTerm, currentTerm);
    cb();
    return;
  }

  // ============================================================================
  // Edge Case 1b: Leader is ahead of us - update term
  // ============================================================================
  if (leaderTerm > currentTerm) {
    Log_info("[TIMEOUT-NOW] Site %d: Leader %d has higher term (%lu > %lu) - updating term and stepping down",
             site_id_, leaderSiteId, leaderTerm, currentTerm);

    currentTerm = leaderTerm;
    vote_for_ = INVALID_SITEID;  // Reset vote for new term

    // CRITICAL: Persist term before responding to TimeoutNow
    PersistState(currentTerm, vote_for_, "OnTimeoutNow: leader higher term");

    if (is_leader_) {
      setIsLeader(false);  // Step down from leadership
    }

    *followerTerm = currentTerm;
  }

  // ============================================================================
  // Edge Case 2: Already leader
  // ============================================================================
  if (is_leader_) {
    Log_info("[TIMEOUT-NOW] Site %d: Ignoring TimeoutNow from leader %d - already leader in term %lu",
             site_id_, leaderSiteId, currentTerm);
    *success = true;  // Success = already leader (goal achieved)
    cb();
    return;
  }

  // ============================================================================
  // Edge Case 3: Currently candidate (already in election)
  // ============================================================================
  if (req_voting_) {
    Log_info("[TIMEOUT-NOW] Site %d: Ignoring TimeoutNow from leader %d - already requesting votes (term=%lu)",
             site_id_, leaderSiteId, currentTerm);
    *success = true;  // Success = already trying to become leader
    cb();
    return;
  }

  // ============================================================================
  // Edge Case 4: We're transferring leadership (stepping down)
  // ============================================================================
  if (transferring_leadership_) {
    Log_info("[TIMEOUT-NOW] Site %d: Ignoring TimeoutNow from leader %d - currently transferring leadership",
             site_id_, leaderSiteId);
    cb();
    return;
  }

  // ============================================================================
  // Valid TimeoutNow - Start Election Immediately
  // ============================================================================
  Log_info("[TIMEOUT-NOW] *** Site %d: Received TimeoutNow from leader %d (term=%lu) - STARTING ELECTION IMMEDIATELY ***",
           site_id_, leaderSiteId, leaderTerm);

  // Start election immediately (bypass random timeout)
  // This will increment term and send RequestVote RPCs
  bool election_started = RequestVote();

  if (election_started) {
    *success = true;
    Log_info("[TIMEOUT-NOW] Site %d: Election started successfully (new_term=%lu)",
             site_id_, currentTerm);
  } else {
    *success = false;
    Log_warn("[TIMEOUT-NOW] Site %d: Failed to start election",
             site_id_);
  }

  cb();
}

void RaftServer::StopLeadershipTransferMonitoring() {
  leadership_monitor_stop_ = true;

  // Detach the monitor thread so it can exit gracefully without deadlock
  // The thread will see leadership_monitor_stop_ and exit on its own
  if (leadership_monitor_thread_.joinable()) {
    Log_debug("[LEADERSHIP-TRANSFER] Site %d: Detaching monitor thread (will exit on its own)", site_id_);
    leadership_monitor_thread_.detach();
  }
}

// @unsafe
void RaftServer::StartLeadershipTransferMonitoring() {
  if (leadership_monitor_stop_.load()) {
    leadership_monitor_stop_ = false;
  }

  // Stop any existing monitor thread
  if (leadership_monitor_thread_.joinable()) {
    leadership_monitor_stop_ = true;
    leadership_monitor_thread_.join();
    leadership_monitor_stop_ = false;
  }

  Log_info("[LEADERSHIP-TRANSFER] Site %d: Starting leadership transfer monitoring thread",
           site_id_);

  // Launch monitoring thread
  leadership_monitor_thread_ = std::thread([this]() {
    const uint64_t CHECK_INTERVAL_MS = 1000;  // Check every 1 second
    const uint64_t MIN_STABLE_TIME_US = 500000; // Wait 0.5 seconds (in microseconds) after becoming leader before transferring

    uint64_t became_leader_time = Time::now();

    Log_info("[LEADERSHIP-TRANSFER] Site %d: Monitor thread started (will check every %lums)",
             site_id_, CHECK_INTERVAL_MS);

    while (true) {
      std::this_thread::sleep_for(std::chrono::milliseconds(CHECK_INTERVAL_MS));

      bool should_transfer = false;

      // Critical section: check shared state with proper locking
      {
        std::lock_guard<std::recursive_mutex> lock(mtx_);

        // Check if we should stop monitoring
        if (leadership_monitor_stop_) {
          Log_info("[LEADERSHIP-TRANSFER] Site %d: Monitor stop requested, exiting", site_id_);
          break;
        }

        // Check if server is shutting down
        if (stop_) {
          Log_info("[LEADERSHIP-TRANSFER] Site %d: Server shutting down, exiting monitor", site_id_);
          break;
        }

        // Check if we're still leader
        if (!is_leader_) {
          Log_info("[LEADERSHIP-TRANSFER] Site %d: No longer leader, exiting monitor", site_id_);
          break;
        }

        // Check if we became preferred (no longer need to transfer)
        if (AmIPreferredLeader()) {
          Log_info("[LEADERSHIP-TRANSFER] Site %d: I am now preferred leader, exiting monitor",
                   site_id_);
          break;
        }

        // Wait for cluster to stabilize after becoming leader
        uint64_t time_as_leader = Time::now() - became_leader_time;
        if (time_as_leader < MIN_STABLE_TIME_US) {
          continue;
        }

        // Check if we should transfer leadership
        if (ShouldTransferLeadership()) {
          Log_info("[LEADERSHIP-TRANSFER] Site %d: Conditions met, initiating transfer NOW",
                   site_id_);
          should_transfer = true;
        }
      } // End critical section - LOCK RELEASED

      // Call InitiateLeadershipTransfer WITHOUT holding lock to avoid deadlock
      if (should_transfer) {
        InitiateLeadershipTransfer();
        break;  // Exit after transferring
      }
    }

    Log_info("[LEADERSHIP-TRANSFER] Site %d: Monitor thread exiting", site_id_);
  });
}

void RaftServer::EnsureSetup() {
  if (heartbeat_setup_) {
    return;
  }
  heartbeat_setup_ = true;
  Setup();
}

bool RaftServer::ShouldTransferLeadership() {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // Must be leader
  if (!is_leader_) {
    return false;
  }

  // Must not be preferred (preferred leaders don't transfer)
  if (AmIPreferredLeader()) {
    return false;
  }

  // Must have a preferred leader configured
  if (preferred_leader_site_id_ == INVALID_SITEID) {
    return false;
  }

  // Already transferring
  if (transferring_leadership_) {
    return false;
  }

  // Check if preferred replica is in our peer list
  auto it = match_index_.find(preferred_leader_site_id_);
  if (it == match_index_.end()) {
    Log_debug("[LEADERSHIP-TRANSFER] Site %d: Preferred replica %d not in peer list",
              site_id_, preferred_leader_site_id_);
    return false;
  }

  // Check if preferred replica is caught up
  slotid_t preferred_match_index = it->second;
  bool is_caught_up = (preferred_match_index >= commitIndex);

  if (!is_caught_up) {
    Log_debug("[LEADERSHIP-TRANSFER] Site %d: Preferred replica %d not caught up (match=%lu, commit=%lu)",
              site_id_, preferred_leader_site_id_, preferred_match_index, commitIndex);
    return false;
  }

  Log_info("[LEADERSHIP-TRANSFER] Site %d: Preferred replica %d is caught up! Ready to transfer",
           site_id_, preferred_leader_site_id_);
  return true;
}

void RaftServer::InitiateLeadershipTransfer() {
  // Check if server is shutting down
  if (stop_) {
    Log_info("[LEADERSHIP-TRANSFER] Site %d: Aborting transfer - server shutting down", site_id_);
    return;
  }

  siteid_t target_site_id;
  parid_t par_id;
  uint64_t current_term_snapshot;

  // ============================================================================
  // PIGGYBACKED LEADERSHIP TRANSFER (Approach 2)
  // ============================================================================

  {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    target_site_id = preferred_leader_site_id_;
    par_id = partition_id_;
    current_term_snapshot = currentTerm;

    // Mark transfer as in progress - this will suppress elections on non-preferred replicas
    transferring_leadership_ = true;
    leadership_transfer_start_time_ = Time::now();

    Log_info("[LEADERSHIP-TRANSFER] Site %d (partition %d): Starting transfer to site %d",
             site_id_, partition_id_, target_site_id);

    // Send heartbeats to ALL replicas
    for (auto& kv : match_index_) {
      siteid_t peer_site_id = kv.first;

      if (peer_site_id == site_id_) {
        continue;
      }

      slotid_t slot = commitIndex;
      ballot_t ballot = 0;
      uint64_t prevLogIndex = next_index_[peer_site_id] - 1;
      uint64_t prevLogTerm = 0;

      if (prevLogIndex > 0 && prevLogIndex < logs_.size()) {
        prevLogTerm = logs_[prevLogIndex]->term;
      }

      // Send trigger_election_now=true to ALL replicas during transfer:
      // - Preferred replica: Will start election
      // - Non-preferred replicas: Will activate election suppression
      bool trigger_election = true;  // Signal transfer to ALL replicas

      commo()->SendAppendEntries(
        peer_site_id,
        partition_id_,
        slot,
        ballot,
        true,
        site_id_,
        currentTerm,
        prevLogIndex,
        prevLogTerm,
        commitIndex,
        nullptr,
        0,
        trigger_election
      );
    }
  }

  // Sleep briefly to ensure the RPC library has time to send the packets.
  // Note: The preferred replica will wait 30ms before starting election,
  // so this sleep is just to ensure packet transmission, not to delay step-down.
  // We will likely step down earlier when we receive RequestVote from preferred replica.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // ============================================================================
  // Step Down from Leadership Immediately
  // ============================================================================
  // With piggybacked approach, we step down immediately after sending the message.
  // The preferred replica will:
  // 1. Reset its election timeout (from the heartbeat)
  // 2. Start election immediately (from the trigger_election_now flag)
  // 3. Win the election (since it's caught up and has all committed entries)
  //
  // Other replicas will:
  // 1. Reset their election timeouts (from normal heartbeats)
  // 2. Not start elections (timers reset)
  // 3. Vote for preferred replica when it requests votes
  {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    Log_info("[LEADERSHIP-TRANSFER] Site %d: Stepping down from leadership (current_term=%lu)",
             site_id_, currentTerm);

    // Become follower - this stops heartbeats and allows new leader to emerge
    setIsLeader(false);

    Log_info("[LEADERSHIP-TRANSFER] Site %d: Leadership transfer complete - now follower",
             site_id_);
  }
}

// ============================================================================
// SPECULATIVE REPLICATION STATE (Phase 1.1)
// ============================================================================

void RaftServer::ResetSpeculativeState() {
  // Note: caller must hold mtx_ lock

  if (is_leader_) {
    // On becoming leader: initialize with self votes
    specVoters_.clear();
    specVoters_.insert(site_id_);  // voted for self
    durableVoters_.clear();
    durableVoters_.insert(site_id_);  // self vote is always durable

    // Reset commit indices to current commitIndex (from previous term)
    securedLogIndex_ = commitIndex;
    specCommitIndex_ = commitIndex;

    // Leader starts unsecured until durable vote quorum is achieved
    securedLeader_ = false;

    Log_info("[SPEC-RAFT] Site %d: Reset speculative state as new leader - "
             "specVoters={%d} durableVoters={%d} securedLogIndex=%lu specCommitIndex=%lu",
             site_id_, site_id_, site_id_, securedLogIndex_, specCommitIndex_);
  } else {
    // On stepping down: clear all speculative state
    specVoters_.clear();
    durableVoters_.clear();
    securedLogIndex_ = 0;
    specCommitIndex_ = 0;
    securedLeader_ = false;

    Log_info("[SPEC-RAFT] Site %d: Cleared speculative state (stepped down)",
             site_id_);
  }

  // Clear ack tracking maps
  memoryAcks_.clear();
  durableAcks_.clear();

  // Reset callback notification tracking
  // Note: We don't clear pendingCallbacks_ here because:
  // - On becoming leader: there shouldn't be any pending callbacks yet
  // - On stepping down: NotifyRollback() handles clearing after notification
  lastSpecNotifiedIndex_ = commitIndex;  // Don't re-notify already-committed entries
  lastDurableNotifiedIndex_ = commitIndex;
}

void RaftServer::VerifySpeculativeInvariants() const {
  // Invariant 1: securedLogIndex <= specCommitIndex <= lastLogIndex
  if (securedLogIndex_ > specCommitIndex_) {
    Log_error("[SPEC-RAFT] INVARIANT VIOLATION: securedLogIndex (%lu) > specCommitIndex (%lu)",
              securedLogIndex_, specCommitIndex_);
    verify(securedLogIndex_ <= specCommitIndex_);
  }

  if (specCommitIndex_ > lastLogIndex) {
    Log_error("[SPEC-RAFT] INVARIANT VIOLATION: specCommitIndex (%lu) > lastLogIndex (%lu)",
              specCommitIndex_, lastLogIndex);
    verify(specCommitIndex_ <= lastLogIndex);
  }

  // Note (Phase 6): durableVoters ⊆ specVoters is NOT strictly enforced after crashes.
  // A crashed node loses its memory vote but keeps its durable vote on disk.
  // This is expected behavior, not an invariant violation.
  //
  // Key insight: |durableVoters| >= quorum is sufficient for securedLeader = true.
  // Once durable quorum is reached, specVoters quorum is no longer required.
  // See docs/dev/phase6_relax_invariant_plan.md for full safety argument.

  Log_debug("[SPEC-RAFT] Site %d: Invariants OK - securedLogIndex=%lu specCommitIndex=%lu lastLogIndex=%lu",
            site_id_, securedLogIndex_, specCommitIndex_, lastLogIndex);
}

// ============================================================================
// OnPeerRestart - Handle speculative state invalidation on peer restart
// ============================================================================

void RaftServer::OnPeerRestart(siteid_t restarted_site_id) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // Only process if we're the leader
  if (!is_leader_) {
    Log_debug("[SPEC-RAFT] Site %d: Ignoring peer restart from %d - not leader",
              site_id_, restarted_site_id);
    return;
  }

  Log_info("[SPEC-RAFT] Site %d: Handling peer restart from site %d",
           site_id_, restarted_site_id);

  // Remove from specVoters (their memory vote is no longer reliable)
  size_t removed_from_voters = specVoters_.erase(restarted_site_id);
  if (removed_from_voters > 0) {
    Log_info("[SPEC-RAFT] Site %d: Removed site %d from specVoters (now size=%zu)",
             site_id_, restarted_site_id, specVoters_.size());
  }

  // Remove from memoryAcks for unsecured entries only
  // Entries at or below securedLogIndex are already durably committed,
  // so removing the restarted server doesn't affect their status
  size_t entries_affected = 0;
  for (auto& entry : memoryAcks_) {
    uint64_t idx = entry.first;
    if (idx > securedLogIndex_) {
      if (entry.second.erase(restarted_site_id) > 0) {
        entries_affected++;
      }
    }
  }
  if (entries_affected > 0) {
    Log_info("[SPEC-RAFT] Site %d: Removed site %d from memoryAcks for %zu unsecured entries",
             site_id_, restarted_site_id, entries_affected);
  }

  // Note: We don't remove from durableVoters or durableAcks because:
  // 1. durableVoters represents votes that were persisted to disk BEFORE the crash
  //    - If the vote was durable, it survives the crash
  //    - If it wasn't durable, it was never in durableVoters
  // 2. durableAcks represents entries that were persisted to disk
  //    - Same logic: durable acks survive crashes by definition

  // Check if we need to become secured or step down
  // Phase 6: Relaxed invariant - durableVoters and specVoters are independent after crashes
  if (!securedLeader_ && is_leader_) {
    size_t quorum = (Config::GetConfig()->GetPartitionSize(partition_id_) / 2) + 1;

    // NEW (Phase 6.4.1): Check if durable quorum is sufficient for secured status
    // Note: site_id_ is already in durableVoters_ (inserted by ResetSpeculativeState
    // or RequestElection), so no +1 needed. This matches OnVoteDurable() at line 1417.
    size_t durable_vote_count = durableVoters_.size();
    if (durable_vote_count >= quorum) {
      // We have durable quorum - become secured leader
      // Safety: durableVoters have votedFor=us on disk, can't vote for others in this term
      securedLeader_ = true;
      Log_info("[SPEC-RAFT] Site %d: Became secured via durable quorum (%zu/%zu) "
               "despite spec quorum loss (specVoters=%zu)",
               site_id_, durable_vote_count, quorum, specVoters_.size());
    } else {
      // No durable quorum yet - check speculative quorum
      // Note: site_id_ is already in specVoters_ (inserted by ResetSpeculativeState
      // or RequestElection), so no +1 needed.
      size_t vote_count = specVoters_.size();
      if (vote_count < quorum) {
        // No durable quorum AND no speculative quorum - must step down
        Log_info("[SPEC-RAFT] Site %d: Lost both spec quorum (%zu/%zu) and durable quorum (%zu/%zu) - stepping down",
                 site_id_, vote_count, quorum, durable_vote_count, quorum);
        stepDown(StepDownReason::UnsecuredFailure);
        return;  // Don't verify invariants after stepping down
      }
    }
  }

  VerifySpeculativeInvariants();
}

// ============================================================================
// stepDown - Central leader step-down function
// ============================================================================

static const char* StepDownReasonToString(StepDownReason reason) {
  switch (reason) {
    case StepDownReason::UnsecuredFailure: return "UnsecuredFailure";
    case StepDownReason::SecuredFailure: return "SecuredFailure";
    case StepDownReason::HigherTerm: return "HigherTerm";
    default: return "Unknown";
  }
}

void RaftServer::stepDown(StepDownReason reason) {
  // Must be called with mtx_ held (caller's responsibility)
  // Most callers already hold the lock

  Log_info("[SPEC-RAFT] Site %d: Stepping down as leader (reason=%s, term=%lu, "
           "securedLeader=%d, specVoters=%zu, durableVoters=%zu)",
           site_id_, StepDownReasonToString(reason), currentTerm,
           securedLeader_, specVoters_.size(), durableVoters_.size());

  // TODO (future): Notify pending clients based on reason
  // The client notification infrastructure is deferred to a future phase.
  // For now, just log the step-down reason.
  //
  // Future implementation outline:
  // if (reason == StepDownReason::UnsecuredFailure) {
  //     // All current-term entries are suspect
  //     notifyClientsRollback(commitIndex + 1, lastLogIndex);
  // } else if (reason == StepDownReason::SecuredFailure) {
  //     // Only unsecured entries are suspect
  //     notifyClientsRollback(securedLogIndex_ + 1, specCommitIndex_);
  // }
  // // HigherTerm: no automatic rollback - entries may still commit

  // Reset speculative state
  // This clears specVoters_, durableVoters_, etc.
  ResetSpeculativeState();

  // Transition to follower state
  // This handles view updates, callback notifications, etc.
  setIsLeader(false);

  // Reset election timer
  // Important: Give other servers time to elect a new leader
  resetTimer("stepDown");

  Log_info("[SPEC-RAFT] Site %d: Step-down complete, now follower", site_id_);

  // Notify pending callbacks of rollback (Phase 5.3)
  NotifyRollback();
}

// ============================================================================
// CLIENT NOTIFICATION CALLBACKS (Phase 5.3)
// ============================================================================

void RaftServer::RegisterCommitCallback(uint64_t index,
                                        std::function<void(CommitStatus)> callback) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // If already speculatively committed, invoke immediately
  if (index <= specCommitIndex_) {
    Log_debug("[SPEC-CALLBACK] Index %lu already spec-committed, notifying SPECULATIVE",
              index);
    callback(CommitStatus::SPECULATIVE);
  }

  // If already durably committed, invoke immediately
  if (securedLeader_ && index <= securedLogIndex_) {
    Log_debug("[SPEC-CALLBACK] Index %lu already durable-committed, notifying DURABLE",
              index);
    callback(CommitStatus::DURABLE);
    return;  // No need to track - already fully committed
  }

  // Store callback for future notification
  pendingCallbacks_[index] = std::move(callback);
  Log_debug("[SPEC-CALLBACK] Registered callback for index %lu", index);
}

void RaftServer::NotifyCallbacks(uint64_t from, uint64_t to, CommitStatus status) {
  // Note: Caller must hold mtx_
  // Notify callbacks for indices in (from, to]

  for (uint64_t idx = from + 1; idx <= to; ++idx) {
    auto it = pendingCallbacks_.find(idx);
    if (it != pendingCallbacks_.end()) {
      Log_debug("[SPEC-CALLBACK] Notifying index %lu with status %d",
                idx, static_cast<int>(status));
      it->second(status);

      // If DURABLE, remove callback (fully committed)
      if (status == CommitStatus::DURABLE) {
        pendingCallbacks_.erase(it);
      }
    }
  }
}

void RaftServer::NotifyRollback() {
  // Note: Caller must hold mtx_
  // Notify all pending callbacks above securedLogIndex_ with ROLLEDBACK

  Log_info("[SPEC-CALLBACK] Notifying rollback for %zu pending callbacks (securedLogIndex=%lu)",
           pendingCallbacks_.size(), securedLogIndex_);

  for (auto& [idx, callback] : pendingCallbacks_) {
    if (idx > securedLogIndex_) {
      Log_debug("[SPEC-CALLBACK] Notifying index %lu with ROLLEDBACK", idx);
      callback(CommitStatus::ROLLEDBACK);
    }
  }

  // Clear all pending callbacks after notification
  pendingCallbacks_.clear();

  // Reset notification tracking
  lastSpecNotifiedIndex_ = 0;
  lastDurableNotifiedIndex_ = 0;
}

} // namespace janus
